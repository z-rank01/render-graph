# Render Graph 开发说明

## 当前架构

Core 公共描述、hash、range 与同步编译均与 native API 无关。资源能力校验、持久资源描述查询和
allocation requirements 通过显式 function table 注入。公共入口是非模板
`compile_graph(const graph_compile_request&)`，一次 compile 按以下固定阶段完成：

1. `validate_recipe` 校验 resource/pass/access/command row 和能力限制。
2. `build_resource_versions` 将 frame rows 规范化为逻辑资源和 pass access rows。
3. `build_dependency_dag` 与 `schedule_passes` 生成确定性执行顺序。
4. `compile_lifetimes` 编译 object reuse、memory block 和 alias handoff。
5. `compile_synchronization` 生成 pass prologue 与 graph epilogue 同步行。
6. `compile_submissions` 按 queue class 生成 batch 与 timeline waits。
7. `publish_compiled_plan` 发布 backend 可消费的 `compiled_graph_plan`。

`compiler_state`、DAG 和临时表只存在于 `src/core/`；公共头只公开 request、compiled rows、diagnostics
和 Render Device ABI。`render_graph::core` 是 STATIC/SHARED library，不是 header-only target。

## Pass 与 Command Contract

Render Device 的 `frame_recipe` 通过扁平 resource/pass/access/attachment/copy/dispatch/indirect rows 描述帧。
recipe 不能取得 native command buffer，也不提供 unsafe/native barrier。Core 编译同步计划；Vulkan executor
在对应 pass 边界 lowering barriers、Dynamic Rendering 和 command rows。

## Vulkan Backend

- `resource_state` 纯函数 lower 到 Synchronization2 stage/access/layout/range。
- barrier batch 通过 `vkCmdPipelineBarrier2` 发出；layout 相同的 RAW/WAW 仍保留 memory dependency。
- transient image/buffer 由注入的 VMA dispatch 创建，支持真实 memory alias、allocation reuse 与延迟销毁。
- imported native handle 可逐帧 rebind；image view 按 format/range/type 缓存。
- raster pass 使用 Vulkan 1.3 Dynamic Rendering，callback 外自动 begin/end rendering。
- Vulkan runtime 拥有 instance/device、swapchain、frame sync、VMA、bindless descriptor、pipeline cache、
  command recording、submit/present 与 completed-submission retirement。
- 帧执行固定经过 acquire、realize_resources、record_batches、submit、present、collect_retired；零 extent
  resize 返回 skipped，不 acquire 旧 swapchain。

## 帧与多队列 Contract

- frame recipe 的结构 hash、extent、format 或 swapchain initial state 变化触发 recompile；上传数量和 draw 数量不进入结构 key。
- 编译计划变化时，Vulkan executor 选择性复用兼容的 physical allocation/view。
- persistent/history 资源携带跨帧 initial/final state。
- `submission_plan` 标注每个 queue batch、timeline value、wait、ownership transfer 以及外部 acquire/present 接点。
- 设备缺少独立 compute/copy queue 时确定性回退到 graphics queue。

## 诊断与硬化

用户输入错误不依赖 `assert`。`compile_result` 覆盖 no-output、越界、read-before-write、cycle、attachment mismatch、pass/image/buffer/access 上限、backend failure 和 unsupported feature 类别；内部不变量仍使用 assert。

Core contract tests 验证稳定 schedule/hash、aliasing、同步、queue fallback 和错误 diagnostics；allocator、
barrier lowering、native range 与 Render Device 生命周期由 Vulkan contract tests 覆盖。

## 目录边界

- `include/render_graph/`：稳定 API、frame/compiler rows、diagnostics、Render Device 和可选 backend factory。
- `src/core/`：`compiler_state`、dependency graph 与固定 free-function phases。
- `src/backend/vulkan/`：VMA、资源、bindless、pipeline、barrier/command lowering 和 acquire→present phases。

## 已知边界

- 不处理 ray tracing acceleration structures、sparse resources、video queues 或 classic Vulkan subpass。
- DX12/Metal 只提供公共 lowering/command contract；本轮可运行 runtime 是 Vulkan。
- ray tracing、sparse resources、video queues、classic Vulkan subpass、GPU culling 与 mesh shader 不在范围内。
- public package 只安装 `include/render_graph/`、targets 与 package config；`tests/install_consumer` 必须能在
  全新 build tree 中只依赖安装前缀完成编译链接。
