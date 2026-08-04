# Render Graph 开发笔记

## 当前状态

这个子模块已经不只是“准备进入 barrier”阶段了：`compile()` 中已经完成了资源依赖收集、resource version 生成、producer map、culling、DAG 构建、拓扑排序、生命周期分析、aliasing、API-agnostic barrier plan 生成，以及 compile 末尾的物理资源分配回调。

目前真正还没有落地的是 backend 侧的 barrier lowering：`execute()` 已经在每个 pass 执行前调用 `backend.apply_barriers(pass, per_pass_barriers)`，但 Vulkan/DX12 backend 里的 `apply_barriers()` 仍然是空实现或者 TODO。也就是说，核心层已经生成了 barrier 数据，下一步要做的是把抽象 barrier 转换成具体 Graphics API 的 command buffer / command list 操作。

## 模块边界

- `render_graph_system<BackendT>` 是核心入口，使用模板 backend concept，而不是虚函数接口。
- `pass_setup_context` 负责声明资源、读写依赖和 output root。
- `pass_execute_context` 目前只暴露 `resources`，用于通过 logical handle 获取 backend native resource。
- `resource.h` 保存资源 handle、version handle、meta table、producer map、lifetime、logical-to-physical mapping。
- `graph.h` 保存 pass 的 read/write dependency 和 DAG CSR 数据。
- `barrier.h` 保存 API-agnostic 的 barrier op 与 per-pass barrier plan。
- `vk_backend.h` / `dx12_backend.h` 负责 desc hash、兼容性判断、资源创建、imported binding、logical-to-physical native resource 查询；barrier lowering 还未完成。

## Compile Pipeline

### Step A: Invoke Setup Functions

遍历 `graph.passes`，调用每个 pass 的 setup function。setup 阶段只记录声明，不执行 GPU 工作。

写入的数据：

- `meta_table.image_metas` / `meta_table.buffer_metas`
- `image_read_deps` / `image_write_deps`
- `buffer_read_deps` / `buffer_write_deps`
- `output_table`

资源读写依赖使用一维数组表达多维关系：

- `read_list` / `write_list` 是所有 pass 的资源访问连续存储。
- `usage_bits` 与 list 同索引，记录每次访问的 usage。
- `begins[pass] + lengthes[pass]` 表示某个 pass 的访问区间。

这种布局本质上是 CSR/SoA 风格：对 pass 查依赖时是连续区间，适合 cache，也避免 `vector<vector<T>>` 的大量小分配。

### Step B: Compute Resource Version

setup 阶段用户只看到 `resource_handle`，compile 阶段内部会为每次 write 生成版本。

核心规则：

- `resource_handle` 是低 32 位 index。
- `version_handle` 是高 32 位 version。
- `resource_version_handle` 是 `uint64_t`，通过 `pack(index, version)` 合成。
- `unpack_to_resource()` 取低 32 位。
- `unpack_to_version()` 取高 32 位。
- packed handle 不能直接当 vector index，必须先 unpack。

读访问会读取当前 latest version；写访问会创建新的 version。若读发生在任何内部 write 之前，则 version 为 `invalid_resource_version`，之后 validation 会要求它必须是 imported resource。

### Step C: Build Resource-Producer Map

建立 version 到 producer pass 的反查表。

仍然使用一维数组表达二维数据：

- `img_version_offsets[image]` 给出 image 的 version 起始位置。
- `img_version_producers[offset + version]` 得到该 image/version 的 producer pass。
- `latest_img[image]` 存每个 image 当前最新版本。
- buffer 对应 `buf_version_offsets` / `buf_version_producers` / `latest_buf`。

这样避免把 packed `resource_version_handle` 当稀疏 key 使用，也避免 unordered map 成为核心路径。

### Step D: Culling

从 declared outputs 作为 roots，反向追踪 producer：

1. output resource 的 latest version producer 入队。
2. live pass 读取的 resource version producer 继续入队。
3. 没有通向 output 的 pass 会被标记为 inactive。

产物是 `active_pass_flags`。

### Step E: Validate Resource

当前 validation 主要覆盖：

- output 至少有一个。
- active pass 的 read/write resource handle 不越界。
- non-imported resource 不允许 read-before-write。
- write 生成的 version handle 必须有效。

注意：这些检查目前依赖 `assert`，release build 下不会给用户可恢复的错误结果。

### Step F: DAG Construction

根据 active pass 的 read dependency 查询 producer，建立 producer -> consumer 的 pass edge。

DAG 同样用 CSR 表达：

- `adjacency_list` 存所有边的 dst pass。
- `adjacency_begins[pass]` 与 `adjacency_begins[pass + 1]` 表示 pass 的 outgoing edge range。
- `in_degrees` / `out_degrees` 用于 scheduling 和调试。

