# Render Graph 使用示例（Cookbook）

> as-built，2026-08。本文面向宿主编程：只依赖公共头 `include/render_graph/`，
> 展示三类核心用法 —— ① 创建 device；② `apply_resource_changes` 批量资源变更；
> ③ 每帧 `render`（recipe → `frame_plan`）。
> 算法与实现细节见 `ArchitectureAndInternals.md`（§4 compile 流水线、§5 每帧执行、
> §6–9 Vulkan 实现、§13 现代特性）。

## 0. 引入方式

CMake 消费（构建树内或 `find_package(render_graph)` 安装后均可）：

```cmake
find_package(render_graph CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE render_graph::core render_graph::vulkan)
# RENDER_GRAPH_ENABLE_VULKAN=OFF 时不存在 render_graph::vulkan target，
# 需按 RENDER_GRAPH_HAS_VULKAN 宏裁剪厂商专属代码（见 tests/install_consumer）。
```

```cpp
#include <render_graph/render_device.h>      // render_device, frame_plan, 资源变更行
#include <render_graph/compiler.h>           // compile_graph（一般不需要直接调用）
#include <render_graph/backend/vulkan/device.h> // render_graph::vulkan::create_device
```

`device.h` 只有在启用了 Vulkan 后端时才可用：把 `render_graph::vulkan` 靶子链进可执行文件，
并按需给自己定义 `RENDER_GRAPH_HAS_VULKAN=1`（参考 `tests/install_consumer/CMakeLists.txt`），
用 `#if RENDER_GRAPH_HAS_VULKAN` 包裹厂商专属部分，保证纯 core 编译也能过。

## 1. 第一步：创建 Device

`render_graph::vulkan` 不需要宿主做任何 Vulkan 初始化 —— 唯一义务是实现
`surface_provider`（`include/render_graph/backend/vulkan/surface_provider.h`）：
给库提供 instance 扩展名列表、创建 `VkSurfaceKHR` 的回调、以及当前可绘制尺寸。

```cpp
using render_graph::vulkan::surface_provider;

// ① instance 扩展：把宿主已有窗口系统（如 SDL）要求的扩展名暴露给库。
//    库自身需要的扩展（debug_utils、KHRONOS_validation 等）由库自行追加。
bool provide_extensions(void* state, const char* const*& names, uint32_t& count, std::string& error)
{
    auto* window = static_cast<SDL_Window*>(state);
    unsigned int sdl_count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &sdl_count, nullptr))
        { error = SDL_GetError(); return false; }
    static std::vector<const char*> exts;
    exts.resize(sdl_count);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &sdl_count, exts.data()))
        { error = SDL_GetError(); return false; }
    names = exts.data(); count = static_cast<uint32_t>(exts.size());
    return true;
}

// ② surface：库创建好 VkInstance 之后调用这里，拿到窗口表面。
bool create_surface(void* state, VkInstance instance, VkSurfaceKHR& surface, std::string& error)
{
    auto* window = static_cast<SDL_Window*>(state);
    if (!SDL_Vulkan_CreateSurface(window, instance, &surface))
        { error = SDL_GetError(); return false; }
    return true;
}

// ③ 可绘制尺寸：窗口最小化时应返回 0x0（库会返回 skipped，不提交半帧）。
VkExtent2D drawable_extent(void* state)
{
    auto* window = static_cast<SDL_Window*>(state);
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    return VkExtent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}
```

然后一行创建设备 —— 返回的 `render_device` 是 RAII 对象（析构自动销毁，也可显式
`shutdown()`）；`frames_in_flight` 决定帧在飞数量（内部为此分配多个 cmd buffer /
semaphore / fence）。

```cpp
render_graph::vulkan::device_config config{
    .application_name = "MyApp",
    .frames_in_flight = 3,
    .validation = true,                     // 打开则加载 KHRONOS_validation 并计入 validation_error_count
    .surface = {
        .state = &my_window,
        .instance_extensions = &provide_extensions,
        .create_surface = &create_surface,
        .drawable_extent = &drawable_extent,
    },
    // .diagnostics = my_sink,   // 可选：宿主注入的 diagnostic_sink（转发 error/warning）
};
auto created = render_graph::vulkan::create_device(config);
if (!created) { /* created.error 按序列出初始化失败原因 */ return; }
render_graph::render_device device = std::move(created.device);
```

要点：

- `render_device`（`render_device.h`）是 `void* state + render_device_api` 函数表包装，
  所有调用转发到后端；空设备调用返回错误而不是崩。
- 全库只有 `surface_provider` 涉及 `Vk` 类型；其余公共 API 全部与 Vulkan 无关。

