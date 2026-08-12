# Render Graph 架构与实现笔记

> as-built，2026-08。本文以当前代码为准（`include/render_graph/`、`src/core/`、`src/backend/vulkan/`），
> 作为深入阅读源码时的导航与原理说明。README 描述能力边界，本文描述"它是怎么跑起来的"。
> 对外 API 的逐字段用法见 `Examples.md`；本文不重复接口内容，只讲机制。
> 图示统一使用 ASCII（`-->`），图内文字保持英文以保证等宽对齐。

## 1. 对外部分

调用者负责：**创建设备**、**提交资源变更**（拿句柄）、**每帧渲染**（给一个
"本帧要画什么"的声明）。库内部的核心分工是：

- **资源 = 句柄表里的一个槽**。句柄是 `{index, generation}`，调用者手里拿着的是"凭证"，
  不是原生对象；每次使用句柄（apply 的引用、帧 plan 的引用）都要**对照当前句柄表校验**，
  凭证失效（retire 后复用）立即报错，绝不静默产生错误帧。
- **编译是每帧事件，不是一次性事件**。每帧 `render()` 先用当前句柄表逐句柄重新校验并
  lowering，再按 cache key 决定"重编译"还是"复用上一次编译产物"；缓存命中也意味着
  本帧引用的所有句柄都与上次完全一致（key 里含每个 persistent 句柄的 index+generation）。
- **编译产出的是"同步计划"，不是命令**。compile 算出调度序、生命周期、barrier 计划与
  submission 分组；执行期按计划逐 pass 录制命令、发 barrier、提交、present。

```text
apply_resource_changes(batch1: creates)  --> 句柄 {index, generation}（此刻才存在）
apply_resource_changes(batch2: uploads+publishes) --> bindless 槽号（引用 batch1 的句柄）
render(recipe) 每帧:  acquire → build → 逐句柄校验+lowering → compile 或缓存命中
                     → record → submit → present
```

## 2. 分层总览

```text
Caller (engine / samples / tests)
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
|   acquire -> validate/lower -> compile -> record ->    |
|   submit -> present                                    |
+--------------------------------------------------------+
  |
  v
Vulkan 1.3 driver (Synchronization2, Dynamic Rendering, VMA)
```

- Core 不含任何原生 API 类型；backend 能力通过显式 function table
  （`resource_validation_api` / `resource_description_api` / `resource_allocation_api`）注入。
  其中 `resource_description_api` 是 **core ↔ 句柄表之间的唯一桥梁**：core 编译时靠它
  把调用者给的句柄换成真实 desc（§4.3 第 4 道防线）。
- `render_graph::core` 是真实编译库（STATIC，可选 SHARED），不是 header-only target。
- DX12 / Metal 目前只有 lowering contract 与 fake tests，见 §12。

源码导航（行号随代码更新，搜索符号名更稳）：

| 关注点 | 位置 |
|--------|------|
| 句柄表 / publish / find_handle / retire | `src/backend/vulkan/vulkan_device.cpp:27`（`handle_row`）、`:92`（`publish_handle`）、`:106`（`find_handle`） |
| apply_resource_changes 事务 | `vulkan_device.cpp:206`（`apply_changes`） |
| 每帧校验 + lowering + cache key | `vulkan_device.cpp:831` 起（`render_frame`） |
| 编译入口与八个 phase | `src/core/system.cpp:955`（`compile_graph`） |
| 编译期句柄校验（describe 回调） | `system.cpp:239`（`build_resource_versions`）+ `vulkan_device.cpp:594`（回调注入） |
| 每帧执行序列 | `vulkan_device.cpp:793`（`render_frame`）→ `:653`（`record_graph`） |

## 3. 核心时序：两批 apply → 每帧 render

```text
第 1 批 apply_resource_changes（创建）
  buffer/image/sampler/pipeline create 行
    --> [validate]      只读检查：desc 合法性、SPIR-V 格式
    --> [prepare]       创建原生对象（失败则整体 rollback）
    --> [publish]       写入句柄表，返回句柄 {index, generation}
    --> [retire]        按本批 retire 行销毁旧资源（延迟到 GPU 用完）
  此刻调用者才第一次拿到 buffer/image/sampler/pipeline 句柄。

第 2 批 apply_resource_changes（上传 + 发布）
  buffer/image upload 行、bindless publish 行
    --> [validate]      引用的句柄必须已在表里且 generation 匹配
    --> [prepare]       staging 拷贝入 upload arena / 分配 bindless 槽
    --> [publish]       返回 bindless 槽号（uint32_t，shader 直接当索引用）
  为什么必须分两批：upload/publish 行是"引用句柄"的行，
  而句柄只有第一批 publish 之后才存在——不是设计偏好，是依赖决定的。

每帧 render(recipe)
  --> [1] acquire         等 fence → 推 completed_submission → 回收已完成的销毁
  --> [2] build           调用者回调拼出本帧 frame_plan（声明：资源+pass+访问）
  --> [3] validate        逐句柄对照句柄表（防 stale）+ 行段越界检查
  --> [4] lower + key     句柄解析成 native 对象，同时混出设备侧 cache key
  --> [5] compile/缓存     key 命中 → 复用上次编译产物；未命中 → compile_graph
  --> [6] record          按调度序：prologue barrier → pass 命令 → epilogue barrier
  --> [7] submit / present
```

三个关键事实先记住，后面各节展开：

1. **句柄在 compile 之前就存在**，但编译不是"用一次句柄就完了"——每帧都重新校验。
2. **句柄失效 = 校验失败 = 整帧失败**（不录制、不提交），不存在"悄悄画错"。
3. **缓存只缓存编译产物**（同步计划），不缓存句柄解析；句柄 → 原生对象的解析每帧重做。

