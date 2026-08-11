# Render Graph

一个 API-agnostic 的 C++20 Render Graph。Core 负责编译资源依赖、通用 Render Device/资源/命令协议、subresource/range 状态、生命周期、aliasing、同步与多队列 submission plan；backend 负责物理设备与执行 lowering。

## 能力边界

- 强类型 `image_handle`、`buffer_handle`、`pass_handle`。
- 有序 pass access event，支持 mip/layer/aspect 与 buffer byte range。
- transient/imported/persistent/history 生命周期与 object reuse、memory alias 两类计划。
- prologue、pass-internal、alias handoff、epilogue 同步计划。
- 唯一的 pass 内显式同步入口 `explicit_barrier(span)`；不提供 unsafe/native barrier。
- Vulkan Synchronization2、VMA allocation、view cache、延迟销毁与 Dynamic Rendering。
- graphics/compute/copy queue 计划、timeline wait/signal、release/acquire ownership transfer。
- 结构化 compile/execute diagnostics、规模限制、统计与确定性 `debug_dump()`。
- opaque `render_device` + function table，通用 resource change rows、frame recipe 与 indexed indirect command rows。
- 完整 Vulkan 1.3 device/surface/swapchain/frame lifecycle、全局 bindless、pipeline cache 和 acquire→present phases。
- device-local 大 buffer arena + logical slice、staging free-span 与 completed-submission 延迟复用。

宿主只通过 `surface_provider` 提供窗口系统扩展、surface 创建和 drawable extent。Vulkan backend 拥有 instance/device、swapchain、VMA、descriptor、pipeline、command、submit/present；它不依赖 Engine、SDL、glTF、GLM、应用 shader 路径或宿主 logger。

Targets：

- `render_graph_core` / `render_graph::core`
- `render_graph_vulkan_backend` / `render_graph::vulkan`
- `render_graph_dx12_backend` / `render_graph::dx12`
- `render_graph_metal_backend` / `render_graph::metal`

## 基本流程

```cpp
auto created = render_graph::vulkan::create_device(config);
auto resources = created.device.apply_resource_changes(change_batch);
auto frame = created.device.render(frame_recipe);
```

`frame_recipe` 只输出 API 无关的 graph/command rows，不能取得 `VkCommandBuffer`。Vulkan backend 内部执行 acquire、recipe build、graph compile/cache、resource realization、command lowering、submit、present 与 retirement。

## 构建与测试

```powershell
cmake -S . -B build -DRENDER_GRAPH_BUILD_UNIT_TESTS=ON -DRENDER_GRAPH_BUILD_SAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

单元测试覆盖 compile validation、subresource dependencies、aliasing、同步 lowering、显式 barrier、VMA/views、Dynamic Rendering、帧事务、VulkanSample 等价图、多队列与固定 seed 压力回归。

## 暂不支持

Ray tracing acceleration structures、sparse resources、video queues 和 classic Vulkan subpass 不在当前范围内。Vulkan backend 可运行；DX12 backend 保持同一 concept 和可编译实现，但仍需要更完整的运行时验收。
