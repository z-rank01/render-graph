# Render Graph 架构与实现笔记

> as-built，2026-08。本文以当前代码为准（`include/render_graph/`、`src/core/`、`src/backend/vulkan/`），
> 作为深入阅读源码时的导航。README 描述能力边界，本文描述"它是怎么跑起来的"。
> 图示统一使用 ASCII（`-->`），图内文字保持英文以保证等宽对齐。

## 1. 分层总览

```text
Host (engine / samples / tests)
  |
  |  public contract only: include/render_graph/
  |  (handles, descs, rows, recipes, diagnostics -- no Vk types)
  v
+--------------------------------------------------------+
| render_graph::core        (src/core, compiled library) |
|   compile_graph(request) --> compiled_graph_plan       |
|   free-function phases, compiler_state, DAG            |
+--------------------------------------------------------+
  |  compiled_graph_plan (API-agnostic rows)
  v
+--------------------------------------------------------+
| render_graph::vulkan      (src/backend/vulkan)         |
|   vk_runtime (device/swapchain/bindless/pipeline/VMA)  |
|   vk_graph_executor (physical resources, barriers)     |
|   acquire -> realize -> record -> submit -> present    |
+--------------------------------------------------------+
  |
  v
Vulkan 1.3 driver (Synchronization2, Dynamic Rendering, VMA)
```

- Core 不含任何原生 API 类型；backend-specific 能力校验通过显式 function table
  （`resource_validation_api` / `resource_description_api` / `resource_allocation_api`）注入。
- `render_graph::core` 是真实编译库（STATIC，可选 SHARED），不是 header-only target。
- DX12 / Metal 目前只有 lowering contract 与 fake tests，见 §11。

## 2. 目录与 target

```text
include/render_graph/    -- public contract (installable, single source of truth)
  compiler.h             -- compile_graph request/response, compiled rows
  render_device.h        -- render_device ABI, resource_change_batch, frame_plan
  resource.h             -- typed_handle, access descs, ranges, lifetime
  resource_types.h       -- format/desc/capabilities/validation injection
  barrier.h              -- synchronization plan (CSR) + backend barrier rows
  raster.h               -- raster pass / attachment contract
  submission.h           -- queue batches, timeline waits, ownership transfer
  compile_result.h       -- compile_result / compile_diagnostic (24 codes)
  diagnostic.h           -- diagnostic_sink (host-injected log channel)
  hardening.h            -- limits + statistics
  backend/vulkan/        -- device.h, surface_provider.h (the only Vk-aware headers)
src/core/                -- compiler_state, DAG, free-function phases (system.cpp, graph.cpp)
src/backend/vulkan/      -- vk_runtime, vk_graph_executor, stores, lowering
src/backend/dx12/        -- lowering contract + stub backend (INTERFACE target)
src/backend/metal/       -- lowering contract only (INTERFACE target)
```

构建 target：`render_graph::core`、`render_graph::vulkan`、`render_graph::dx12`（INTERFACE）、
`render_graph::metal`（INTERFACE）。core 与 vulkan 均使用 BUILD/INSTALL interface，
安装后可被 `find_package(render_graph CONFIG REQUIRED)` 在源码树外消费（`tests/install_consumer/` 验证）。

## 3. 对外接口使用

宿主只碰三件事：`create_device`、`apply_resource_changes`、`render`。
> 完整、可编译的逐字段示例见 `Examples.md`（surface_provider 三回调、批量
> 创建+上传+bindless publish、每帧 recipe 构建与主循环）；本节只提炼使用要点。

```cpp
// 1) create: surface_provider is the ONLY host obligation
render_graph::vulkan::device_config config{
    .application_name = "MyApp",
    .frames_in_flight = 3,
    .validation = true,
    .surface = {
        .state = &my_window,
        .instance_extensions = &my_instance_extensions,
        .create_surface = &my_create_surface,       // SDL_Vulkan_CreateSurface adapter
        .drawable_extent = &my_drawable_extent,
    },
    .diagnostics = my_diagnostic_sink,              // optional
};
auto created = render_graph::vulkan::create_device(config);
if (!created) { /* created.error */ }
render_graph::render_device device = std::move(created.device);   // RAII, dtor calls destroy

// 2) resource changes: one atomic batch, submitted at frame boundary
render_graph::resource_change_batch batch{
    .buffer_creates = buffer_rows,        // spans of *_create_row (desc in, handle out)
    .image_creates = image_rows,
    .sampler_creates = sampler_rows,
    .graphics_pipeline_creates = pipeline_rows,   // SPIR-V binary blobs
    .buffer_uploads = upload_rows,        // staged through the upload arena
    .bindless_publishes = publish_rows,   // resource -> bindless slot
    .retires = retire_rows,
};
auto result = device.apply_resource_changes(batch);
// result.value: {buffers, images, samplers, graphics_pipelines, bindless_slots, ...}
// result.error + result.diagnostic: {phase, row_kind, row_index, message}

// 3) per frame: recipe builds API-agnostic rows, backend does everything else
render_graph::frame_recipe recipe{
    .state = &my_scene,
    .build = [](const void* state, const render_graph::frame_environment& env) {
        return render_graph::frame_plan{ /* resources, passes, accesses,
            attachments, copies, dispatches, draws -- flat spans + frame_row_range */ };
    },
};
render_graph::frame_result frame = device.render(recipe);
// frame.status: rendered | skipped | failed
```

要点：

