#pragma once

// Vulkan resource allocation facade for the render graph: an opaque allocation
// handle, a dispatch table of allocation callbacks, and a default VMA-backed
// implementation of that table.

#include <functional>

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include "render_graph/resource.h"

namespace render_graph
{
    // =============================================================================
    // Allocation dispatch types
    // =============================================================================

    // Opaque handle to a memory allocation (a VmaAllocation in the default
    // implementation); intentionally untyped so the graph core stays backend-agnostic.
    using vk_allocation_handle = void*;

    // Table of allocation callbacks consumed by the render graph. All entries
    // must be set for the table to be usable (see valid()).
    struct vk_allocator_dispatch
    {
        std::function<allocation_requirements(const VkImageCreateInfo&)> image_requirements;
        std::function<allocation_requirements(const VkBufferCreateInfo&)> buffer_requirements;
        std::function<bool(const allocation_requirements&, vk_allocation_handle&)> allocate;
        std::function<void(vk_allocation_handle)> free;
        std::function<bool(vk_allocation_handle, const VkImageCreateInfo&, VkImage&)> create_image;
        std::function<bool(vk_allocation_handle, const VkBufferCreateInfo&, VkBuffer&)> create_buffer;
        std::function<void(VkImage)> destroy_image;
        std::function<void(VkBuffer)> destroy_buffer;
        std::function<bool(VkImage, const VkImageViewCreateInfo&, VkImageView&)> create_view;
        std::function<void(VkImageView)> destroy_view;

        [[nodiscard]] bool valid() const noexcept
        {
            return image_requirements && buffer_requirements && allocate && free && create_image && create_buffer &&
                   destroy_image && destroy_buffer && create_view && destroy_view;
        }
    };

    // =============================================================================
    // VMA-backed dispatch factory
    // =============================================================================

    // Builds a vk_allocator_dispatch whose entries call into the given VMA
    // allocator (allocations are made aliasing-capable so the graph can
    // sub-allocate images/buffers from one block).
    [[nodiscard]] inline vk_allocator_dispatch make_vma_allocator_dispatch(VmaAllocator allocator, VkDevice device)
    {
        vk_allocator_dispatch dispatch;

        // --- Memory requirement queries ---

        dispatch.image_requirements = [device](const VkImageCreateInfo& create_info)
        {
            VkMemoryDedicatedRequirements dedicated{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
            };
            VkMemoryRequirements2 requirements{
                .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                .pNext = &dedicated,
            };
            const VkDeviceImageMemoryRequirements request{
                .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                .pCreateInfo = &create_info,
            };
            vkGetDeviceImageMemoryRequirements(device, &request, &requirements);
            return allocation_requirements{
                .size = requirements.memoryRequirements.size,
                .alignment = requirements.memoryRequirements.alignment,
                .memory_type_bits = requirements.memoryRequirements.memoryTypeBits,
                .requires_dedicated = dedicated.requiresDedicatedAllocation == VK_TRUE,
                .supports_aliasing = dedicated.requiresDedicatedAllocation == VK_FALSE,
            };
        };
        dispatch.buffer_requirements = [device](const VkBufferCreateInfo& create_info)
        {
            VkMemoryDedicatedRequirements dedicated{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
            };
            VkMemoryRequirements2 requirements{
                .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                .pNext = &dedicated,
            };
            const VkDeviceBufferMemoryRequirements request{
                .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
                .pCreateInfo = &create_info,
            };
            vkGetDeviceBufferMemoryRequirements(device, &request, &requirements);
            return allocation_requirements{
                .size = requirements.memoryRequirements.size,
                .alignment = requirements.memoryRequirements.alignment,
                .memory_type_bits = requirements.memoryRequirements.memoryTypeBits,
                .requires_dedicated = dedicated.requiresDedicatedAllocation == VK_TRUE,
                .supports_aliasing = dedicated.requiresDedicatedAllocation == VK_FALSE,
            };
        };

        // --- Raw allocation / free ---
        // Every allocation gets VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT so the
        // graph can alias several images/buffers on the same block.

        dispatch.allocate = [allocator](const allocation_requirements& requirements, vk_allocation_handle& output)
        {
            const VkMemoryRequirements memory_requirements{
                .size = requirements.size,
                .alignment = requirements.alignment,
                .memoryTypeBits = requirements.memory_type_bits,
            };
            const VmaAllocationCreateInfo create_info{
                .flags = VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT,
                // vmaAllocateMemory() receives only VkMemoryRequirements and
                // therefore cannot infer buffer/image usage. VMA rejects all
                // VMA_MEMORY_USAGE_AUTO* values on this API; prefer device
                // local memory explicitly instead.
                .usage = VMA_MEMORY_USAGE_UNKNOWN,
                .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            };
            VmaAllocation allocation = nullptr;
            if (vmaAllocateMemory(allocator, &memory_requirements, &create_info, &allocation, nullptr) != VK_SUCCESS)
            {
                return false;
            }
            output = allocation;
            return true;
        };
        dispatch.free = [allocator](vk_allocation_handle allocation)
        {
            vmaFreeMemory(allocator, static_cast<VmaAllocation>(allocation));
        };

        // --- Resource creation on an allocation (aliasing) / destruction ---

        dispatch.create_image = [allocator](vk_allocation_handle allocation, const VkImageCreateInfo& create_info, VkImage& image)
        {
            return vmaCreateAliasingImage2(allocator, static_cast<VmaAllocation>(allocation), 0, &create_info, &image) == VK_SUCCESS;
        };
        dispatch.create_buffer = [allocator](vk_allocation_handle allocation, const VkBufferCreateInfo& create_info, VkBuffer& buffer)
        {
            return vmaCreateAliasingBuffer2(allocator, static_cast<VmaAllocation>(allocation), 0, &create_info, &buffer) == VK_SUCCESS;
        };
        dispatch.destroy_image = [device](VkImage image) { vkDestroyImage(device, image, nullptr); };
        dispatch.destroy_buffer = [device](VkBuffer buffer) { vkDestroyBuffer(device, buffer, nullptr); };

        // --- Image view management ---

        dispatch.create_view = [device](VkImage, const VkImageViewCreateInfo& create_info, VkImageView& view)
        {
            return vkCreateImageView(device, &create_info, nullptr, &view) == VK_SUCCESS;
        };
        dispatch.destroy_view = [device](VkImageView view) { vkDestroyImageView(device, view, nullptr); };
        return dispatch;
    }

    // =============================================================================
    // Image view descriptor
    // =============================================================================

    // Backend-neutral description of the image view a graph resource wants;
    // compared by value, so it can serve as a lookup key.
    struct vk_image_view_desc
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
        VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        uint32_t base_mip_level = 0;
        uint32_t mip_level_count = VK_REMAINING_MIP_LEVELS;
        uint32_t base_array_layer = 0;
        uint32_t array_layer_count = VK_REMAINING_ARRAY_LAYERS;

        [[nodiscard]] constexpr auto operator<=>(const vk_image_view_desc&) const noexcept = default;
    };
}