## 4. 句柄模型与有效性保证

### 4.1 句柄：{index, generation}

```cpp
template <typename Tag> struct device_handle
{ uint32_t index = UINT32_MAX; uint32_t generation = 0; };
```

- 每类资源一张句柄表（buffer / image / sampler / pipeline / bindless 各一张），
  表项 `handle_row{native, generation = 1, alive = false}`（`vulkan_device.cpp:27`）。
- 句柄**不是指针，是凭证**。`find_handle(table, handle)` 只有当
  `index` 未越界 && 表项 `alive` && `generation == 表项.generation` 才返回表项，
  否则返回 null（`vulkan_device.cpp:106`）。
- `generation == 0` 是空句柄哨兵；调用者绝不该构造它（apply 返回的句柄 generation 恒 ≥ 1）。

### 4.2 生命周期：publish / retire / generation++

```text
publish（apply 的 publish 阶段）
  线性扫表找第一个 dead 槽 --> 复用该槽，返回 {index, 当前 generation}
  表已满 --> push_back 新槽 {generation = 1}
retire（apply 的 retire 阶段，或显式 resource_retire_row）
  找到槽 --> 原生对象延迟销毁（safe_after = next_submission，GPU 用完后才销毁）
          --> alive = false; ++generation        <-- 代际只在这里递增
  找不到（stale）--> 静默跳过，不算错误
```

所以：**同一 index 被 retire 后再次 publish 复用，generation 已经 +1**。旧句柄
（旧 generation）从此在所有校验点失败——即使 index 相同、甚至原生对象还活着。

### 4.3 失效句柄的四道防线

一个句柄从 apply 拿到，到被 GPU 使用，要跨过四道独立校验。任何一道失败，
该次操作整体失败（apply 返回诊断 / 本帧 `failed`），**不产生半帧、不提交任何东西**：

```text
[1] apply 的 validate 阶段（vulkan_device.cpp:246-281）
      buffer/image upload 的 destination、bindless publish 的 image/sampler/buffer
      --> find_handle 失败即报 "stale handle"，整批回滚、不发布任何新句柄。

[2] render 帧级预校验（vulkan_device.cpp:854-882）
      frame plan 里每个 persistent_buffer / persistent_image 资源行
      --> find_handle 失败即整帧 failed（"stale persistent buffer resource"）。

[3] lowering 阶段（vulkan_device.cpp:940-1035）
      draw / copy / dispatch 行引用的 pipeline/buffer 句柄
      --> 解析成 native 时 find_handle 失败即整帧 failed。

[4] 编译阶段 build_resource_versions（system.cpp:271-330）
      core 不认识句柄表，它通过注入的 describe_buffer/describe_image 回调（vulkan_device.cpp:594）
      把句柄换成真实 desc；回调内部就是 find_handle，返回 false 即
      compile_error_code::backend_failure "persistent buffer handle is stale"。
```

防线 [2]–[4] 覆盖的是**帧 plan 里的引用**，防线 [1] 覆盖的是 **apply 批里的引用**。
它们查的是**同一张表、同一时刻的状态**，所以不存在"apply 时有效、compile 时失效"的
时间窗口：compile 每帧发生，每次编译用的都是当帧的表。

### 4.4 缓存到底缓存了什么

设备侧 cache key（`vulkan_device.cpp:890-1035`）混合：

- `plan.cache_key`（compile 侧 key，含调用者自增的 `frame_plan.cache_key`）；
- 环境：extent / color format / `swapchain_initialized[image]`；
- 每个资源行：source + name + （persistent → **句柄 index+generation**）/
  （transient → desc 摘要）；
- pass（kind/queue/name/行段范围）、attachment、buffer/image access（persistent 的
  buffer access 同样混入**句柄 index+generation**）。

`rebuild_row_graph`（`vulkan_device.cpp:564`）在 key 完全一致时直接复用上次的
`compiled_graph_plan`，跳过编译与 VMA 重建。于是：

- **命中缓存 ⇔ 本帧所有 persistent 句柄与上次完全相同**（index 和 generation 都进了 key）。
  命中的编译产物引用的逻辑资源 → 物理资源映射与当前句柄表严格一致。
- **lowering 不在缓存里**：即使命中缓存，每帧仍会重新 find_handle、重新生成
  native draw/copy/dispatch 行。缓存只省编译，不省校验。
- 刻意不进 key 的：draw/upload 行数与内容、push constant——这些在编译产物之外，
  每帧重新 lowering 即可，不值得触发重编译。

### 4.5 常见疑问

**Q：句柄被 retire 后同 index 复用（generation 变了），帧里还拿着旧句柄怎么办？**
A：帧级预校验（防线 [2]）就失败，整帧 `failed`，不录制不提交。调用者改拿新句柄
（新 generation）后，cache key 变化 → 自动重编译。没有任何路径会"拿着旧凭证画出新东西"。

**Q：一个 persistent 资源本帧没有 pass 消耗它，会被编译器剔除吗？**
A：不会。persistent 资源**不做引用计数、不做死代码消除**——它在帧 plan 的资源表里被
登记就会保留（逻辑资源、desc 都在编译产物里）；没有 access 事件只是意味着不产生
依赖边、不产生 barrier，资源本身原样活着。真正杀死它的是**显式 retire**。调用者不再把
某个资源放进帧 plan，它也只是"从本帧编译产物里消失"，句柄依然有效，随时可放回。

