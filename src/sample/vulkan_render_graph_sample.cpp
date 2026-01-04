#include <iostream>

#include "render_graph/system.h"

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#endif

int main()
{
    render_graph::render_graph_system rg;

#if __has_include(<vulkan/vulkan.h>)
    std::cout << "vulkan_render_graph_sample: compiled graph (Vulkan headers available, VK_HEADER_VERSION=" << VK_HEADER_VERSION << ")\n";
#else
    std::cout << "vulkan_render_graph_sample: compiled graph (Vulkan headers not available)\n";
#endif

    return 0;
}