- `render_device` 是 opaque state + function table（`render_device_api`：initialize /
  apply_resource_changes / render / request_resize / shutdown / statistics / destroy），
  RAII 包装析构时自动 `destroy`。
- 句柄全部是 `device_handle<Tag>{index, generation}`；stale 句柄在 validate 阶段被拒。
- recipe 拿不到 `VkCommandBuffer`，也没有 unsafe/native barrier 入口；
  唯一的 pass 内显式同步是 `explicit_barrier(span)`。
- `frame_status::skipped`：窗口最小化（零 extent）或 swapchain OUT_OF_DATE，未提交半帧。
- `device.statistics()` 暴露 `render_statistics`（upload/draw pass 次数、presented frames、
  steady-frame descriptor updates、pipeline creations、indirect groups），宿主 smoke 契约靠它断言。

## 4. Compile 层：八个固定 phase

入口是非模板的 `compile_graph(const graph_compile_request&)`（`src/core/system.cpp:955`）。
任一 phase 失败即返回累积了 diagnostics 的 `graph_compile_output`，`compiler_state` 只是
phase 间传递的纯值聚合，不对外暴露。

```text
graph_compile_request (frame_plan + environment + injected backend tables)
  |
  v
[1] validate_recipe
  |   non-empty plan, pass limit, frame_row_range bounds,
  |   command-kind compatibility, capability flags
  v
[2] build_resource_versions
  |   frame rows --> logical resources + access_event rows
  |   inject stable "UploadPass" (passes[0]) + "UploadArena" buffer
  |   persistent descs via description callback, swapchain contract
  v
[3] build_dependency_dag
  |   O(n^2) access pairing; same resource + non-read-read --> edge
  v
[4] schedule_passes
  |   Kahn topological sort (min-heap by pass index) --> deterministic order
  |   incomplete sort --> cycle_detected diagnostic
  v
[5] compile_lifetimes
  |   first/last used pass per resource
  |   transient + aliasable + non-overlapping --> physical reuse (alias_handoffs)
  |   memory requirements via allocation callback --> memory blocks
  v
[6] compile_synchronization
  |   abstract state tracking --> transition intents
  |   pass prologue ops + alias handoff ops + graph epilogue ops
  |   output: CSR layout synchronization_plan
  v
[7] compile_submissions
  |   merge consecutive same-queue passes --> queue_submission_batch
  |   first batch waits external acquire, last signals external present
  |   cross-batch edges --> timeline_wait; cross-queue --> release/acquire
  v
[8] publish_compiled_plan
      plan.cache_key = recipe key ^ extent ^ format ^ swapchain state
                       ^ resource desc hashes ^ pass/access details
      fill statistics --> compiled_graph_plan
```

各 phase 的实现位置（便于跳转）：

| # | phase | 位置 |
|---|-------|------|
| 1 | `validate_recipe` | `src/core/system.cpp:196` |
| 2 | `build_resource_versions` | `system.cpp:239`（含 `inject_stable_upload_pass`） |
| 3 | `build_dependency_dag` | `system.cpp:499` → `graph.cpp:16` |
| 4 | `schedule_passes` | `system.cpp:508` → `graph.cpp:41` |
| 5 | `compile_lifetimes` | `system.cpp:521` |
| 6 | `compile_synchronization` | `system.cpp:655` |
| 7 | `compile_submissions` | `system.cpp:798` |
| 8 | `publish_compiled_plan` | `system.cpp:872` |

产出 `compiled_graph_plan`（`include/render_graph/compiler.h:101`）：

- `resources`：SoA `compiled_resource_rows`（names/descs/desc_hashes/is_imported/lifetime_classes 分列）；
- `passes` / `scheduled_passes`：编译后 pass 行与调度序；
- `lifetimes` + `physical_resources`：物理复用 meta、handle→physical 映射、memory blocks、alias handoffs；
- `synchronization`：CSR 布局的 `synchronization_plan{prologue_begins/lengths, epilogue, ops}`；
- `submissions`：`queue_submission_batch` 序列与 timeline 依赖；
- `cache_key` + `statistics`。

诊断是结构化的：`compile_diagnostic{code, pass, kind, resource, pass_name, resource_name, message}`，
24 种错误码（`cycle_detected`、`pass_limit_exceeded`、`backend_failure`、`unsupported_feature`……），
用户输入错误不依赖 assert。

### 4.1 [1] validate_recipe —— 输入把关

一句话：**"这份 recipe 是不是格式合法、当前后端能不能跑？"** 只做只读检查，不改任何状态。

- 空帧 / 空 `frame`，或 pass 数超出 `environment.limits.max_passes` → 报错；
- 逐 pass 用 `valid_range{begin,count}(size)` 检查六个行段（buffer/image access、attachments、
  copies、dispatches、draws）与 push constant 段是否越界 —— 防越界读是首要目标；
- 命令与 pass 类型的兼容矩阵：raster 不能带 copy/dispatch，compute 不能带 attachment/copy/
  draw，copy 不能带 attachment/dispatch/draw；每种 pass 还要过
  `capabilities.{supports_graphics, compute, copy}`；
- 失败路径只往 diagnostics 里追加，尽量"检查完所有再返回"，最终看 `result.succeeded()`。

复杂度 O(passes)。

### 4.2 [2] build_resource_versions —— 把"声明"翻译成"事件"