**Q：资源的原生对象会不会在 GPU 还在用的时候被销毁？**
A：不会。所有销毁都是 **submission-gated**：retire 记下 `safe_after = next_submission`，
`collect_retired`（在每帧 acquire 时）只回收 `safe_after <= completed_submission` 的对象。
同一代际的帧（缓存命中的稳定帧）天然共享同一批原生对象，绝无竞态。

**Q：声明了但从不使用的资源，会真的创建物理对象和分配显存吗？**
A：会——两条路径都是 eager，与是否被使用无关：
- **persistent 资源**：apply 的 prepare 阶段立即创建。image 总是 `vmaCreateImage`
  （真实显存分配）；buffer `device_local + automatic` 从 device arena 切 slice（不新增
  显存分配，但 VkBuffer 对象已建），dedicated/upload/readback 则 `vmaCreateBuffer`。
  之后只能靠显式 retire 释放——没有引用计数、没有自动回收。
- **transient 资源**：编译时照常登记为逻辑资源；compile_lifetimes 里它没有访问事件
  （first_order 是哨兵值），不满足 alias 复用条件 → 拿到独立 physical slot + 独立
  memory block；`on_compile_resource_allocation` 逐块分配、逐对象创建，没有
  "未使用就跳过"的分支。零引用的 transient 同样得到一块真实显存和一个 VkImage/VkBuffer。
- 真正的惰性只在这些地方：缓存命中帧不重编译（§4.4，零分配）；image view 在 record
  时才懒建（§14.4）。这是**计划级惰性**，不是**使用级惰性**。

## 5. 资源变更事务（apply_resource_changes）

入口 `apply_changes`（`vulkan_device.cpp:206`）。一批 = 一个原子事务：任何一行失败，
全部回滚，**不发布任何句柄 / bindless 槽**。

```text
[validate]  只读检查，零副作用
  - create 行：desc 过 vk_graph_executor::validate_*_desc
  - pipeline 行：必须 SPIR-V 二进制
  - upload 行：destination 句柄有效（find_handle）+ 字节范围不越界
  - publish 行：对应类型的句柄有效（image 表 / buffer 表 / sampler 表）+ 范围不越界
  - retire 行：stale 静默跳过
[prepare]   逐个创建原生对象，任一失败触发 rollback
  - 顺序：pipelines --> buffers（device arena 子分配 / readback arena / vmaCreateBuffer）
         --> images --> samplers --> bindless publish（分配槽）--> staged uploads
  - uploads：device_local 目标先 memcpy 进 upload arena（等每帧 UploadPass 拷入）；
    host-visible 目标直接 update_buffer 写穿
[publish]   publish_handle 逐个入表 --> 返回 {buffers, images, samplers, pipelines,
            bindless_slots[]}（槽号是 uint32_t，shader 直接当非均匀索引下标）
[retire]    本批 retire 行：延迟销毁 + generation++（§4.2）
```

`rollback()` 覆盖（`vulkan_device.cpp:295`）：回滚本批 staging slice（按 upload
checkpoint）、销毁本批新建 pipeline（index ≥ `pipeline_rows_before`，缓存命中复用旧行的
不受影响）、释放 bindless/sampler 槽、销毁 image / buffer slice、`collect_retired()`
立即回收。销毁一律带 `completed_submission` 门（本批从未提交，立刻安全）。

## 6. Compile 层：八个固定 phase

入口 `compile_graph(const graph_compile_request&)`（`src/core/system.cpp:955`）。
输入 = **本帧 frame_plan + environment + 注入的后端回调**；输出 `compiled_graph_plan`
（API 无关的扁平行）。任一 phase 失败即返回累积 diagnostics 的失败结果，`compiler_state`
只是 phase 间传递的纯值聚合，不对外暴露。

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
  |   注：persistent 句柄在这里经 describe 回调换成真实 desc（§4.3 防线 [4]）
  |   注入稳定 "UploadPass"（passes[0]）+ "UploadArena" buffer
  v
[3] build_dependency_dag
  |   O(n^2) access pairing; same resource + non-read-read --> edge
  v
[4] schedule_passes
  |   Kahn 拓扑排序（最小堆 by pass index）--> 确定性顺序
  v
[5] compile_lifetimes
  |   first/last used pass per resource
  |   transient + aliasable + 生命周期不重叠 --> 物理复用（alias_handoffs）
  v
[6] compile_synchronization
  |   抽象状态跟踪 --> transition intents（layout / hazard / queue）
  |   pass prologue ops + alias handoff ops + graph epilogue ops
  |   输出 CSR 布局的 synchronization_plan
  v
[7] compile_submissions
  |   相邻同 queue pass 归并 --> queue_submission_batch
  |   跨 batch 边 --> timeline_wait；跨 queue ownership 拆 release/acquire
  v
[8] publish_compiled_plan
      plan.cache_key = recipe key ^ 环境 ^ 资源 desc ^ pass/access 明细
      fill statistics --> compiled_graph_plan
