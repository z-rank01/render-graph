# Render Graph

一个 API-agnostic 的 C++20 Render Graph。Core 负责编译资源依赖、subresource/range 状态、生命周期、aliasing、同步与多队列 submission plan；backend 负责把计划映射到 Vulkan/DX12。

## 能力边界

- 强类型 `image_handle`、`buffer_handle`、`pass_handle`。
- 有序 pass access event，支持 mip/layer/aspect 与 buffer byte range。
- transient/imported/persistent/history 生命周期与 object reuse、memory alias 两类计划。
- prologue、pass-internal、alias handoff、epilogue 同步计划。
- 唯一的 pass 内显式同步入口 `explicit_barrier(span)`；不提供 unsafe/native barrier。
- Vulkan Synchronization2、VMA allocation、view cache、延迟销毁与 Dynamic Rendering。
- graphics/compute/copy queue 计划、timeline wait/signal、release/acquire ownership transfer。
- 结构化 compile/execute diagnostics、规模限制、统计与确定性 `debug_dump()`。

平台层仍负责 instance/device/surface、swapchain acquire、command buffer 提交和 present；pipeline、shader、descriptor 由 renderer 持有。

## 基本流程

```cpp
render_graph_system<vk_backend> graph;
graph.set_backend_context(context);
graph.begin_frame(frame_index, completed_frame, cache_key);

graph.add_raster_pass("Draw", setup, execute);
if (graph.needs_recompile()) {
    const auto result = graph.compile();
    // inspect result.diagnostics on failure
}

graph.bind_imported_image(swapchain_image, acquired_image);
const auto result = graph.execute(command_context);
if (result && submit_succeeded) {
    graph.commit_frame();
} else {
    graph.abort_frame();
}
```

多队列调用方读取 `get_submission_plan()`，为每个 batch 提供对应 queue 的 command context，并通过 `execute_batches()` 录制。外部 acquire/present semaphore 的接入位置由 batch 标志给出。

## 构建与测试

```powershell
cmake -S . -B build -DRENDER_GRAPH_BUILD_UNIT_TESTS=ON -DRENDER_GRAPH_BUILD_SAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

单元测试覆盖 compile validation、subresource dependencies、aliasing、同步 lowering、显式 barrier、VMA/views、Dynamic Rendering、帧事务、VulkanSample 等价图、多队列与固定 seed 压力回归。

## 暂不支持

Ray tracing acceleration structures、sparse resources、video queues 和 classic Vulkan subpass 不在当前范围内。Vulkan backend 可运行；DX12 backend 保持同一 concept 和可编译实现，但仍需要更完整的运行时验收。