一句话：**把 recipe 里扁平的资源表 / 行为表，摊平成编译器内部统一的 `access_event` 流**，
后续各阶段只看事件流，不再看用户行的形状。

- **资源翻译**：每个 `frame_resource_row` 变成一个 logical 资源（SoA 行，`resources.{descs,
  names, desc_hashes, is_imported, lifetime_classes}` 分列）：
  - `persistent_*`：用 `descriptions` 回调把 device handle 换成真实 desc（stale handle 报
    `backend_failure`）；
  - `swapchain_image`：环境给的恒定 desc（`color_format`/`extent`/`PRESENT`、禁止 aliasing、
    imported）；
  - `transient_*`：直接使用描述；所有 desc 都要过 `validation` 回调，并存 `hash_resource_desc`
    摘要。
- **stable UploadPass**：`inject_stable_upload_pass=true` 时，`passes[0]` 固定为一个合成 copy
  pass（`backend_upload`），并造出一个 imported "UploadArena" 缓冲区；随后给它注入事件：
  读 arena、写所有 `TRANSFER_DST` 的 persistent buffer（每帧 host→device 数据的正式入口）。
- **事件采集**：逐 pass 把
  - `buffer_accesses` → `access_event{pass, buffer, usage/access/range}`，
  - `image_accesses` → `{pass, image, usage/access/subresource}`，
  - `attachments` → 既是事件（usage=`COLOR_ATTACHMENT`/`DEPTH_STENCIL_ATTACHMENT`，access 由
    load op 决定：`load`→read_write，`clear/dont_care`→write），也同时填充
    `pass.raster.{colors[], depth_stencil}`；
  - 合并：`append_access` 对同一 `(pass, kind, logical)` 的事件事后合并 —— usage 变量按位或、
    access 相同才保留（否则置 read_write）、subresource/bytes 不同则清空为"整个资源"。
- **swapchain contract**：给 swapchain 逻辑资源标注
  `has_initial/final_state`，`initial = PRESENT(已初始化) 或 NONE(第一帧)`，`final = PRESENT`，
  供 §4.6 生成首帧的 NONE→... 与收尾的 …→PRESENT 过渡。

复杂度 O(passes × 每行段跨度) + 事件合并 O(事件数)。

### 4.3 [3] build_dependency_dag —— 依赖从哪儿来

一句话：**同一资源的两次访问之间，只要不是"读-读"，就必须有一条先后边**。

`build_dependency_dag`（`graph.cpp:16`）是纯函数：事件表 → `outgoing[pass]` / `in_degrees[pass]`。

```text
for each pair (previous_event, current_event)  [O(n^2)，n = 事件数]
    skip if 同 pass / kind 不同 / logical 不同
    若 conflicts(before.access, after.access)   // 非 read-read 即冲突
        → 建边 previous.pass --> current.pass（去重）
```

直觉：read/read 可以并行，一定不能乱序的是"有人读它时要保证写已完成 / 有人要写时必须等读
完成"。边是"同一资源的访问链"，后续调度天然保证资源使用顺序，barrier 也贴着这条链生成。

复杂度 O(n²) + 去重 O(边数 × 邻接平均长)。

### 4.4 [4] schedule_passes —— 拓扑排序

一句话：**给 DAG 一个确定性的执行顺序**（`graph.cpp:41`）。

- Kahn 算法 + **最小堆**：所有入度为 0 的 pass 进堆，每次取 `pass_handle` 最小的一个排入
  结果，再释放它的邻居。堆保证：多个"同时就绪"的 pass 字典序稳定，同一份输入永远得到同一
  份调度（对缓存与可复现调试验证很关键）。
- 结束后若 `schedule.size() != pass_count`，说明有环 → `cycle_detected`。

复杂度 O(P + E·log P)。产物 `scheduled_passes` 是后续所有"序号"的基准。

### 4.5 [5] compile_lifetimes —— 谁能共用一块内存

一句话：**给每个逻辑资源算"死区"，把死区不重叠、描述完全一致的 transient 资源指到同一块
物理内存** —— 这是显存复用 / aliasing 的源头（`system.cpp:521`）。

- 先把每个 pass 映射到调度序号（`order[pass]`），扫事件得到每个逻辑资源的
  `first/last_used_pass`。
- 物理复用：对逻辑资源 `logical`，向前扫描候选 `candidate`，满足
  - 都 `transient`，且 `aliasing != forbidden`；
  - desc 完全相等（`descs[candidate] == desc`）；
  - `candidate.last_order < logical.first_order`（生命周期不重叠）；
  → 复用 `candidate` 的 physical id 与 memory block，并记录
  `alias_handoff{previous, next, memory_block, at_pass = first_used_pass}`（交接点）。
  否则开新 physical slot。
- 内存需求：每个 physical（非 imported）由 `allocations` 回调算出
  `allocation_requirements{size, alignment, memory_type_bits, ...}`，作为独立
  **memory block**；backend 按 block 分配，再 `Aliasing*` 创建对象（见 §13.5）。

复杂度 O(R²)（candidate 线性扫描；R 为资源数）。产物就是
`physical_resources.{handle_to_physical_*_id, physical_*_meta, *memory_blocks, alias_handoffs}`，
也是 backend `can_reuse_plan` 的比对依据（跨帧整块复用 VMA 分配）。

### 4.6 [6] compile_synchronization —— 生成 barrier 计划

一句话：**沿着"访问链"推导出每个 pass 前面要插哪些 barrier，产出 CSR 布局的
`synchronization_plan`**（`system.cpp:655`）。

