#pragma once

#include "backend.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <iostream>

namespace render_graph
{
    // Physical Resource Table for Vulkan (SoA)
    // This is NOT visible to the generic Render Graph, only to the Vulkan Backend.
    struct vulkan_image_table
    {
        std::vector<VkImage>        images;
        std::vector<VkImageView>    image_views;
        std::vector<VkDeviceMemory> memories;
        std::vector<VkFormat>       vk_formats; // Converted from generic Format

        void resize(size_t size)
        {
            images.resize(size, VK_NULL_HANDLE);
            image_views.resize(size, VK_NULL_HANDLE);
            memories.resize(size, VK_NULL_HANDLE);
            vk_formats.resize(size, VK_FORMAT_UNDEFINED);
        }

        void clear()
        {
            images.clear();
            image_views.clear();
            memories.clear();
            vk_formats.clear();
        }
    };

    class vulkan_backend : public backend
    {
    public:
        VkDevice device; // Assumed to be initialized

        void apply_barriers(pass_handle /*pass*/, const per_pass_barrier& /*plan*/) override
        {
            // TODO: Lower barrier_op into VkImageMemoryBarrier2/VkBufferMemoryBarrier2 etc.
            // Intentionally kept empty for now.
        }

        // Helper to convert generic format to Vulkan format
        static VkFormat to_vk_format(format format)
        {
            switch (format)
            {
            case format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
            case format::R8G8B8A8_SRGB:  return VK_FORMAT_R8G8B8A8_SRGB;
            case format::D32_SFLOAT:     return VK_FORMAT_D32_SFLOAT;
            // ...
            default: return VK_FORMAT_UNDEFINED;
            }
        }

        static VkImageUsageFlags to_vk_usage(image_usage usage)
        {
            VkImageUsageFlags flags = 0;
            if ((uint32_t)usage & (uint32_t)image_usage::TRANSFER_SRC) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if ((uint32_t)usage & (uint32_t)image_usage::SAMPLED)      flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
            // ...
            return flags;
        }

        // Physical resource creation/lifetime is user-owned.
        // This backend will later only lower abstract barrier ops to Vulkan barriers.
    };
}
