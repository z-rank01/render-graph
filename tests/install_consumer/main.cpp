#include <render_graph/render_device.h>
#include <render_graph/resource_types.h>

#if RENDER_GRAPH_HAS_VULKAN
#include <render_graph/backend/vulkan/device.h>
#endif

int main()
{
    const render_graph::buffer_desc buffer{
        .size = 256,
        .usage = render_graph::buffer_usage::STORAGE_BUFFER,
    };
    render_graph::render_device empty_device;
    const auto statistics = empty_device.statistics();

#if RENDER_GRAPH_HAS_VULKAN
    render_graph::vulkan::device_config config;
    (void)config;
#endif

    return buffer.size == 256 && statistics.presented_frames == 0 ? 0 : 1;
}