对每个 scheduled pass、每个事件维护"该逻辑资源上一次的抽象状态"：

1. `before` 解析顺序：上一次记录的状态 → `contract.initial_state`（首见）→ 兜底
   `{usage=0, access=read}`（不知道就先当"只被读过"）；
2. `after` = 把事件翻译成 `abstract_resource_state{usage_bits, access, domain, queue, range}`；
3. `transition_intents(before, after, kind)` 三件套：
   - image 的 usage 变了 → `layout_transition`；
   - 存在 hazard（非 read-read，见 §4.3 同一判据）→ `execution_dependency |  memory_dependency`；
   - queue 变了 → `queue_ownership`；
4. `intents != none` → 生产一个 prologue op（`scope=pass_prologue`），其中
   **phase**：同 queue → `full`；跨 queue → `acquire`（release 一半挪到 §4.7 拆）；
   并把 `before/after` 整份状态快照进 op —— 后端只靠这一个 op 就能重建完整 barrier。
5. 每个 `alias_handoff` → 一个 `aliasing | memory_dependency` 的 prologue op（提醒 backend
   "这里换了一块内存"，但对象内容不能中途被读）；
6. 图收尾：所有带 `final_state` 的资源，若最后状态 ≠ final → 一条
   `scope=graph_epilogue` 的 op（`phase=full`），`source_pass` 记最后一次使用的 pass；
7. 打包：`ops` 扁平容器，`prologue_begins/lengths[pass]`（CSR 索引）、`epilogue_begin/length`。

复杂度 O(passes × 事件数)，每事件 O(1)。

### 4.7 [7] compile_submissions —— 打包提交

一句话：**把调度好的 pass 归并成若干 queue submission batch，并补上跨 batch 的顺序保证**
（`system.cpp:798`）。

- 归类：**相邻且同 queue** 的 pass 并到一个 batch（图形队列当下就一个 batch 装全部）；
  首个 batch `waits_for_external_acquire`（等 image_available），末个 `signals_external_present`；
- 跨 batch 的 DAG 边 → 一条 `timeline_wait{source_batch, source_queue, value}`（去重）——
  这是编译期就把 queue 之间同步关系显式化、执行期可直接落 `vkSemaphore`；
- 凡是上阶段标了 `queue_ownership` 的 op，在这里**拆成两半**：
  `release` 挂源 batch，`acquire` 挂目标 batch，并记一条 `cross_queue_dependency` ——
  split barrier 的编译起点。

复杂度 O(P + E + ops)。

### 4.8 [8] publish_compiled_plan —— 缓存与统计

一句话：**把整份 plan 浓缩成一个 cache_key，并填好统计字段**（`system.cpp:872`）。

cache key 混入：recipe key ⊕ 环境（extent/color_format/swapchain_initialized）⊕ 每个资源
desc 摘要 ⊕ 每个 pass（kind/queue/name/raster 附件与 subresource）⊕ 每个事件
（pass/logical/kind/access/抽象状态/range）。**刻意排除**：draw/upload 行数、push constant
内容 —— 这些变化不应触发重编译，稳定场景因此逐帧命中缓存（`rebuild_row_graph` 里
`cache_key` 相同直接跳过编译与 VMA 重建）。

统计 `plan.statistics`：pass/激活 pass/资源/事件/同步 op / submission batch / 内存 block 计数，
也是宿主 smoke 契约的观测点。

## 5. 每帧执行序列

`render_frame`（`src/backend/vulkan/vulkan_device.cpp:735`）按固定 phase 表执行：

```text
render(recipe)
  |
  v
[resize pre-pass]  resize_requested --> runtime.resize()
  |                zero extent --> skipped
  v
[1] acquire
  |   wait frame fence --> update completed_submission --> collect_retired
  |   vkAcquireNextImageKHR
  |   OUT_OF_DATE --> skipped (+resize flag)   SUBOPTIMAL --> continue, noted
  v
[2] recipe.build(environment) --> frame_plan
  |   host-side validation + lowering command rows to native rows
  v
[3] graph compile / cache
  |   cache_key hit --> reuse compiled plan
  |   miss --> compile_graph + on_compile_resource_allocation (VMA blocks)
  v
[4] realize_resources   (currently a no-op placeholder; real work happens
  |                      in compile-time allocation + record-time binding)
  v
[5] record_batches
  |   reset pool, begin (ONE_TIME_SUBMIT)
  |   per scheduled pass: prologue barriers
  |     --> raster: vkCmdBeginRendering/EndRendering + draws
  |     --> UploadPass (backend_upload): pending staging copies
  |     --> copies / dispatches / indexed-indirect
  v
[6] submit
  |   vkQueueSubmit2: wait image_available, signal render_finished + fence
  |   frame.submission = next_submission++
  v
[7] commit   commit_pending_uploads(submission) --> staging slices retire
  v
[8] present
  |   OUT_OF_DATE / SUBOPTIMAL --> skipped (+resize flag)
  v
[9] collect_retired   swapchain_initialized[image] = true
```

关键语义：

- **cache key 组成**：recipe key ⊕ extent ⊕ color format ⊕ `swapchain_initialized[image]` ⊕
  资源 source/name/handle(index+generation)/desc 摘要 ⊕ pass/access/attachment 明细。
  **上传行数与 draw 行数不进 key** —— 稳定场景逐帧零重编译。