同一 producer 的边会排序并去重。

### Step G: Scheduling

使用 Kahn 算法对 active DAG 做拓扑排序，结果写入 `sorted_passes`。若排序结果数量不等于 active pass 数量，则说明存在 cycle。

### Step H: Lifetime Analysis & Aliasing

先将 pass handle 映射到拓扑执行序号，然后计算每个 logical resource 的 first/last use。

之后做 greedy first-fit aliasing：

- imported resource 不参与 alias，分配独立 physical id。
- transient resource 若 lifetime 不重叠，并且 backend desc hash 相等且 `is_compatible_*()` 返回 true，就可以复用同一个 physical id。
- `physical_resource_meta.physical_*_meta` 保存 physical id 对应的代表 logical resource。
- `handle_to_physical_*_id[logical]` 保存 logical 到 physical id 的映射。

这个阶段完成的是 logical resource 到 physical resource 的规划，不直接等同于 GPU API object handle。

### Step I: Build Synchronization Plan

当前核心层已经生成 per-pass barrier plan。

流程：

1. 按 `sorted_passes` 遍历执行顺序。
2. 聚合每个 pass 内每个 logical resource 的 read/write 标志和 usage bits。
3. 通过 logical-to-physical id 找到实际复用的 physical resource。
4. 维护每个 physical id 的 `last_use`。
5. 如果 physical id 被新的 logical resource 复用，则插入 aliasing barrier。
6. 如果 usage/access 相比 last use 改变，则插入 transition barrier。
7. 如果 storage buffer/image 存在 write -> read/write 的顺序需求，则插入 UAV-like barrier。
8. 最后将 scratch `vector<vector<barrier_op>>` flatten 到 `per_pass_barrier` 的 CSR + SoA 表。

`per_pass_barrier` 的存储方式：

- `pass_begins[pass] + pass_lengths[pass]` 表示某个 pass 的 barrier 区间。
- `types` / `kinds` / `logicals` / `physicals` / `src_*` / `dst_*` / `prev_logicals` 是并行数组。

### Step J: Physical Resource Allocation

调用 `backend.on_compile_resource_allocation(meta_table, physical_resource_metas)`。

backend 负责：

- 保存 logical-to-physical mapping。
- 为 transient physical resource 创建 native resource。
- 为 imported resource 绑定用户传入的 native handle。

当前 Vulkan/DX12 backend 已经有原型实现，但资源池、跨帧重用、销毁策略还没有完整工程化。

## Execute Pipeline

当前执行流程很简单：

1. 构建 `pass_execute_context`。
2. 遍历 `sorted_passes`。
3. 每个 pass 前调用 `backend.apply_barriers(pass, per_pass_barriers)`。
4. 调用用户的 pass execute function。

这里的设计意图是：核心层只知道抽象 barrier，不知道 Vulkan/DX12/Metal 的具体 barrier 命令；backend 根据 `per_pass_barriers` 的 pass range 把 barrier lower 到当前 API。

## 已完成的数据结构设计重点

### 一维数组表达多维关系

当前多处使用 begin + length / offset table 将多维数据压成一维：

- pass -> read resources: `read_dependency::{read_list, usage_bits, begins, lengthes}`
- pass -> write resources: `write_dependency::{write_list, usage_bits, begins, lengthes}`
- resource -> versions: `version_producer_map::*_version_offsets`
- pass -> outgoing edges: `directed_acyclic_graph::{adjacency_list, adjacency_begins}`
- pass -> barriers: `per_pass_barrier::{pass_begins, pass_lengths}`

优点是布局紧凑、遍历 cache 友好、容易序列化和调试；缺点是插入/删除中间元素不方便，需要清晰维护 begin/length 的一致性。

### Index + Version 位级合并

`resource_version_handle` 将 resource index 与 version pack 到同一个 `uint64_t`：

```cpp
high 32 bits: version_handle
low  32 bits: resource_handle
```

这个设计让“某个资源的某个版本”可以被作为一个值传递、比较和存储。核心约束是：packed handle 只用于标识，不用于直接索引数组。所有数组仍然以 unpack 后的 `resource_handle` 或 `offset + version` 索引。

## 当前架构优点

- 核心层与 Graphics API 解耦较好，backend 以 compile-time concept 注入，不需要虚函数。
- setup/execute 分离清晰，compile 可以在真正 GPU 执行前完成依赖分析。
- 数据布局偏 DOD/SoA，适合 render graph 这种批量分析型流程。
- resource version 让多次写同一 logical resource 的 producer/consumer 关系更明确。
- culling、DAG、topo sort、lifetime、aliasing、barrier plan 已经形成完整 compile 管线。
- barrier 使用 API-agnostic op，方向正确：核心层负责“何时需要同步”，backend 负责“如何同步”。
- 单元测试已经覆盖 deferred graph、producer map、resource generation、culling、DAG、cycle、lifetime aliasing、barrier plan 等关键 compile 行为。