## 2. 一次性资源设置：apply_resource_changes

资源变更以"批"为单位，一次性原子提交：要么全部生效，要么（validate/prepare 失败）
全部回滚且不发布任何句柄。批量构造说明：

> **为什么分两批？** 上传（upload）和 bindless 发布（publish）的行必须引用
> buffer/image/sampler 句柄，而句柄只有第一批 `apply_resource_changes` 返回后才存在 ——
> 所以固定流程是：第一批（创建批）只提交创建行，apply 返回句柄；
> 第二批（上传 + 发布批）用这些句柄做上传与 bindless 发布。

### 第一批：创建资源（apply 返回句柄）

```cpp
using namespace render_graph;   // 真实代码建议限定到具体类型，避免通配 using

// --- 2.1 创建两个 device-local buffer：顶点 + 索引（auto 分配子内存，非独立分配）---
std::array<render_graph::buffer_create_row, 2> buffer_creates{{
    buffer_create_row{.desc = {
        .size = sizeof(Vertex) * vertex_count,
        .usage = buffer_usage::TRANSFER_DST | buffer_usage::VERTEX_BUFFER,
        .memory = memory_domain::device_local,
        .lifetime = resource_lifetime_class::persistent,   // 宿主长期持有
    }},
    buffer_create_row{.desc = {
        .size = sizeof(uint32_t) * index_count,
        .usage = buffer_usage::TRANSFER_DST | buffer_usage::INDEX_BUFFER,
        .memory = memory_domain::device_local,
        .lifetime = resource_lifetime_class::persistent,
    }},
}};

// --- 2.2 创建一个 2D 纹理 image（device_local，自动分配）---
std::array<render_graph::image_create_row, 1> image_creates{{
    image_create_row{.desc = {
        .fmt = format::R8G8B8A8_SRGB,
        .extent = {tex_width, tex_height, 1},
        .usage = image_usage::TRANSFER_DST | image_usage::SAMPLED,
        .mip_levels = 1,
        .lifetime = resource_lifetime_class::persistent,
    }},
}};

// --- 2.3 创建 sampler ---
std::array<render_graph::sampler_create_row, 1> sampler_creates{{
    sampler_create_row{.desc = {
        .min_filter = sampler_filter::linear,
        .mag_filter = sampler_filter::linear,
        .address_u = sampler_address_mode::repeat,
        .address_v = sampler_address_mode::repeat,
    }},
}};

// --- 2.4 图形管线（SPIR-V 二进制约束，全库唯一状态对象）---
std::array<render_graph::graphics_pipeline_create_row, 1> pipeline_creates{{
    graphics_pipeline_create_row{.desc = {
        .shaders = {
            {.stage = shader_stage::vertex, .binary_format = shader_binary_format::spirv,
             .binary = vertex_spv /* std::vector<uint32_t>，glslc -fspv-target-env=vulkan1.3 */},
            {.stage = shader_stage::fragment, .binary_format = shader_binary_format::spirv,
             .binary = fragment_spv},
        },
        .vertex_bindings = {{.binding = 0, .stride = sizeof(Vertex)}},
        .vertex_attributes = {
            {.location = 0, .binding = 0, .format = vertex_format::float3, .offset = offsetof(Vertex, pos)},
            {.location = 1, .binding = 0, .format = vertex_format::float2, .offset = offsetof(Vertex, uv)},
        },
        .topology = primitive_topology::triangle_list,
        .cull = cull_mode::back,
        .depth_test = true, .depth_write = true,
        .color_formats = {format::B8G8R8A8_UNORM},      // 必须与 swapchain 颜色格式一致
        .depth_format = format::D32_SFLOAT,
        .push_constants = {{.stage_mask = shader_stage_vertex_bit | shader_stage_fragment_bit,
                            .offset = 0, .size = sizeof(App::frame_ubo)}},
    }},
}};

// 第一批提交：只包含创建行；句柄由 result 返回（此时上传/发布行还引用不到句柄）
render_graph::resource_change_batch batch{
    .buffer_creates = buffer_creates, .image_creates = image_creates,
    .sampler_creates = sampler_creates, .graphics_pipeline_creates = pipeline_creates,
};
auto result = device.apply_resource_changes(batch);
if (!result) return;    // result.error / result.diagnostic{phase, row_kind, row_index, message}
my_vertex_buffer = result.buffers[0];   // device_buffer_handle{index, generation}
my_index_buffer  = result.buffers[1];
my_tex           = result.images[0];
my_sampler       = result.samplers[0];
my_pipeline      = result.graphics_pipelines[0];
```

### 第二批：上传数据 + bindless 发布（引用第一批句柄）