- swapchain image 的 initial layout（NONE vs PRESENT）由 `swapchain_initialized[image]` 决定，
  因此也是 cache key 的一部分；acquire 失败不推进该标志。
- acquire/present 的 OUT_OF_DATE 路径都返回 `skipped` 并置 resize 标志，不提交半帧；
  graph commit 只发生在成功 submit 之后。
- 失败路径统一 `abort_frame()` + `frame_result{failed, error}`。
- 当前所有 pass 落在 graphics queue（`queue_availability{compute:false, copy:false}`），
  compile 层的多队列契约由 fake tests 固化，物理多队列是预留能力。

## 6. Vulkan runtime 初始化序列

`vk_runtime::initialize`（`src/backend/vulkan/vk_runtime.cpp:54`）严格按序，
任一步失败聚合 `last_error_` 并 `shutdown()` 后返回错误：

```text
initialize
  |
  v
[1] check surface_provider callbacks (3x present)
[2] create_instance
  |     instance extensions from provider (+ debug_utils + KHRONOS_validation if validation)
  |     api = Vulkan 1.3
  |     debug messenger --> validation_callback --> diagnostic sink
[3] surface = provider.create_surface(instance)
[4] select_physical_device
  |     api >= 1.3, swapchain ext, feature checklist (see below)
  |     queue families: graphics(+present); compute/copy fall back to graphics
[5] create_device         -- enable exactly the checked feature set + VK_KHR_swapchain
[6] create_allocator      -- VMA, EXT_MEMORY_BUDGET, api 1.3
[7] create_swapchain      -- B8G8R8A8_UNORM/SRGB preferred, FIFO, per-image view + semaphore
[8] create_frame_rows     -- per frame: cmd pool + primary cmd buffer + semaphore + pre-signaled fence
[9] initialize_bindless   -- see section 7
  v
render_graph::vulkan::create_device (vulkan_device.cpp:1029) then:
[10] create 256MB device-local arena + graph_executor.set_context(...)
```

**feature 硬性清单**（`select_physical_device` 检查、`create_device` 启用，缺一即初始化失败并列出缺失项）：

- `synchronization2`、`dynamicRendering`
- `runtimeDescriptorArray`、`descriptorBindingPartiallyBound`
- `descriptorBinding{SampledImage,StorageImage,UniformBuffer,StorageBuffer}UpdateAfterBind`
- `descriptorBindingUpdateUnusedWhilePending`
- `shader{SampledImage,StorageImage,UniformBuffer,StorageBuffer}ArrayNonUniformIndexing`

不提供传统 descriptor fallback——没有这套 feature 的设备直接不可用。

## 7. Bindless 实现

### 7.1 固定五表 ABI

```text
single persistent descriptor set (maxSets = 1, UPDATE_AFTER_BIND_POOL)
+---------+--------------------------+-----------+
| binding | table                    | capacity  |
+---------+--------------------------+-----------+
|    0    | sampled_images[]         |   2048    |
|    1    | samplers[]               |    128    |
|    2    | storage_images[]         |    512    |
|    3    | uniform_buffers[]        |   1024    |
|    4    | storage_buffers[]        |   4096    |
+---------+--------------------------+-----------+
binding flags: PARTIALLY_BOUND | UPDATE_AFTER_BIND | UPDATE_UNUSED_WHILE_PENDING
stage flags:   ALL
```

- CPU 句柄：`vk_bindless_handle{index, generation, table}`；
  slot 行记录 `{generation, safe_after_submission, occupied, owned_view, owned_sampler}`。
- **slot 0/1 是默认资源**（`initialize_default_bindless_resources`，`vk_bindless.cpp:128`）：
  sampled_images[0]=1×1 白图、[1]=1×1 法线图（0.5,0.5,1）；samplers[0]=linear；
  storage_images[0]、uniform/storage_buffers[0] 均为清零占位。分配起点：
  sampled_images 从 index 2，其余从 index 1。shader 访问未绑定 slot 永远读到合法默认值。
- 所有 pipeline 共用同一个 pipeline layout：单 set（bindless layout）+ desc 声明的
  push constant ranges；录制时统一 `vkCmdBindDescriptorSets(set 0)`。

### 7.2 slot 生命周期

```text
allocate (publish)
  |   linear scan: skip occupied AND safe_after_submission > completed_submission
  |   reuse --> generation++
  |   immediate vkUpdateDescriptorSets (update-after-bind: effective mid-frame)
  v
in use  ---------------------------------------------------
  |                                                        |
  v                                                        v
release (retire)                                 collect_bindless (per frame)
  occupied = false                                 gate passed (safe_after <= completed):
  safe_after_submission = next_submission    -->   rewrite slot back to default resource
                                                   destroy owned view/sampler
```

### 7.3 稳定场景零分配

1. slot 复用内嵌在表内，无 free-list 堆分配；
2. 只有 publish/retire 变更才触发 descriptor write（`statistics.descriptor_updates` 可观测，
   宿主 smoke 断言 steady-frame descriptor updates == 0）；
3. pipeline 按 desc hash 命中缓存（`vk_pipeline_store.cpp:63`）；
4. graph cache key 命中跳过整轮编译与 VMA 重建；重编译时 `can_reuse_plan`
   （`vk_backend.h:794`）进一步复用兼容的整块 VMA allocation 与 image/buffer/view。

## 8. 资源与内存：arena + slice

