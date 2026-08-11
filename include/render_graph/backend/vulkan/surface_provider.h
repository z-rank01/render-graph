#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace render_graph::vulkan
{
    struct surface_provider
    {
        void* state = nullptr;
        bool (*instance_extensions)(void*, const char* const*&, uint32_t&, std::string&) = nullptr;
        bool (*create_surface)(void*, VkInstance, VkSurfaceKHR&, std::string&) = nullptr;
        VkExtent2D (*drawable_extent)(void*) = nullptr;
    };
} // namespace render_graph::vulkan

namespace render_graph
{
    using vk_surface_provider = vulkan::surface_provider;
}