```cpp
// --- 2.5 一次性上传（staging）：设备内存 buffer/纹理通过 upload arena 异步拷贝 ---
// destination 必须填第一批返回的句柄 —— 句柄没到手前上传行构造不出来，只能放第二批。
std::array<render_graph::buffer_upload_row, 2> buffer_uploads{{
    buffer_upload_row{.destination = my_vertex_buffer, .offset = 0,
                      .bytes = std::as_bytes(std::span(vertices))},
    buffer_upload_row{.destination = my_index_buffer, .offset = 0,
                      .bytes = std::as_bytes(std::span(indices))},
}};
std::array<render_graph::image_upload_row, 1> image_uploads{{
    image_upload_row{.destination = my_tex, .width = tex_width, .height = tex_height,
                     .mip_level = 0, .bytes = std::as_bytes(std::span(pixels))},
}};

// --- 2.6 绑定到一个 bindless 槽（publish）：宿主只拿 index，shader 直接用 ---
// bindless_publish_row{table, buffer, image, sampler, offset, size}
std::array<render_graph::bindless_publish_row, 2> publishes{{
    bindless_publish_row{.table = bindless_table_kind::sampled_images,
                         .image = my_tex, .sampler = my_sampler},
    bindless_publish_row{.table = bindless_table_kind::uniform_buffers,
                         .buffer = my_ubo_buffer /* UBO buffer 按 2.1 同样方式创建 */,
                         .offset = 0, .size = sizeof(ubo)},
}};

// 第二批提交：上传 + bindless publish 同批原子提交
render_graph::resource_change_batch upload_batch{
    .buffer_uploads = buffer_uploads, .image_uploads = image_uploads,
    .bindless_publishes = publishes,
};
auto up = device.apply_resource_changes(upload_batch);
if (!up) return;
// up.bindless_slots: uint32_t 槽号，用作 shader 里的非均匀索引下标
tex_bindless_index = up.bindless_slots[0];
```

要点：

- 句柄是 `device_handle<Tag>{index, generation}`；`generation` 每 retire+复用递增，
  stale 句柄在 validate / 编译阶段被拒（读出 `{index, generation}` 的是活跃行的值）。
- 异步上传：先 `stage_buffer_upload` / `stage_image_upload` 到 upload arena 的 slice，
  等每帧自动注入的 `UploadPass`（`passes[0]`）执行 `vkCmdCopyBuffer[ToImage]`（图像带
  `UNDEFINED→TRANSFER_DST→SHADER_READ_ONLY` barrier2），submit 后 staging slice 才按
  submission 序号回收。
- 不再需要的资源用 `resource_retire_row` 放回批里；销毁被推迟到帧在飞全部完成。

## 3. 每帧渲染：recipe → frame_plan → render

`render` 接受一个 `frame_recipe{state, build}`。`build` 在 acquire 之后、拿到真实
`frame_environment`（extent / color_format / frame_index / submission /
completed_submission）时被回调，用它拼眼前的 `frame_plan`（纯扁平行 + CSR 范围）。