```text
+------------------------------------+------------------------------------------+
| arena                              | serves                                   |
+------------------------------------+------------------------------------------+
| upload arena   64MB (lazy)         | staging slices for buffer/image uploads |
|   TRANSFER_SRC, upload, persistent |   alignment 16                          |
| device arena  256MB (at create)    | device_local + automatic buffers        |
|   TRANSFER_DST|VTX|IDX|SSBO|...    |   alignment 256 (vertices/indices/etc)  |
| readback arena 16MB (lazy)         | readback + automatic buffers            |
|   TRANSFER_DST, readback, persist  |   alignment 64                          |
+------------------------------------+------------------------------------------+
```

buffer create 三分支（`vulkan_device.cpp:311`）：

```text
buffer_create row
  |-- device_local + automatic --> slice from device arena   (suballocated)
  |-- readback    + automatic --> slice from readback arena  (suballocated)
  +-- otherwise (upload / dedicated) --> vmaCreateBuffer
                                          (+ DEDICATED_MEMORY if requested)
image create --> always vmaCreateImage
graph transient resources --> vmaAllocateMemory + vmaCreateAliasing{Image,Buffer}2
                               (physical reuse per compiled plan)
```

staging slice 生命周期（submission-gated 复用）：

```text
stage_buffer_upload / stage_image_upload
  |   slice from upload arena + memcpy + queue pending copy
  v
UploadPass (backend_upload, always passes[0])
  |   record vkCmdCopyBuffer / CopyBufferToImage
  |   (+ barrier2: UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY for images)
  v
submit done --> commit_pending_uploads(submission N)
  |   slice --> retired_buffer_slices {safe_after = N}
  v
collect_buffer_slices (when N <= completed_submission)
      slice --> free_spans, merge_free_spans() coalesces neighbours
```

retirement 一律由 submission 序号控制：`retired_buffer_slices` / `retired_buffers` /
`retired_images` 与通用 `vk_retirement_table`（`retire()` / `collect_retired()`）。
persistent 资源不因 resize 重建；resize 只销毁 swapchain 与尺寸相关状态。

## 9. 资源变更事务（apply_changes）

```text
resource_change_batch
  |
  v
[validate]  pure read-only checks (desc validity, SPIR-V, stale handles, ranges)
  |         failure --> diagnostic{phase=validate, row_kind, row_index}, zero side effects
  v
[prepare]   checkpoint = upload_checkpoint()
            pipeline_rows_before = pipelines().rows.size()
            create provisional: pipelines --> buffers --> images --> samplers
                                --> bindless publish --> staged uploads
  |         any failure --> rollback() + diagnostic{phase=prepare}
  v
[commit]    publish_handle() for every provisional row (generation++)
            apply retires (safe_after = next_submission)
            refresh statistics --> resource_change_result
```

`rollback()` 覆盖（顺序即代码顺序）：

```text
rollback_pending_uploads(checkpoint)     -- release this txn's staging slices
destroy pipelines with index >= pipeline_rows_before
                                         -- only NEW rows; cache hits survive
release bindless slots / sampler slots   -- gated on completed_submission
destroy images
release buffer slices / destroy buffers
collect_retired()                        -- reclaim immediately
```

任一失败都不会发布 handle 或 bindless slot，native 对象全部回收——事务对调用方是原子的。

## 10. 诊断体系

- 库内不直接写 stderr/宿主日志（`BackendOwnershipContract.cmake` 强制）。
- 编译：`compile_result{diagnostics[]}`（结构化，见 §4）。
- 资源变更：`resource_change_result{error, diagnostic{phase, row_kind, row_index, message}}`。
- runtime：`vk_runtime_result{.error}` / `last_error()`；frame 级 `frame_result{status, error}`。
- validation layer：`validation_callback` 把 error 计入原子计数
  （`device.validation_error_count()`），error/warning 转发宿主注入的 `diagnostic_sink`。

## 11. DX12 / Metal（预留）

> 这两个 backend 当前只固化"同一公共描述可以被各自 lowering 消费"的契约，没有物理执行。
> 新 backend 落地时在本节补充其实现笔记，结构建议与 §5–§9 对齐。

- **DX12**：`src/backend/dx12/dx12_resource_lowering.h`（format/flags/heap lowering 完整）+
  `dx12_backend.h`（header-only，`on_compile_resource_allocation` 用 `CreateCommittedResource`，
  但 `emit_barriers` / `begin_raster_pass` / `end_raster_pass` 是空 stub）。
  INTERFACE target `render_graph::dx12`。
  - TODO: device/queue/swapchain 初始化笔记（占位）
  - TODO: descriptor heap / bindless 映射方案（占位）
  - TODO: barrier lowering 与 execute 序列（占位）
- **Metal**：仅 `src/backend/metal/metal_resource_lowering.h`（纯数据 lowering）。
  INTERFACE target `render_graph::metal`。
  - TODO: 初始化与执行笔记（占位）
- 共享契约测试：`render_graph.resource_description_lowering`（同一 desc 经 vk/dx12/metal
  三条 lowering 互校）；`dx12_render_graph_sample` 复用 compiler sample 源码（不触 DX12 API）。

## 12. 测试与契约

- 单元测试（`RENDER_GRAPH_BUILD_UNIT_TESTS=ON`）：`render_graph.<case>`——
  dag/schedule、lifetime aliasing、synchronization plan、multi-queue fallback、
  render_device_contract（fake backend 全流程）、vulkan_barrier_lowering、
  vulkan_resource_allocator、vulkan_sample_graph 等。
