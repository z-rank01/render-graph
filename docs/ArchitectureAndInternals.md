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

入口是非模板的 `compile_graph(const graph_compile_request&)`（`src/core/system.cpp:879`）。
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
| 1 | `validate_recipe` | `src/core/system.cpp:173` |
| 2 | `build_resource_versions` | `system.cpp:219`（含 `inject_stable_upload_pass`） |
| 3 | `build_dependency_dag` | `system.cpp:465` → `graph.cpp:16` |
| 4 | `schedule_passes` | `system.cpp:472` → `graph.cpp:41` |
| 5 | `compile_lifetimes` | `system.cpp:482` |
| 6 | `compile_synchronization` | `system.cpp:607` |
| 7 | `compile_submissions` | `system.cpp:739` |
| 8 | `publish_compiled_plan` | `system.cpp:807` |

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

## 暂不支持

Ray tracing acceleration structures、sparse resources、video queues、classic subpass。
独立 compute/copy queue 的物理使用（compile 契约已就绪，Vulkan executor 暂全部回落 graphics）。