## 当前架构风险与不足

- `apply_barriers()` 尚未真正 lower 到 Vulkan/DX12，因此 barrier 目前只是计划数据，不会产生真实 GPU synchronization。
- `pass_execute_context` 没有 command buffer / command list / encoder 信息。backend 若要在 `apply_barriers()` 里录命令，要么内部保存当前 command context，要么需要扩展 execute 接口。
- barrier 状态仍然较粗：只有 resource-level usage/access，没有 subresource、mip、array layer、queue family/queue ownership、pipeline stage 精度。
- `pipeline_domain` 目前基本都是 `any`，domain 变化还没有被真实建模。
- pass 内 read/write 被聚合到一个 resource-level access，无法表达同一 pass 内不同 phase 或 subresource 的顺序。
- 首次使用 `last.valid == false` 时当前不会插入 initial transition。transient resource 从 undefined/common 到第一次实际 usage、imported resource 从外部 state 到 RG usage，都需要更明确的 initial state 设计。
- same-state write-after-write、attachment write 之间是否需要 memory dependency，目前交给 backend 或隐含 pass boundary，后续需要明确规则。
- `std::unordered_map` 用于 pass 内资源聚合，barrier 输出顺序可能不稳定；如果需要可复现日志/测试，最好改为临时数组或排序 vector。
- validation 依赖 `assert`，不适合 runtime-facing 错误报告。
- （完成）`resource_lifetime::clear()` 目前只清 image vectors，没有清 buffer vectors；虽然 compile 后面会重新 assign buffer lifetime，但这个函数语义容易让人踩坑。
- imported resource 的生命周期、外部初始/最终状态、跨帧 backbuffer 状态还没有形成正式 contract。

## Barrier 如何落到 Execute 层

推荐保持现在的大方向：核心层创建抽象 barrier plan，backend 在 execute 阶段 lower。不要让 core include Vulkan/DX12 headers，也不要在 `barrier_op` 里放 Vk/D3D12 类型。

### 1. Core 继续负责创建 API-agnostic barrier

`compile()` 中的 Step I 应该继续产出这样的信息：

- barrier type: transition / uav / aliasing
- resource kind: image / buffer
- logical handle
- physical id
- previous logical handle for aliasing
- src/dst abstract usage bits
- src/dst abstract access
- src/dst abstract pipeline domain

核心层只回答：“pass N 执行前，physical resource X 从上一次使用到本次使用需要什么抽象同步？”

### 2. Backend 负责维护 native state cache

backend 应维护每个 physical resource 的 native 状态：

- Vulkan: image layout、access mask、stage mask、queue family index；buffer 则主要是 access/stage。
- D3D12: `D3D12_RESOURCE_STATES`。
- Metal: resource usage、encoder boundary、fence/event 等。

这个 state cache 可以由 compile plan 初始化，也可以由 execute 的 first-use barrier lazy 初始化。core 不应该知道这些 native enum。

### 3. Execute 需要一个 command context

当前 `execute()` 没有接收命令录制上下文，这会阻塞真正 barrier lowering。可以考虑两种接口：

方案 A：backend 持有当前 frame command context。

```cpp
backend.begin_execute(command_context);
system.execute();
backend.end_execute();
```

优点是 `render_graph_system::execute()` 签名不变；缺点是 backend 有更多隐式状态。

方案 B：让 `execute()` 接收 backend-defined command context。

```cpp
template <typename CommandContextT>
void execute(CommandContextT& command_context)
{
    backend.apply_barriers(command_context, pass, per_pass_barriers);
    graph.execute_funcs[pass](exec_ctx);
}
```

优点是 command context 显式；缺点是 `pass_execute_context` 也要能访问它，否则用户 pass execute 仍然无法录制 draw/dispatch。

更推荐 B 的变体：把 command context 放入 `pass_execute_context`，但类型仍由 backend 定义：

```cpp
using command_context = typename BackendT::command_context;

struct pass_execute_context
{
    command_context* commands = nullptr;
    resource_access resources;
};
```

这样 core 只依赖 `BackendT::command_context` 这个抽象类型名，不依赖具体 Graphics API。

### 4. 每个 pass 执行前插入 barrier

backend lowering 的基本逻辑：

```cpp
void apply_barriers(command_context& cmd, pass_handle pass, const per_pass_barrier& plan)
{
    const uint32_t begin = plan.pass_begins[pass];
    const uint32_t end = begin + plan.pass_lengths[pass];

    for (uint32_t i = begin; i < end; i++)
    {
        switch (plan.types[i])
        {
        case barrier_op_type::transition:
            // map abstract usage/access/domain to native state/layout/stage/access
            // emit native transition barrier
            break;
        case barrier_op_type::uav:
            // emit UAV/global memory barrier
            break;
        case barrier_op_type::aliasing:
            // emit aliasing barrier / memory alias dependency
            break;
        }
    }
}
```

