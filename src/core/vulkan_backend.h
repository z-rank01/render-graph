#pragma once

#include "backend.h"
#include <vulkan/vulkan.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace render_graph
{
    class vk_backend : public backend
    {
    public:
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;

        // Mapping from logical handle -> physical id (filled at compile)
        std::vector<uint32_t> logical_to_physical_img_id;
        std::vector<uint32_t> logical_to_physical_buf_id;

        // Physical tables (one entry per physical id)
        std::vector<VkImage> images;
        std::vector<VkDeviceMemory> image_memories;
        std::vector<VkBuffer> buffers;
        std::vector<VkDeviceMemory> buffer_memories;

        // Pending imported bindings (logical -> native)
        std::unordered_map<resource_handle, VkImage> pending_imported_images;
        std::unordered_map<resource_handle, VkBuffer> pending_imported_buffers;

        void set_context(VkPhysicalDevice physical_device_in, VkDevice device_in)
        {
            physical_device = physical_device_in;
            device = device_in;
        }

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
            const auto bits = static_cast<uint32_t>(usage);
            if (bits & static_cast<uint32_t>(image_usage::TRANSFER_SRC)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (bits & static_cast<uint32_t>(image_usage::TRANSFER_DST)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (bits & static_cast<uint32_t>(image_usage::SAMPLED)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
            if (bits & static_cast<uint32_t>(image_usage::STORAGE)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
            if (bits & static_cast<uint32_t>(image_usage::COLOR_ATTACHMENT)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (bits & static_cast<uint32_t>(image_usage::DEPTH_STENCIL_ATTACHMENT)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            return flags;
        }

        static VkBufferUsageFlags to_vk_usage(buffer_usage usage)
        {
            VkBufferUsageFlags flags = 0;
            const auto bits = static_cast<uint32_t>(usage);
            if (bits & static_cast<uint32_t>(buffer_usage::TRANSFER_SRC)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            if (bits & static_cast<uint32_t>(buffer_usage::TRANSFER_DST)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (bits & static_cast<uint32_t>(buffer_usage::UNIFORM_BUFFER)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            if (bits & static_cast<uint32_t>(buffer_usage::STORAGE_BUFFER)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if (bits & static_cast<uint32_t>(buffer_usage::INDEX_BUFFER)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            if (bits & static_cast<uint32_t>(buffer_usage::VERTEX_BUFFER)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            if (bits & static_cast<uint32_t>(buffer_usage::INDIRECT_BUFFER)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
            return flags;
        }

        static uint32_t find_memory_type(VkPhysicalDevice phys,
                                        uint32_t type_filter,
                                        VkMemoryPropertyFlags properties)
        {
            VkPhysicalDeviceMemoryProperties mem_props{};
            vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

            for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
            {
                const bool type_ok = (type_filter & (1u << i)) != 0;
                const bool prop_ok = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
                if (type_ok && prop_ok)
                {
                    return i;
                }
            }
            return std::numeric_limits<uint32_t>::max();
        }

        void bind_imported_image(resource_handle logical_image,
                                 native_handle native_image,
                                 native_handle /*native_view*/ = 0) override
        {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            pending_imported_images[logical_image] = reinterpret_cast<VkImage>(native_image);
        }

        void bind_imported_buffer(resource_handle logical_buffer,
                                  native_handle native_buffer) override
        {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            pending_imported_buffers[logical_buffer] = reinterpret_cast<VkBuffer>(native_buffer);
        }

        void on_compile_resource_allocation(const resource_meta_table& meta,
                                            const physical_resource_meta& physical_meta) override
        {
            logical_to_physical_img_id = physical_meta.handle_to_physical_img_id;
            logical_to_physical_buf_id = physical_meta.handle_to_physical_buf_id;

            images.assign(physical_meta.physical_image_meta.size(), VK_NULL_HANDLE);
            image_memories.assign(physical_meta.physical_image_meta.size(), VK_NULL_HANDLE);
            buffers.assign(physical_meta.physical_buffer_meta.size(), VK_NULL_HANDLE);
            buffer_memories.assign(physical_meta.physical_buffer_meta.size(), VK_NULL_HANDLE);

            if (!physical_device || !device)
            {
                return;
            }

            // Images
            for (size_t physical_id = 0; physical_id < physical_meta.physical_image_meta.size(); physical_id++)
            {
                const auto rep = physical_meta.physical_image_meta[physical_id];
                if (rep >= meta.image_metas.names.size())
                {
                    continue;
                }

                if (meta.image_metas.is_imported[rep])
                {
                    auto it = pending_imported_images.find(rep);
                    if (it != pending_imported_images.end())
                    {
                        images[physical_id] = it->second;
                    }
                    continue;
                }

                VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                ci.imageType = VK_IMAGE_TYPE_2D;
                const auto extent = meta.image_metas.extents[rep];
                ci.extent = VkExtent3D{extent.width, extent.height, extent.depth};
                ci.mipLevels = meta.image_metas.mip_levels[rep];
                ci.arrayLayers = meta.image_metas.array_layers[rep];
                ci.format = to_vk_format(meta.image_metas.formats[rep]);
                ci.tiling = VK_IMAGE_TILING_OPTIMAL;
                ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                ci.usage = to_vk_usage(meta.image_metas.usages[rep]);
                ci.samples = static_cast<VkSampleCountFlagBits>(meta.image_metas.sample_counts[rep]);
                ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                VkImage image = VK_NULL_HANDLE;
                if (vkCreateImage(device, &ci, nullptr, &image) != VK_SUCCESS)
                {
                    continue;
                }

                VkMemoryRequirements req{};
                vkGetImageMemoryRequirements(device, image, &req);
                const auto mem_type = find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mem_type == std::numeric_limits<uint32_t>::max())
                {
                    vkDestroyImage(device, image, nullptr);
                    continue;
                }

                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = mem_type;

                VkDeviceMemory memory = VK_NULL_HANDLE;
                if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS)
                {
                    vkDestroyImage(device, image, nullptr);
                    continue;
                }
                (void)vkBindImageMemory(device, image, memory, 0);

                images[physical_id] = image;
                image_memories[physical_id] = memory;
            }

            // Buffers
            for (size_t physical_id = 0; physical_id < physical_meta.physical_buffer_meta.size(); physical_id++)
            {
                const auto rep = physical_meta.physical_buffer_meta[physical_id];
                if (rep >= meta.buffer_metas.names.size())
                {
                    continue;
                }

                if (meta.buffer_metas.is_imported[rep])
                {
                    auto it = pending_imported_buffers.find(rep);
                    if (it != pending_imported_buffers.end())
                    {
                        buffers[physical_id] = it->second;
                    }
                    continue;
                }

                VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                ci.size = meta.buffer_metas.sizes[rep];
                ci.usage = to_vk_usage(meta.buffer_metas.usages[rep]);
                ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                VkBuffer buffer = VK_NULL_HANDLE;
                if (vkCreateBuffer(device, &ci, nullptr, &buffer) != VK_SUCCESS)
                {
                    continue;
                }

                VkMemoryRequirements req{};
                vkGetBufferMemoryRequirements(device, buffer, &req);
                const auto mem_type = find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mem_type == std::numeric_limits<uint32_t>::max())
                {
                    vkDestroyBuffer(device, buffer, nullptr);
                    continue;
                }

                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = mem_type;

                VkDeviceMemory memory = VK_NULL_HANDLE;
                if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS)
                {
                    vkDestroyBuffer(device, buffer, nullptr);
                    continue;
                }
                (void)vkBindBufferMemory(device, buffer, memory, 0);

                buffers[physical_id] = buffer;
                buffer_memories[physical_id] = memory;
            }
        }

        [[nodiscard]] uint32_t get_physical_image_id(resource_handle logical) const
        {
            if (logical >= logical_to_physical_img_id.size())
            {
                return std::numeric_limits<uint32_t>::max();
            }
            return logical_to_physical_img_id[logical];
        }

        [[nodiscard]] uint32_t get_physical_buffer_id(resource_handle logical) const
        {
            if (logical >= logical_to_physical_buf_id.size())
            {
                return std::numeric_limits<uint32_t>::max();
            }
            return logical_to_physical_buf_id[logical];
        }

        // Physical resource creation/lifetime is still user-owned at the engine level.
        // This backend provides a minimal implementation to create transient resources
        // from render-graph allocation results (useful for samples/prototyping).
    };
}
