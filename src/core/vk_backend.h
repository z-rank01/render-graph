#pragma once

#include "barrier.h"
#include "resource.h"
#include <vulkan/vulkan.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace render_graph
{
    class vk_backend
    {
    public:
        using image_desc          = VkImageCreateInfo;
        using buffer_desc         = VkBufferCreateInfo;
        using native_image_handle = VkImage;
        using native_buffer_handle = VkBuffer;

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

        void apply_barriers(pass_handle /*pass*/, const per_pass_barrier& /*plan*/)
        {
            // TODO: Lower barrier_op into VkImageMemoryBarrier2/VkBufferMemoryBarrier2 etc.
            // Intentionally kept empty for now.
        }

        static uint64_t hash_combine(uint64_t seed, uint64_t v) noexcept
        {
            // 64-bit mix (similar to boost::hash_combine)
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }

        static uint64_t hash_image_desc(const image_desc& d) noexcept
        {
            uint64_t hash = 0;
            hash = hash_combine(hash, static_cast<uint64_t>(d.flags));
            hash = hash_combine(hash, static_cast<uint64_t>(d.imageType));
            hash = hash_combine(hash, static_cast<uint64_t>(d.format));
            hash = hash_combine(hash, (static_cast<uint64_t>(d.extent.width) << 32) | d.extent.height);
            hash = hash_combine(hash, static_cast<uint64_t>(d.extent.depth));
            hash = hash_combine(hash, (static_cast<uint64_t>(d.mipLevels) << 32) | d.arrayLayers);
            hash = hash_combine(hash, static_cast<uint64_t>(d.samples));
            hash = hash_combine(hash, static_cast<uint64_t>(d.tiling));
            hash = hash_combine(hash, static_cast<uint64_t>(d.usage));
            hash = hash_combine(hash, static_cast<uint64_t>(d.sharingMode));
            return hash;
        }

        static uint64_t hash_buffer_desc(const buffer_desc& d) noexcept
        {
            uint64_t hash = 0;
            hash = hash_combine(hash, static_cast<uint64_t>(d.flags));
            hash = hash_combine(hash, static_cast<uint64_t>(d.size));
            hash = hash_combine(hash, static_cast<uint64_t>(d.usage));
            hash = hash_combine(hash, static_cast<uint64_t>(d.sharingMode));
            return hash;
        }

        static bool is_compatible_image(const image_desc& a, const image_desc& b) noexcept
        {
            return a.flags == b.flags && a.imageType == b.imageType && a.format == b.format && a.extent.width == b.extent.width &&
                   a.extent.height == b.extent.height && a.extent.depth == b.extent.depth && a.mipLevels == b.mipLevels &&
                   a.arrayLayers == b.arrayLayers && a.samples == b.samples && a.tiling == b.tiling && a.usage == b.usage &&
                   a.sharingMode == b.sharingMode;
        }

        static bool is_compatible_buffer(const buffer_desc& a, const buffer_desc& b) noexcept
        {
            return a.flags == b.flags && a.size == b.size && a.usage == b.usage && a.sharingMode == b.sharingMode;
        }

        static uint32_t find_memory_type(VkPhysicalDevice phys,
                                        uint32_t type_filter,
                                        VkMemoryPropertyFlags properties)
        {
            VkPhysicalDeviceMemoryProperties mem_props{};
            vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

            for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
            {
                const bool type_ok = (type_filter & (1U << i)) != 0;
                const bool prop_ok = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
                if (type_ok && prop_ok)
                {
                    return i;
                }
            }
            return std::numeric_limits<uint32_t>::max();
        }

        void bind_imported_image(resource_handle logical_image, native_image_handle native_image)
        {
            pending_imported_images[logical_image] = native_image;
        }

        void bind_imported_buffer(resource_handle logical_buffer, native_buffer_handle native_buffer)
        {
            pending_imported_buffers[logical_buffer] = native_buffer;
        }

        template <typename MetaTableT>
        void on_compile_resource_allocation(const MetaTableT& meta,
                                            const physical_resource_meta& physical_meta)
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

                VkImageCreateInfo ci = meta.image_metas.descs[rep];
                if (ci.sType == 0)
                {
                    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                }

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

                VkMemoryAllocateInfo ai{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .pNext = nullptr,
                    .allocationSize = req.size, 
                    .memoryTypeIndex = mem_type 
                };
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

                VkBufferCreateInfo ci = meta.buffer_metas.descs[rep];
                if (ci.sType == 0)
                {
                    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                }

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

                VkMemoryAllocateInfo ai{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .pNext = nullptr,
                    .allocationSize = req.size, 
                    .memoryTypeIndex = mem_type 
                };
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

        [[nodiscard]] native_image_handle get_image(resource_handle logical) const
        {
            const auto physical = get_physical_image_id(logical);
            if (physical == std::numeric_limits<uint32_t>::max() || physical >= images.size())
            {
                return VK_NULL_HANDLE;
            }
            return images[physical];
        }

        [[nodiscard]] native_buffer_handle get_buffer(resource_handle logical) const
        {
            const auto physical = get_physical_buffer_id(logical);
            if (physical == std::numeric_limits<uint32_t>::max() || physical >= buffers.size())
            {
                return VK_NULL_HANDLE;
            }
            return buffers[physical];
        }

        // Physical resource creation/lifetime is still user-owned at the engine level.
        // This backend provides a minimal implementation to create transient resources
        // from render-graph allocation results (useful for samples/prototyping).
    };
}
