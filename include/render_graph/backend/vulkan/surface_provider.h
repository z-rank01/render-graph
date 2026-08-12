// =============================================================================
// surface_provider: host-supplied callbacks that hand the render graph a
// platform window surface (extensions, VkSurfaceKHR creation, drawable extent).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace render_graph::vulkan
{
    struct surface_provider
    {
        // --- Host context & surface acquisition ---

        void* state = nullptr; // Opaque host context, passed as the first argument to every callback.

        // Both callbacks return false and fill the trailing std::string with an error message on failure.
        bool (*instance_extensions)(void*, const char* const*&, uint32_t&, std::string&) = nullptr;
        bool (*create_surface)(void*, VkInstance, VkSurfaceKHR&, std::string&) = nullptr;

        VkExtent2D (*drawable_extent)(void*) = nullptr;
    };
} // namespace render_graph::vulkan

namespace render_graph
{
    using vk_surface_provider = vulkan::surface_provider;
}