```

各 phase 实现位置（便于跳转）：

| # | phase | 位置 |
|---|-------|------|
| 1 | `validate_recipe` | `src/core/system.cpp:196` |
| 2 | `build_resource_versions` | `system.cpp:239`（含 describe 回调查句柄、`inject_stable_upload_pass`） |
| 3 | `build_dependency_dag` | `system.cpp:499` → `graph.cpp:16` |
| 4 | `schedule_passes` | `system.cpp:508` → `graph.cpp:41` |
| 5 | `compile_lifetimes` | `system.cpp:521` |
| 6 | `compile_synchronization` | `system.cpp:655` |
| 7 | `compile_submissions` | `system.cpp:798` |
| 8 | `publish_compiled_plan` | `system.cpp:872` |

### 6.1 [1] validate_recipe —— 输入把关

一句话：**"这份 recipe 格式合法吗？当前后端能跑吗？"** 只读检查，不改状态。

- 空帧 / pass 超限 → 报错；
- 逐 pass 用 `valid_range{begin,count}(size)` 检查六个行段（buffer/image access、
  attachments、copies、dispatches、draws）与 push constant 段是否越界；
- 命令与 pass 类型的兼容矩阵：raster 不能带 copy/dispatch，compute 不能带
  attachment/copy/draw，copy 不能带 attachment/dispatch/draw；每种 pass 还要过
  `capabilities.{supports_graphics, compute, copy}`；
- 失败只追加 diagnostics，"检查完所有再返回"。

复杂度 O(passes)。

### 6.2 [2] build_resource_versions —— 声明 → 事件流

一句话：**把扁平资源表/行为表摊平成编译器内部统一的 `access_event` 流**，后续阶段
只看事件流。

- **资源翻译**：每个 `frame_resource_row` 变成一个 logical 资源：
  - `persistent_*`：**describe 回调把句柄换成真实 desc**——stale 句柄在这里被拒
    （`backend_failure`，§4.3 防线 [4]）；
  - `swapchain_image`：环境给的恒定 desc（color_format/extent/PRESENT、禁止 aliasing、
    imported）；
  - `transient_*`：直接用调用者给的 desc；所有 desc 过 validate 回调并记 desc hash。
- **stable UploadPass**：`inject_stable_upload_pass=true` 时 `passes[0]` 固定为合成 copy
  pass（`backend_upload`），并造一个 imported "UploadArena" 缓冲区；随后给它注入事件：
  读 arena、写所有带 `TRANSFER_DST` 的 persistent buffer。
- **事件采集**：buffer/image access → 事件；attachments → 既是事件
  （load=read_write / clear·dont_care=write）也填充 `pass.raster`；同
  `(pass, kind, logical)` 的事件合并（usage 按位或、access 不同取 read_write）。
- **swapchain contract**：`initial = PRESENT(已初始化) 或 NONE(第一帧)`，
  `final = PRESENT`，供 §6.6 生成首帧 NONE→… 与收尾 …→PRESENT。

复杂度 O(passes × 行段跨度) + 事件合并 O(事件数)。

### 6.3 [3] build_dependency_dag —— 依赖从哪来

一句话：**同一资源两次访问之间，只要不是"读-读"，就必须有一条先后边**。

```text
for each pair (previous_event, current_event)  [O(n^2)，n = 事件数]
    skip if 同 pass / kind 不同 / logical 不同
    若 conflicts(before.access, after.access)   // 非 read-read 即冲突
        → 建边 previous.pass --> current.pass（去重）
```

直觉：read/read 可以并行，必须乱序不能的是"写要先于读完成 / 写要等读完成"。边是
"同一资源的访问链"，barrier 也贴着这条链生成（§6.6）。

复杂度 O(n²) + 去重 O(边数 × 邻接平均长)。

### 6.4 [4] schedule_passes —— 拓扑排序

一句话：**给 DAG 一个确定性的执行顺序**。

- Kahn + **最小堆**：入度 0 的 pass 进堆，每次取 `pass_handle` 最小的排入结果。
  堆保证多个"同时就绪"的 pass 字典序稳定——同一输入永远同一调度（缓存与可复现
  调试的关键）。
- 排不完 → `cycle_detected`。

复杂度 O(P + E·log P)。产物 `scheduled_passes` 是后续所有"序号"的基准。

### 6.5 [5] compile_lifetimes —— 谁能共用一块内存

一句话：**给每个逻辑资源算"死区"，死区不重叠、desc 完全一致的 transient 资源
指到同一块物理内存**——显存复用 / aliasing 的源头。

- 扫事件得到每个逻辑资源的 `first/last_used_pass`（折进调度序号）；
- 物理复用：`candidate` 是 `transient`、`aliasing != forbidden`、desc 完全相等、
  `candidate.last_order < logical.first_order` → 复用其 physical id 与 memory block，
  记 `alias_handoff{previous, next, memory_block, at_pass}`；
- 内存需求：每个非 imported physical 由 allocation 回调算
  `allocation_requirements`，作为独立 memory block；backend 按 block 分配，
  `vmaCreateAliasing{Image,Buffer}2` 叠对象（§14.5）；
- **未被任何 pass 消耗的 transient 资源**：first_order 保持哨兵，不满足上面的复用
  条件 → 开独立 physical slot + 独立 block，后端照样分配创建，不做跳过（§4.5 Q&A）。

复杂度 O(R²)。产物 `physical_resources` 也是 backend `can_reuse_plan` 跨帧整块
复用 VMA 分配的依据。

### 6.6 [6] compile_synchronization —— 生成 barrier 计划

一句话：**沿"访问链"推导每个 pass 前要插哪些 barrier，产出 CSR 布局的
`synchronization_plan`**。

对每个 scheduled pass、每个事件维护"该逻辑资源上一次的抽象状态"：

1. `before` 解析顺序：上次记录的状态 → `contract.initial_state`（首见）→ 兜底
   `{usage=0, access=read}`；
2. `after` = 事件翻译成 `abstract_resource_state{usage_bits, access, domain, queue, range}`；
3. `transition_intents(before, after, kind)`：image usage 变 → `layout_transition`；
   存在 hazard → `execution | memory dependency`；queue 变 → `queue_ownership`；
4. intents 非空 → 一个 prologue op（`scope=pass_prologue`），**phase**：同 queue → `full`；
   跨 queue → `acquire`（release 半条挪到 §6.7）；并把 before/after 状态快照进 op——
   后端只靠这一个 op 重建完整 barrier；
5. 每个 `alias_handoff` → 一个 `aliasing | memory_dependency` 的 prologue op；
6. 图收尾：带 `final_state` 的资源若末状态 ≠ final → 一条 `scope=graph_epilogue` op；
7. 打包：`ops` 扁平容器 + `prologue_begins/lengths[pass]` + `epilogue_begin/length`（CSR）。

复杂度 O(passes × 事件数)，每事件 O(1)。

### 6.7 [7] compile_submissions —— 打包提交

一句话：**把调度好的 pass 归并成 submission batch，补上跨 batch 的顺序保证**。

- 相邻且同 queue 的 pass 并到一个 batch；首个 batch `waits_for_external_acquire`
  （等 image_available），末个 `signals_external_present`；
- 跨 batch 的 DAG 边 → 一条 `timeline_wait{source_batch, source_queue, value}`（去重）
  ——编译期就把 queue 间同步显式化；
- 标了 `queue_ownership` 的 op 拆两半：`release` 挂源 batch，`acquire` 挂目标 batch，
  记 `cross_queue_dependency`——split barrier 的编译起点。

复杂度 O(P + E + ops)。

### 6.8 [8] publish_compiled_plan —— 缓存 key 与统计

一句话：**把整份 plan 浓缩成 compile 侧 cache key，并填统计字段**。

compile 侧 key 混入：`frame.cache_key`（调用者 revision）⊕ 环境 ⊕ 每个资源 desc 摘要
⊕ 每个 pass（kind/queue/name/raster 附件明细）⊕ 每个事件。**刻意排除** draw/upload
行与 push constant 内容。设备侧再叠加句柄 index+generation 等（§4.4）——两层 key
拼出最终的缓存门。

统计：pass/资源/事件/同步 op/submission batch/内存 block 计数，调用者 smoke 契约的观测点。

## 7. 每帧执行序列

`render_frame`（`vulkan_device.cpp:793`）固定执行：

```text
render(recipe)
  |
  v