- 构建契约（CTest）：
  - `render_graph.core_layout`：公共头 ≤32KB、不暴露 `compiler_state`/DAG/src 路径、
    core 源码不得 include vulkan/VMA/d3d12/Metal；
  - `render_graph.backend_ownership`：backend 不依赖 engine/SDL/tinygltf/glm/logger、
    库内不写 stderr、公共头不转发 src。
- 安装消费：`tests/install_consumer/`（独立 project + `find_package(render_graph)`）。

## 13. Vulkan 现代高性能特性（实现细节）

> 本节收拢 backend 用到的"现代特性"及各自实现要点。§7/§8/§9 已经讲过的数据流不重复，
> 这里补的是"为什么、怎么落 API、代价是什么"。全部特性只依赖 Vulkan 1.3 core +
> `VK_KHR_swapchain`；不提供任何传统 fallback（§6 feature 硬性清单）。

### 13.1 特性总览

| 特性 | 位置 | 收益 |
|------|------|------|
| Synchronization2（core 1.3） | `vk_barrier_lowering.h`、`vkCmdPipelineBarrier2` | 精确实例化 barrier；stage/access 用 64 位精确位；无老式 layout 限制 |
| Dynamic Rendering（core 1.3） | `vk_backend.h:216 begin_raster_pass` | 免 render-pass/framebuffer 对象；附件动态、切换快；视图懒建 |
| Descriptor Update-After-Bind（bindless） | `vk_bindless.cpp` | 单描述符集 ABI；录制中途可更新；零 set 切换 |
| 非均匀索引（non-uniform indexing） | 强制的 feature 清单 | shader 用运行时下标访问表 |
| VMA 子分配 / aliasing | `vk_vma.cpp` + executor | 显存按块 + 逻辑复用，慢路径只在计划变化 |
| 编译期计划缓存 | `vulkan_device.cpp:rebuild_row_graph` | 稳态帧零编译、零 VMA 分配、零 pipeline 创建 |

### 13.2 Bindless：单描述符集 ABI

固定五表（§7.1），**整个图、所有 pipeline 共用同一个 pipeline layout、同一个 set**，
录制时只用一次 `vkCmdBindDescriptorSets(set 0)`。这意味着：

- **零描述符集切换**：一帧无论几个 pass，绑定开销恒为 1；多 pass 预算里最常见的抖动
  （descriptor set 不够用 → 翻页重建）从根上消失。
- **录制中途可更新**：`publish`（§7.2）落地为一次 `vkUpdateDescriptorSets`，slots 是
  `UPDATE_AFTER_BIND`，所以发布之后、recording 命令引用它都合法 —— 不需要像老式子集那样
  "画完再改"，分帧上传/动态数据可以贴着提交点写。
- **shader 侧**：表就是数组，运行时下标 + `nonuniformEXT`：

  ```glsl
  layout(set = 0, binding = 0) uniform sampler2D   textures[];
  layout(set = 0, binding = 3) uniform UBOData     ubo_table[];
  // …以 bindless_publish 返回的 slot 下标作为 nonuniformEXT(textures[id])
  ```

- **安全网**：slot 0/1 是默认白/法线图、默认 linear sampler、清零占位（§7.1），
  shader 访问任何"从未发布或已被 recycle"的槽都读到合法默认值，而不是 UB/缺页。
- **代价/限制**：容量固定（2048/128/512/1024/4096）；无传统 descriptor fallback ——
  不支持 update-after-bind 特性的设备直接拒绝初始化；`statistics.descriptor_updates`
  只统计真正 publish/retire 的动作，稳态帧为 0（宿主 smoke 断言）。

### 13.3 Dynamic Rendering

`begin_raster_pass`（`vk_backend.h:216`）把编译期 `raster_pass_desc` 直接转成
`VkRenderingInfo`：color/formats、load/store/clear、depth/stencil、resolve、层数全部
**动态决定**，不创建任何 render pass / framebuffer 对象，因此：

- 附件切换 = 重新 begin，无 pipeline 与 framebuffer 匹配；跨 pass 仅一次 prologue barrier。
- **image view 按需懒建并缓存**：`get_or_create_image_view(image, desc)` 以
  `(VkImage, vk_image_view_desc)` 为键查 `view_cache`，命中直接复用；desc 允许 format
  override 时强制要求 `MUTABLE_FORMAT`。视图是显存里稀缺对象，缓存显著减少创建/销毁抖动。
- MSAA resolve 内联在 RenderingAttachmentInfo 里（`resolveMode=AVERAGE` + `resolveImageView`）。
- render area 省略时自动落到首个附件的 extent。

### 13.4 Synchronization2 barrier 生成

编译层产出的是 §4.6 的抽象 `synchronization_op`（不命名 Vulkan 阶段/访问/布局），
`build_vk_barrier_batch`（`vk_barrier_lowering.h:291`）负责翻译：

- **状态 → (stage, access, layout) 映射**：
  - image **按 usage 优先级取单态**：`PRESENT`→`PRESENT_SRC_KHR`（不绑定 stage）；
    `STORAGE`→`GENERAL`；`COLOR/DEPTH`→对应 attachment layout（stage 精确到
    `COLOR_ATTACHMENT_OUTPUT` / early+late fragment test）；`TRANSFER_DST/SRC`→transfer；
    `SAMPLED`→`SHADER_READ_ONLY`（stage 按 domain 展开图形/计算）。一个状态一个 layout，
    不会有"多个 layout 同时成立"的歧义；
  - buffer usage **按位累积**：TRANSFER/VERTEX/INDEX/INDIRECT/UNIFORM/STORAGE 各自贡献
    精确的 `VkPipelineStageFlagBits2 | VkAccessFlagBits2`（STORAGE 再按 read/write 分）；