```cpp
// --- 每帧回调返回的 plan：本帧用到的资源 + pass + 访问 + 绘制 ---
// 表格全在 stack 上构造（或 std::array），frame_plan 只持有 span；
// 数据需在 device.render() 返回前保持有效。
render_graph::frame_build_result build_frame(void* state, const render_graph::frame_environment& env,
                                             render_graph::frame_plan& plan)
{
    auto* app = static_cast<App*>(state);

    // [0] 帧资源表：persistent 资源给句柄；transient 直接给 desc；swapchain 免描述。
    const std::array resources{
        render_graph::frame_resource_row{
            .source = frame_resource_source::persistent_buffer, .name = "mesh",
            .buffer = app->mesh_buffer},
        render_graph::frame_resource_row{
            .source = frame_resource_source::swapchain_image, .name = "swapchain"},
        render_graph::frame_resource_row{
            .source = frame_resource_source::transient_image, .name = "depth",
            .image_description = {.fmt = format::D32_SFLOAT, .extent = env.extent,
                .usage = image_usage::DEPTH_STENCIL_ATTACHMENT}},
    };

    // [1] 访问行 + 附件行：index 指向 [0] 的资源行。
    const std::array buffer_accesses{
        render_graph::frame_buffer_access_row{
            .resource = {0},                            // frame_resource_handle{index}
            .usage = buffer_usage::VERTEX_BUFFER | buffer_usage::INDEX_BUFFER,
            .access = access_type::read},
    };
    const std::array attachments{
        render_graph::frame_attachment_row{
            .resource = {1}, .kind = frame_attachment_kind::color,
            .load = attachment_load_op::clear, .store = attachment_store_op::store,
            .clear = {.color = {0.12f, 0.12f, 0.16f, 1.0f}}},
        render_graph::frame_attachment_row{
            .resource = {2}, .kind = frame_attachment_kind::depth_stencil,
            .load = attachment_load_op::clear, .store = attachment_store_op::dont_care,
            .clear = {.depth = 1.0f}},
    };

    // [2] 间接绘制行（CPU 只需给 pipeline/vertex/index/indirect 四件套 + count）。
    const std::array draws{
        render_graph::draw_indexed_indirect_row{
            .pipeline = app->pipeline,
            .vertex_buffer = app->mesh_buffer,   .vertex_offset = 0,
            .index_buffer  = app->mesh_buffer,   .index_offset  = vertex_bytes,
            .indirect_buffer = app->indirect_buffer, .indirect_offset = per_frame_indirect_byte,
            .draw_count = /*由 indirect buffer 决定，0 表示未初始化*/ app->indirect_draw_count,
            .stride = sizeof(render_graph::indexed_indirect_command),
            .indices = index_format::uint32,
        },
    };

    // [3] push constants（pass 级，span<byte>）：任意二进制布局，通常 per-frame ring。
    //     App::frame_ubo 是宿主自己定义的 uniform 结构（MVP 等）。
    const auto push = std::as_bytes(std::span(&app->frame_ubo, 1));

    // [4] pass 行：所有行段用 {begin, count} 切进扁平表。
    const std::array passes{
        render_graph::frame_pass_row{
            .name = "DrawPass", .kind = pass_kind::raster, .queue = queue_class::graphics,
            .buffer_accesses = {0, 1},     // 访问段
            .attachments     = {0, 2},     // 附件段
            .indexed_indirect_draws = {0, 1},
            .push_constant_offset = 0, .push_constant_size = sizeof(App::frame_ubo),
            .push_constant_stage_mask = shader_stage_vertex_bit | shader_stage_fragment_bit,
        },
    };

    plan = render_graph::frame_plan{
        .cache_key = app->recipe_revision,               // 宿主自增版本号，参与编译缓存 key
        .resources = resources, .passes = passes,
        .buffer_accesses = buffer_accesses, .image_accesses = {},
        .attachments = attachments, .push_constants = push,
        .indexed_indirect_draws = draws, .buffer_copies = {}, .dispatches = {},
    };
    return {};
}

// --- 主循环 ---
render_graph::frame_recipe recipe{.state = this, .build = &build_frame};
for (bool running = true; running;)
{
    // ... poll window events ...
    if (window_resized) device.request_resize();   // 库在下一帧开头自动 resize

    const auto frame = device.render(recipe);
    switch (frame.status)
    {
    case render_graph::frame_status::rendered: break;
    case render_graph::frame_status::skipped:  break; // 最小化 / OUT_OF_DATE，本帧未提交
    case render_graph::frame_status::failed:
        // frame.error（编译失败还会附带 diagnostics）
        break;
    }
}
// 析构时 device 自动 destroy；也可 device.shutdown() 显式收尾。
```

`frame_plan` 里的访问/附件/绘制在库内部完成其余一切：

- 校验 → lowering 成原生行 → 计算编译 cache key → cache 命中则整帧跳过编译与
  VMA 分配；未命中才跑 §4 的八个 phase；
- 每个 pass 前按 `synchronization_plan` 的 prologue 发 barrier2（layout/存取过渡、
  alias handoff、跨队列 ownership 自动推论）；
- raster pass 走 dynamic rendering（`vkCmdBeginRendering`，无 render pass 对象），
  附件 image view 与 barrier 内联转换；
- 结束将 swapchain image transition 到 present layout，再 submit/present。

## 4. 归属感与排查

- `device.statistics()`：`render_graph::render_statistics{presented_frames, graph_compiles,
  descriptor_updates, pipeline_creations, ...}` —— 稳定帧 `graph_compiles` 不增长、
  `descriptor_updates == 0` 是健康的标志（bindless 槽复用 + 编译缓存生效）。
- `device.validation_error_count()`：validation layer error 数（需 `.validation = true`）。
- 编译失败（第 3 节 `frame_status::failed`）时，诊断可精确到
  `compile_diagnostic{code, pass, kind, resource, pass_name, message}`（24 种错误码），
  用于把用户侧 recipe 错误反馈回 UI/日志。
- 别忘了 prep：**persistent 资源句柄变化（retire 后再创建）后，每帧 `frame_plan.cache_key`
  必须变化**（宿主维护 revision 或句柄代际），否则库会按旧的 cache key 复用旧 plan。