[0] resize 前处理    resize_requested --> runtime.resize()；零 extent --> skipped
  v
[1] acquire
  |   wait frame fence --> 前推 completed_submission --> collect_retired（回收到期销毁）
  |   vkAcquireNextImageKHR
  |   OUT_OF_DATE --> skipped（+resize 标志）；SUBOPTIMAL --> 继续
  v
[2] recipe.build(environment) --> frame_plan（调用者回调）
  v
[3] 帧级校验（§4.3 防线 [2]/[3]）
  |   行段越界、命令/pass 兼容、persistent 句柄 find_handle、transient desc、
  |   access/attachment 资源类别 —— 任一失败整帧 failed，零录制零提交
  v
[4] lowering + 设备侧 cache key（§4.4）
  |   draw/copy/dispatch 行 --> native 行（每帧重做，不进缓存）
  v
[5] rebuild_row_graph
  |   key 命中 --> 复用 compiled_graph_plan（跳过编译与 VMA 重建）
  |   miss --> compile_graph + on_compile_resource_allocation（VMA blocks）
  v
[6] realize_resources    目前是 no-op 占位（vk_runtime.cpp:481）：
  |                       资源在 initialize/编译期已就绪，无延迟创建
  v
[7] record_batches
  |   reset pool, begin（ONE_TIME_SUBMIT）
  |   per scheduled pass：prologue barriers（CSR 索引取 op 段）
  |     --> raster：vkCmdBeginRendering/EndRendering + indirect draws
  |     --> UploadPass（backend_upload）：flush 本帧待拷 staging 拷贝
  |     --> copies / dispatches / indexed-indirect
  |   图收尾：epilogue barriers（swapchain → PRESENT_SRC_KHR）
  v
[8] submit
  |   vkQueueSubmit2：wait image_available, signal render_finished + fence
  |   commit_pending_uploads(submission) --> staging slice 到期登记
  v
[9] present
  |   OUT_OF_DATE / SUBOPTIMAL --> skipped（+resize 标志）
  v