Vulkan backend 可将多条 op batch 成 `VkDependencyInfo` 后调用 `vkCmdPipelineBarrier2`。DX12 backend 可收集 `D3D12_RESOURCE_BARRIER` 后调用 `ID3D12GraphicsCommandList::ResourceBarrier`。

### 5. 需要补充 initial/final state contract

barrier 需要知道“前后资源状态”，当前 plan 能知道 RG 内部的 last use，但还缺两个边界：

- initial state：resource 在第一次进入 RG 时是什么状态。
- final state：resource 离开 RG 后应该停在什么状态。

建议新增：

- transient resource 默认 initial 为 undefined/common，第一次使用前允许 discard transition。
- imported resource 在 `bind_imported_*()` 或 `create_*()` 时传入 external initial state。
- output resource 可声明 desired final state，例如 present、shader read、copy src。
- backend 在 execute 结束时为 declared outputs 插入 final transition，或在最后一次使用 pass 前生成 final barrier。

为了保持 API 无关，core 可以定义 abstract external state：

```cpp
struct imported_resource_state
{
    uint32_t usage_bits = 0;
    access_type access = access_type::read;
    pipeline_domain domain = pipeline_domain::any;
};
```

backend 再把它转换成 native state。

### 6. Barrier 不应该是“资源对象”

barrier 更适合作为 compile 产出的 command plan，而不是像 image/buffer 一样创建成 resource。它依赖前后访问关系，生命周期是“某个 pass 执行前的一段同步命令”，不是图里的长期资源。

因此推荐保持：

- resource: 用户声明、可被 read/write、可 alias、可映射到 physical GPU object。
- barrier: compile 推导出的 per-pass op list，由 backend 在 execute 前消费。

这样 barrier 不会污染资源系统，也不会与具体 Graphics API 耦合。

### 7. Pass 内显式 Barrier（计划）

对于确实无法合理拆成多个 pass、需要在同一个 pass 内表达多个有序访问阶段的场景，计划只在 context 的 public API 中新增一个 `add_explicit_barrier(...)` 入口。这个入口属于 Render Graph 可追踪的显式同步声明，不是绕过 Render Graph 状态追踪的 unsafe/native barrier。

设计约束：

- barrier 使用 API-agnostic 的 resource state 描述，例如 usage、access、pipeline domain，以及后续扩展的 subresource range；core 不接收 `VkImageLayout`、`VkAccessFlags` 或 `D3D12_RESOURCE_STATES`。
- 同一 pass 内的 explicit barrier 按声明顺序记录，compile 必须验证完整状态链：`pass initial state -> user explicit barriers -> pass final state`。
- 相邻 barrier 的前后状态必须兼容；第一条 barrier 的输入状态必须与 pass initial state 兼容，最后一条 barrier 的输出状态必须与 pass final state 兼容。
- layout 不变但存在 write -> read/write 可见性需求时，仍然要保留 memory dependency，不能只比较 image layout。
- backend 只负责把已经验证的 explicit barrier lower 到实际 command context 中对应的同步位置。
- 当前阶段不提供 unsafe barrier，也不允许用户通过 Render Graph API 注入无法被 core 追踪的原生 barrier。无法使用 explicit barrier 表达的情况暂时继续拆分为多个 pass。

该计划只增加一个受追踪的 public explicit-barrier 入口，不在当前阶段继续扩展 phase API 或 unsafe pass API。

## 下一步建议

1. 给 `BackendT` 增加 `command_context` 概念，并让 `execute()` / `pass_execute_context` 显式携带它。
2. 在 context 的 public API 中增加受 core 跟踪的 `add_explicit_barrier(...)`，记录 pass 内 barrier 顺序并验证 initial-to-final 状态链；暂不增加 unsafe barrier。
3. 在 core 层补 initial/final abstract state contract，特别是 imported swapchain/backbuffer。
4. 在 Vulkan backend 中实现 `image_usage` / `buffer_usage` 到 layout/stage/access 的映射，并用 `vkCmdPipelineBarrier2` lower `per_pass_barrier`。
5. 在 DX12 backend 中实现 usage 到 `D3D12_RESOURCE_STATES` 的映射，并 lower transition/UAV/aliasing barrier。
6. 将 pass 内 barrier 聚合从 `unordered_map` 改成稳定顺序的数据结构，便于 debug 和测试。
7. 把 validation 从纯 `assert` 逐步升级为可返回错误信息的 compile result。
8. 扩展 subresource 级 resource access：mip、array layer、aspect，避免全资源 barrier 过保守。