- **release/acquire 截断**（`synchronization_op.phase`）：release 半条 barrier 的
  dst stage/access 置 0，acquire 半条 src 置 0 —— split barrier 的物理实现；
- **aliasing intent** → 一条全局 `VkMemoryBarrier2`（`ALL_COMMANDS × MEMORY_WRITE→R/W`），
  因为别名点是"整块内存易主"，对象级精确无意义；
- **queue ownership** → `src/dstQueueFamilyIndex`（同 family 自动回落 `IGNORED`）；
- **UploadPass 内联过渡**（`vk_resource_store.cpp:426`）：图像上传在拷贝命令旁
  就地发两条 barrier2 `UNDEFINED→TRANSFER_DST→SHADER_READ_ONLY`，不进入 plan 的 prologue；
- 图收尾的 epilogue 正是把 swapchain 推进 `PRESENT_SRC_KHR` 的那条 op。

执行期每条 op 可能伸展成 `VkMemoryBarrier2 / VkBufferMemoryBarrier2 / VkImageMemoryBarrier2`
之一，全部打包进单一 `VkDependencyInfo`，只发一次 `vkCmdPipelineBarrier2`（v2 barrier
可合并，是 v1 时代多条 RF 的显著简化）。

### 13.5 资源与内存管理

三层分工（详细数据流见 §8）：

1. **宿主长期资源**：`apply_resource_changes` 创建，`arena`/子分配 或 `vmaCreateBuffer/
   Image` 直接分配；`buffer_create` 按 `memory_domain` 走 device arena（256MB 块）、readback
   arena（16MB，懒建）或 upload；**独立分配**（`allocation_policy::dedicated`）只给 arena 这种
   巨型对象。
2. **帧瞬态资源（graph-owned）**：编译期 `compile_lifetimes`（§4.5）已经给出"谁跟谁共用
   哪一块内存"的 plan；`on_compile_resource_allocation`（`vk_backend.h:467`）按
   `image_memory_blocks` 逐块 `vmaAllocateMemory`，再 `vmaCreateAliasing{Image,Buffer}2`
   在同一块上叠多个对象 —— **共用 = 单块显存，绝不复制**。
3. **跨计划复用**（防抖关键）：
   - `make_block_keys`：每块内存按"块内资源的名称 + desc hash + lifetime"混出一个 key；
   - 重编译时（cache miss）先扫旧块，**key 与 requirements 相同就整体"偷"过来**，否则才新建；
   - 对象级再用 `is_compatible_native_{image,buffer}` 复核，能复用则复用旧 `VkImage/VkBuffer`，
     否则旧对象进 `retired_resources{safe_after_frame = current + frames_in_flight}`，
     由 `collect_retired` 在 `begin_frame` 按完成帧清理 —— **所有销毁都是 submission-gated**，
     不会出现"GPU 还在用就销毁"。
   - `can_reuse_plan`（`vk_backend.h:838`）在映射与全部 desc 兼容时把整块 plan 直接沿用，
     连分配都不做。

配合 generation 句柄与 `view_cache`，稳态帧的内存/视图/对象创建全部为零。

### 13.6 帧同步模型

- 每帧（`vk_runtime.cpp:407 create_frame_rows`）一对 semaphore + 一个**预置 signal 的
  fence**；`acquire` 先 `vkWaitForFences`（保证上一轮同槽已结束 —— 这同时是
  `completed_submission` 前推与 `collect_retired` 的门槛），再 `vkAcquireNextImageKHR`。
- `submit`（`vkQueueSubmit2`）挂 `wait=image_available`、`signal=render_finished+fence`；
  `present` 等 `render_finished`。**timeline 部分在编译层**（§4.7 的 `timeline_wait` 已把
  跨 queue 依赖全部显式化），当前单图形队列下退化为 binary semaphore 通路，多队列落地时
  可平滑映射 timeline/dyad。
- `OUT_OF_DATE / SUBOPTIMAL` 统一映射为 `frame_status::skipped` 并回置 resize 标志，
  不提交半帧（acquire 失败不推进 `swapchain_initialized`，缓存语义因此保持单调）。
- frame rows 环形游标 + `++cursor % frames_in_flight`，3 in-flight 下 CPU 永不追 GPU。

### 13.7 组合效果与代价

- **稳态帧的不变量**：`graph_compiles`、`descriptor_updates`、`pipeline_creations`、
  VMA 分配、view 创建全部为零 —— 一帧只做：acquire → build → (cache hit) → record →
  submit → present。
- **代价（有意为之的限制）**：无传统 descriptor/非 update-after-bind fallback；无
  `history` 资源的执行语义（宿主自行管理 imported history）；未真正使用独立 compute/copy
  队列（编译契约与 timeline 已就绪，executor 暂回落 graphics）；ray tracing / sparse /
  video queue / classic subpass 不在范围。

## 暂不支持

Ray tracing acceleration structures、sparse resources、video queues、classic subpass。
独立 compute/copy queue 的物理使用（compile 契约已就绪，Vulkan executor 暂全部回落 graphics）。