[10] collect_retired    swapchain_initialized[image] = true
```

关键语义：

- **失败路径**：任何阶段失败统一 `abort_frame()` + `frame_result{failed, error}`，
  已录制的命令丢弃、不 submit。compile 失败时 `graph_valid` 保持旧值，下一帧 key 不同
  会重试编译。
- **稳定帧不变量**：cache 命中 → 无编译、无 VMA 分配、无 pipeline 创建、无
  descriptor update；一帧只剩 acquire → build → 校验/lower → record → submit → present。
- 当前所有 pass 落在 graphics queue（`queue_availability{compute:false, copy:false}`），
  多队列契约由编译层与 fake tests 固化，物理多队列是预留能力（§6.7 的 timeline_wait
  已在编译期生成）。

## 8. Vulkan runtime 初始化序列

`vk_runtime::initialize`（`src/backend/vulkan/vk_runtime.cpp:54`）严格按序，任一步失败
聚合 `last_error_` 并 `shutdown()` 后返回错误：

```text
[1] check surface_provider callbacks (3x present)
[2] create_instance    （debug messenger --> validation_callback --> diagnostic sink）
[3] surface = provider.create_surface(instance)
[4] select_physical_device（api >= 1.3, swapchain ext, feature 清单）
[5] create_device      （启用精确 feature 集 + VK_KHR_swapchain）
[6] create_allocator   （VMA, EXT_MEMORY_BUDGET, api 1.3）
[7] create_swapchain   （B8G8R8A8_UNORM/SRGB 优先, FIFO, 每 image view+semaphore）
[8] create_frame_rows  （每帧 cmd pool + primary buffer + semaphore + 预置 signal fence）
[9] initialize_bindless（见 §9）
之后 create_device（vulkan_device.cpp:1104）：
[10] 创建 256MB device-local arena + graph_executor.set_context(...)
```

**feature 硬性清单**（缺一即初始化失败并列出缺失项，无传统 fallback）：

- `synchronization2`、`dynamicRendering`
- `runtimeDescriptorArray`、`descriptorBindingPartiallyBound`
- `descriptorBinding{SampledImage,StorageImage,UniformBuffer,StorageBuffer}UpdateAfterBind`
- `descriptorBindingUpdateUnusedWhilePending`
- `shader{SampledImage,StorageImage,UniformBuffer,StorageBuffer}ArrayNonUniformIndexing`

## 9. Bindless 实现

### 9.1 固定五表 ABI

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

- slot 行记录 `{generation, safe_after_submission, occupied, owned_view, owned_sampler}`；
- **slot 0/1 是默认资源**：sampled_images[0]=1×1 白图、[1]=1×1 法线图；samplers[0]=linear；
  storage_images[0]、uniform/storage_buffers[0] 为清零占位。分配起点 sampled_images 从 2、
  其余从 1。shader 访问未绑定槽永远读到合法默认值——**bindless 槽没有"悬空"**；
- 所有 pipeline 共用同一 pipeline layout：单 set + push constant ranges；
  录制时统一 `vkCmdBindDescriptorSets(set 0)` 一次。

### 9.2 slot 生命周期与句柄的关系

```text
publish（apply 的 prepare/publish 阶段）
  |   线性扫表：skip occupied AND safe_after_submission > completed_submission
  |   复用 --> generation++
  |   立即 vkUpdateDescriptorSets（update-after-bind：录制中途更新也合法）
  |   apply 返回 {slot index (uint32_t), device_bindless_handle{index, generation}}
  v
use（shader 用 slot index 非均匀索引；CPU 端 device_bindless_handle 只为 retire 记账）
  v
retire（apply 的 retire 行，传 device_bindless_handle）
  |   occupied = false；safe_after_submission = next_submission
  |   collect_bindless（gate 通过后）--> 槽重写回默认资源、销毁 owned view/sampler
```

注意区分两种"句柄"：**slot index** 是 shader 用的数组下标（uint32_t，来自
`result.bindless_slots[]`）；**device_bindless_handle** 是 CPU 端凭证（来自
`result.bindless[]`），只有 retire 时用。槽的默认资源兜底意味着即使调用者丢了
bindless 句柄，shader 读到的也是合法默认值而不是 UB。

### 9.3 稳定场景零分配

1. slot 复用内嵌表内，无 free-list 堆分配；
2. 只有 publish/retire 才触发 descriptor write（`statistics.descriptor_updates` 可观测，
   稳态帧 == 0）；
3. pipeline 按 desc hash 命中缓存；
4. graph cache key 命中跳过整轮编译与 VMA 重建；重编译时 `can_reuse_plan`
   进一步复用兼容的整块 VMA allocation 与 image/buffer/view。

## 10. 资源与内存：arena + slice

```text
+------------------------------------+------------------------------------------+
| arena                              | serves                                   |
+------------------------------------+------------------------------------------+
| upload arena   64MB (lazy)         | staging slices for buffer/image uploads |
|   TRANSFER_SRC, upload, persistent |   alignment 16                          |
| device arena  256MB (at create)    | device_local + automatic buffers        |
|   TRANSFER_DST|VTX|IDX|SSBO|...    |   alignment 256                          |
| readback arena 16MB (lazy)         | readback + automatic buffers            |
|   TRANSFER_DST, readback, persist  |   alignment 64                          |
+------------------------------------+------------------------------------------+
```

buffer create 三分支（apply 的 prepare 阶段）：

```text
buffer_create row
  |-- device_local + automatic --> slice from device arena   (suballocated)
  |-- readback    + automatic --> slice from readback arena  (suballocated)
  +-- otherwise（upload / dedicated）--> vmaCreateBuffer
                                          (+ DEDICATED_MEMORY if requested)
image create --> 一律 vmaCreateImage
graph transient 资源 --> vmaAllocateMemory + vmaCreateAliasing{Image,Buffer}2
                        （物理复用计划来自 §6.5）
```

staging slice 生命周期（submission-gated 复用）：

```text
stage_buffer_upload / stage_image_upload（apply 的 prepare 阶段）
  |   slice from upload arena + memcpy + 排入 pending copy 队列
  v
UploadPass（backend_upload，永远 passes[0]）——每帧 record 时
  |   vkCmdCopyBuffer / CopyBufferToImage
  |   （图像带 barrier2：UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY）
  v
submit 成功 --> commit_pending_uploads(submission N)
  |   slice --> retired_buffer_slices {safe_after = N}
  v
collect_buffer_slices（N <= completed_submission 时）
      slice --> free_spans，merge_free_spans() 合并邻居
