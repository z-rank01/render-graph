# Render Graph 开发说明

## 当前架构

Core 公共描述、hash、compatibility、range 与同步编译均与 native API 无关。资源能力校验通过
`resource_validation_api` function table 显式注入；`render_graph_system<BackendT>` 仅在物理资源实现和命令
执行边界持有 backend adapter。一次 compile 按以下固定阶段完成：

1. 执行 pass setup，生成有序 `pass_access_event`。
2. 校验 handle、range、read-before-write、raster attachment 与规模限制。
3. 生成 resource version、producer map、culling DAG 和确定性拓扑序。
4. 分析 subresource/range 生命周期，编译 object reuse 与 memory alias 计划。
5. 生成 pass prologue、pass-internal、alias handoff、graph epilogue 同步计划。
6. 按 queue class 生成 submission batches、timeline waits 与 release/acquire ownership transfer。
7. 发布确定性的 compiled state，再由 backend 实现或复用物理资源；backend 错误转换为 `backend_failure` diagnostic。

成功 compile 后可通过 `get_statistics()` 获取规模统计，通过 `debug_dump()` 输出稳定的 schedule、资源生命周期/物理映射、同步和 submission 信息。

## Pass 内显式 Barrier

唯一公共入口是：

```cpp
void pass_execute_context::explicit_barrier(
    std::span<const explicit_transition> transitions);
```

setup 中 `read/write` 的顺序定义 pass 内状态链。compile 预生成 internal transitions；execute 时 `explicit_barrier()` 必须按顺序消费下一组 transition。pass 结束会校验是否全部消费并到达 compile 声明的 final state。漏调用、重复、乱序、资源或状态不匹配均返回 `execute_result` 错误。

不提供 unsafe/native barrier。Render Device 的 `frame_recipe` 通过扁平 pass/resource/access/command rows
描述 raster、compute 与 copy；recipe 不能取得 native command buffer。旧模板 graph executor 只作为 backend
内部 lowering 设施。

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

- `begin_frame(frame, completed, cache_key)` 开启事务。
- execute 成功且平台 submit 成功后调用 `commit_frame()`；失败、out-of-date 或中止调用 `abort_frame()`。
- cache key 变化触发 recompile；兼容的 physical allocation/view 会选择性复用。
- persistent/history 资源携带跨帧 initial/final state。
- `submission_plan` 标注每个 queue batch、timeline value、wait、ownership transfer 以及外部 acquire/present 接点。
- 设备缺少独立 compute/copy queue 时确定性回退到 graphics queue。

## 诊断与硬化

用户输入错误不依赖 `assert`。`compile_result` 覆盖 no-output、越界、read-before-write、cycle、attachment mismatch、pass/image/buffer/access 上限、backend failure 和 unsupported feature 类别；内部不变量仍使用 assert。

固定 seed 压力测试构造 96-pass DAG，并随机选择额外依赖与 image mip/layer range，验证重复 compile 的 dump 完全一致。allocator、barrier emission、native binding 等失败路径都有明确 execute/compile diagnostic。

## 完成状态

- Step 0–1：可构建测试入口、结构化错误、重复 compile 与确定性。
- Step 2–4：有序 subresource access、生命周期/aliasing、完整抽象同步编译器。
- Step 5–8：tracked explicit barrier、Vulkan Synchronization2、VMA/views、Dynamic Rendering。
- Step 9–11：VulkanSample 单队列迁移、帧事务/resize/history、多队列 submission plan。
- Step 12：dump/statistics、限制与 backend diagnostics、固定 seed stress、文档与旧路径清理。

## 已知边界

- 不处理 ray tracing acceleration structures、sparse resources、video queues 或 classic Vulkan subpass。
- DX12/Metal 只提供公共 lowering/command contract；本轮可运行 runtime 是 Vulkan。
- ray tracing、sparse resources、video queues、classic Vulkan subpass、GPU culling 与 mesh shader 不在范围内。
- public package 只安装 `include/render_graph/`、targets 与 package config；`tests/install_consumer` 必须能在
  全新 build tree 中只依赖安装前缀完成编译链接。