```

retirement 一律由 submission 序号控制：`retired_buffer_slices` / `retired_buffers` /
`retired_images` 与通用 `vk_retirement_table`（`retire()` / `collect_retired()`）。
persistent 资源不因 resize 重建；resize 只销毁 swapchain 与尺寸相关状态。

## 11. 诊断体系

- 库内不直接写 stderr/调用者日志（`BackendOwnershipContract.cmake` 强制）。
- 编译：`compile_result{diagnostics[]}`（结构化，24 种错误码，见 §6）。
- 资源变更：`resource_change_result{error, diagnostic{phase, row_kind, row_index, message}}`。
- runtime：`vk_runtime_result{.error}` / `last_error()`；frame 级 `frame_result{status, error}`。
- validation layer：`validation_callback` 把 error 计入原子计数
  （`device.validation_error_count()`），error/warning 转发调用者注入的 `diagnostic_sink`。

## 12. DX12 / Metal（预留）

> 两个 backend 当前只固化"同一公共描述可以被各自 lowering 消费"的契约，没有物理执行。
> 新 backend 落地时在本节补充其实现笔记，结构建议与 §5–§10 对齐。

- **DX12**：`src/backend/dx12/dx12_resource_lowering.h`（format/flags/heap lowering 完整）+
  `dx12_backend.h`（header-only，`on_compile_resource_allocation` 用
  `CreateCommittedResource`，但 `emit_barriers` / `begin_raster_pass` / `end_raster_pass`
  是空 stub）。INTERFACE target `render_graph::dx12`。
- **Metal**：仅 `src/backend/metal/metal_resource_lowering.h`（纯数据 lowering）。
- 共享契约测试：`render_graph.resource_description_lowering`（同一 desc 经 vk/dx12/metal
  三条 lowering 互校）；`dx12_render_graph_sample` 复用 compiler sample 源码。

## 13. 测试与契约

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

## 14. Vulkan 现代高性能特性（实现细节）

> §9/§10/§6.6 已经讲过的数据流不重复，这里补"为什么、怎么落 API、代价是什么"。
> 全部特性只依赖 Vulkan 1.3 core + `VK_KHR_swapchain`；不提供传统 fallback。

### 14.1 特性总览

| 特性 | 位置 | 收益 |
|------|------|------|
| Synchronization2（core 1.3） | `vk_barrier_lowering.h`、`vkCmdPipelineBarrier2` | 精确实例化 barrier；64 位精确 stage/access；无老式 layout 限制 |
| Dynamic Rendering（core 1.3） | `vk_backend.h:216 begin_raster_pass` | 免 render-pass/framebuffer 对象；附件动态、切换快；视图懒建 |
| Descriptor Update-After-Bind（bindless） | `vk_bindless.cpp` | 单描述符集 ABI；录制中途可更新；零 set 切换 |
| 非均匀索引（non-uniform indexing） | 强制的 feature 清单 | shader 用运行时下标访问表 |
| VMA 子分配 / aliasing | `vk_vma.cpp` + executor | 显存按块 + 逻辑复用，慢路径只在计划变化 |
| 编译期计划缓存 | `vulkan_device.cpp:rebuild_row_graph` | 稳态帧零编译、零 VMA 分配、零 pipeline 创建 |

### 14.2 编译期计划缓存（回答"缓存到底省了什么"）

`rebuild_row_graph`（`vulkan_device.cpp:564`）的完整快路径：

```text
每帧：lowering + key（O(行数)）--> key == graph_cache_key 且 graph_valid？
  | yes --> 直接复用 compiled_graph_plan：跳过 compile_graph、跳过
  |         on_compile_resource_allocation（VMA blocks）、跳过 pipeline 创建
  v no  --> compile_graph -->
            on_compile_resource_allocation（VMA 按块分配，见 §14.5）
            can_reuse_plan（vk_backend.h:838）：映射与 desc 全部兼容时
              连分配都不做，整块沿用旧 VMA allocation / 对象 / view
```

配合 §4.4（key 含句柄 index+generation）与 `view_cache`，稳态帧的内存/视图/对象
创建全部为零。

### 14.3 Bindless：单描述符集 ABI

固定五表（§9.1），**整个图、所有 pipeline 共用同一个 pipeline layout、同一个 set**：

- **零描述符集切换**：一帧无论几个 pass，绑定开销恒为 1；
- **录制中途可更新**：publish 落地为一次 `vkUpdateDescriptorSets`，slots 是
  `UPDATE_AFTER_BIND`，发布之后 recording 命令引用它都合法；
- **shader 侧**：表就是数组，运行时下标 + `nonuniformEXT`：

  ```glsl
  layout(set = 0, binding = 0) uniform sampler2D   textures[];
  layout(set = 0, binding = 3) uniform UBOData     ubo_table[];
  // …以 bindless_publish 返回的 slot 下标作为 nonuniformEXT(textures[id])
  ```

- **安全网**：slot 0/1 默认资源兜底（§9.1），访问未发布/已回收槽读到合法默认值；
- **代价/限制**：容量固定；无传统 descriptor fallback——不支持 update-after-bind 的
  设备直接拒绝初始化；`statistics.descriptor_updates` 只统计 publish/retire，稳态帧为 0。

### 14.4 Dynamic Rendering

`begin_raster_pass` 把编译期 `raster_pass_desc` 直接转成 `VkRenderingInfo`：color/formats、
load/store/clear、depth/stencil、resolve、层数全部动态决定，不创建 render pass /
framebuffer 对象：

- 附件切换 = 重新 begin，无 pipeline 与 framebuffer 匹配；跨 pass 仅一次 prologue barrier；
- **image view 按需懒建并缓存**：`get_or_create_image_view` 以 `(VkImage, desc)` 为键查
  `view_cache`；desc 允许 format override 时强制 `MUTABLE_FORMAT`；
- MSAA resolve 内联（`resolveMode=AVERAGE` + `resolveImageView`）；render area 省略时
  自动落首个附件 extent。

### 14.5 资源与内存三层分工

1. **调用者长期资源**：apply 创建，arena 子分配或 `vmaCreateBuffer/Image`（§10）；
2. **帧瞬态资源（graph-owned）**：编译期 §6.5 已给出"谁跟谁共用哪块内存"的 plan；
   `on_compile_resource_allocation`（`vk_backend.h:467`）按 `image_memory_blocks` 逐块
   `vmaAllocateMemory`，再 `vmaCreateAliasing{Image,Buffer}2` 叠对象——**共用 = 单块显存，
   绝不复制**；
3. **跨计划复用（防抖关键）**：
   - `make_block_keys`：每块内存按"块内资源 name + desc hash + lifetime"混出 key；
   - 重编译时先扫旧块，key 与 requirements 相同就整体"偷"过来；
   - 对象级 `is_compatible_native_{image,buffer}` 复核，能复用则复用旧对象，否则进
     `retired_resources{safe_after_frame = current + frames_in_flight}`，由
     `collect_retired` 在 `begin_frame` 按完成帧清理——**所有销毁都是 submission-gated**；
   - `can_reuse_plan` 在映射与全部 desc 兼容时把整块 plan 直接沿用。

### 14.6 帧同步模型

- 每帧一对 semaphore + 一个**预置 signal 的 fence**；acquire 先 `vkWaitForFences`
  （保证上一轮同槽已结束——这同时是 `completed_submission` 前推与 `collect_retired`
  的门槛），再 `vkAcquireNextImageKHR`；
- submit 挂 `wait=image_available`、`signal=render_finished+fence`；present 等
  `render_finished`。timeline 部分在编译层（§6.7 的 `timeline_wait` 已把跨 queue 依赖
  显式化），当前单图形队列下退化为 binary semaphore 通路；
- `OUT_OF_DATE / SUBOPTIMAL` 统一映射为 `skipped` 并回置 resize 标志，不提交半帧
  （acquire 失败不推进 `swapchain_initialized`，缓存语义保持单调）；
- frame rows 环形游标 + `++cursor % frames_in_flight`，3 in-flight 下 CPU 永不追 GPU。

### 14.7 组合效果与代价

- **稳态帧的不变量**：`graph_compiles`、`descriptor_updates`、`pipeline_creations`、
  VMA 分配、view 创建全部为零——一帧只做：acquire → build → 校验/lower →
  (cache hit) → record → submit → present。
- **代价（有意为之的限制）**：无传统 descriptor fallback；无 `history` 资源执行语义
  （调用者自行管理 imported history）；未真正使用独立 compute/copy 队列（编译契约与
  timeline 已就绪，executor 暂回落 graphics）；ray tracing / sparse / video queue /
  classic subpass 不在范围。

## 13. 已知设计偏差：culling 与资源惰性化（2026-08 审查）

> 本节记录当前实现与最初设计的两点偏差及实施方案。**尚未实施**，实施前请先读本节。

### 13.1 偏差一：pass culling 不存在

最初版本（`d0f0a3f` 的 `system.h`）的 compile() 步骤为：

```text
A setup callbacks --> B resource versions --> C producer map
--> D culling (outputs as roots, reverse mark active_pass_flags)
--> E validate --> F DAG --> G topo schedule
--> H lifetime + aliasing --> I barrier plan
```

当前八 phase（§4）中**没有 culling**：所有声明的 pass 都进调度，所有声明的资源都参与
生命周期与物理分配。接口重构为扁平 frame rows 时丢失了 output 根声明，culling 随之消失。

注意测试名的误导：`culling_compile`、`barrier_plan`、`deferred_rendering_compile`、
`resource_producer_map_compile` 等在 `src/unit_test/compiler_contract_test.cpp:221` 落入默认
分支，实际跑的是通用依赖测试——它们是占位名，不验证各自命名的行为。

### 13.2 偏差二：持久资源急切物化

- 帧内 transient 资源（`frame_resource_row`）是"先描述、compile 时物理分配"，符合惰性原则；
  但因无 culling，声明即分配。
- 持久资源（几何/纹理/材质）经 `apply_resource_changes` 在 prepare 阶段直接
  `vmaCreateBuffer/Image`——调用方拿到的是即时物理资源，而非"首次被活跃 graph 引用时才物化"
  的逻辑句柄。

### 13.3 实施方案（层级 A：恢复 culling）

改动集中在 `src/core`，主仓无感：

1. `frame_pass_row` 增加活性根语义：写 swapchain attachment 的 pass、带副作用的 pass
   （backend_upload、写 imported/persistent 资源）天然为根；其余可加显式 `side_effect` 位。
2. 在 `build_dependency_dag` 之后插入 `cull_passes` phase：从根反向遍历标活跃集；
   `schedule_passes` / `compile_lifetimes` / `compile_synchronization` 只处理活跃 pass。
3. 被剔除 pass 独占的 transient 资源不进入生命周期计算，自然不参与物理分配——
   graph 资源恢复"culling 之后才物化"。
4. 做实 `culling_compile` 测试：声明不被任何根消费的 pass + transient 资源，
   断言 pass 被剔除且资源无物理分配；其余占位测试名同理补齐或删除。

### 13.4 实施方案（层级 B：持久资源逻辑句柄化，未评估）

`apply_resource_changes` 的 create 行只登记逻辑资源并返回句柄，物理创建推迟到首次被
编译后的活跃 graph 引用。难点：upload 行需要物理目标才能 stage；事务（§9）需拆成
"逻辑提交 + 按需物化"两段；retire/回滚语义随之调整。属 `render_device` ABI 级变更，
是否值得做取决于资产是否需要参与逐帧裁剪，实施前需单独评审。

## 暂不支持

Ray tracing acceleration structures、sparse resources、video queues、classic subpass。
独立 compute/copy queue 的物理使用（compile 契约已就绪，Vulkan executor 暂全部回落 graphics）。
