#pragma once

#include "barrier.h"
#include "resource.h"
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <string>
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
        using command_context       = VkCommandBuffer;

        using error_callback_t = std::function<void(const char*)>;

        void set_error_callback(error_callback_t cb) { error_callback = std::move(cb); }

        [[nodiscard]] const std::string& get_last_error() const { return last_error; }

        void set_context(VkPhysicalDevice physical_device_in, VkDevice device_in)
        {
            physical_device = physical_device_in;
            device = device_in;
        }

        static const char* vk_result_to_string(VkResult r) noexcept
        {
            // Switch on int to avoid -Wswitch-enum / exhaustive-enum warnings across Vulkan header versions.
            switch (static_cast<int>(r))
            {
            case static_cast<int>(VK_SUCCESS): return "VK_SUCCESS";
            case static_cast<int>(VK_NOT_READY): return "VK_NOT_READY";
            case static_cast<int>(VK_TIMEOUT): return "VK_TIMEOUT";
            case static_cast<int>(VK_EVENT_SET): return "VK_EVENT_SET";
            case static_cast<int>(VK_EVENT_RESET): return "VK_EVENT_RESET";
            case static_cast<int>(VK_INCOMPLETE): return "VK_INCOMPLETE";
            case static_cast<int>(VK_ERROR_OUT_OF_HOST_MEMORY): return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case static_cast<int>(VK_ERROR_OUT_OF_DEVICE_MEMORY): return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case static_cast<int>(VK_ERROR_INITIALIZATION_FAILED): return "VK_ERROR_INITIALIZATION_FAILED";
            case static_cast<int>(VK_ERROR_DEVICE_LOST): return "VK_ERROR_DEVICE_LOST";
            case static_cast<int>(VK_ERROR_MEMORY_MAP_FAILED): return "VK_ERROR_MEMORY_MAP_FAILED";
            case static_cast<int>(VK_ERROR_LAYER_NOT_PRESENT): return "VK_ERROR_LAYER_NOT_PRESENT";
            case static_cast<int>(VK_ERROR_EXTENSION_NOT_PRESENT): return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case static_cast<int>(VK_ERROR_FEATURE_NOT_PRESENT): return "VK_ERROR_FEATURE_NOT_PRESENT";
            case static_cast<int>(VK_ERROR_INCOMPATIBLE_DRIVER): return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case static_cast<int>(VK_ERROR_TOO_MANY_OBJECTS): return "VK_ERROR_TOO_MANY_OBJECTS";
            case static_cast<int>(VK_ERROR_FORMAT_NOT_SUPPORTED): return "VK_ERROR_FORMAT_NOT_SUPPORTED";
            case static_cast<int>(VK_ERROR_FRAGMENTED_POOL): return "VK_ERROR_FRAGMENTED_POOL";
            case static_cast<int>(VK_ERROR_OUT_OF_POOL_MEMORY): return "VK_ERROR_OUT_OF_POOL_MEMORY";
            case static_cast<int>(VK_ERROR_INVALID_EXTERNAL_HANDLE): return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
            case static_cast<int>(VK_ERROR_SURFACE_LOST_KHR): return "VK_ERROR_SURFACE_LOST_KHR";
            case static_cast<int>(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR): return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
            case static_cast<int>(VK_ERROR_OUT_OF_DATE_KHR): return "VK_ERROR_OUT_OF_DATE_KHR";
            case static_cast<int>(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR): return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
            case static_cast<int>(VK_ERROR_VALIDATION_FAILED_EXT): return "VK_ERROR_VALIDATION_FAILED_EXT";
            case static_cast<int>(VK_ERROR_INVALID_SHADER_NV): return "VK_ERROR_INVALID_SHADER_NV";
            default: return "VK_RESULT_UNKNOWN";
            }
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

        bool emit_barriers(command_context& /*commands*/, std::span<const synchronization_op> /*barriers*/)
        {
            return true;
        }

        static uint32_t image_mip_levels(const image_desc& desc) noexcept { return desc.mipLevels; }
        static uint32_t image_array_layers(const image_desc& desc) noexcept { return desc.arrayLayers; }
        static uint64_t buffer_size(const buffer_desc& desc) noexcept { return desc.size; }

        static allocation_requirements get_image_allocation_requirements(const image_desc&) noexcept
        {
            return allocation_requirements{.supports_aliasing = false};
        }

        static allocation_requirements get_buffer_allocation_requirements(const buffer_desc&) noexcept
        {
            return allocation_requirements{.supports_aliasing = false};
        }

        void bind_imported_image(image_handle logical_image, native_image_handle native_image)
        {
            if (native_image == VK_NULL_HANDLE)
            {
                report_error("bind_imported_image: native_image is VK_NULL_HANDLE (logical=" +
                             std::to_string(static_cast<unsigned>(logical_image)) + ")");
            }
            pending_imported_images[logical_image] = native_image;
        }

        void bind_imported_buffer(buffer_handle logical_buffer, native_buffer_handle native_buffer)
        {
            if (native_buffer == VK_NULL_HANDLE)
            {
                report_error("bind_imported_buffer: native_buffer is VK_NULL_HANDLE (logical=" +
                             std::to_string(static_cast<unsigned>(logical_buffer)) + ")");
            }
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
                report_error("on_compile_resource_allocation: missing Vulkan context (physical_device/device is null)");
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

                // imported
                if (meta.image_metas.is_imported[rep])
                {
                    auto it = pending_imported_images.find(rep);
                    if (it != pending_imported_images.end() && it->second != VK_NULL_HANDLE)
                    {
                        images[physical_id] = it->second;
                    }
                    else
                    {
                        report_error("imported image is not bound (logical=" +
                                     std::to_string(static_cast<unsigned>(rep)) +
                                     ", physical=" +
                                     std::to_string(static_cast<unsigned>(physical_id)) +
                                     ")");
                    }
                    continue;
                }

                // transient
                VkImageCreateInfo ci = meta.image_metas.descs[rep];
                if (ci.sType == 0)
                {
                    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                }

                VkImage image = VK_NULL_HANDLE;
                const VkResult create_res = vkCreateImage(device, &ci, nullptr, &image);
                if (create_res != VK_SUCCESS)
                {
                    report_error("vkCreateImage failed (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", VkResult=" + std::to_string(static_cast<int>(create_res)) +
                                 " " + vk_result_to_string(create_res) + ")");
                    continue;
                }

                VkMemoryRequirements req{};
                vkGetImageMemoryRequirements(device, image, &req);
                const auto mem_type = find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mem_type == std::numeric_limits<uint32_t>::max())
                {
                    report_error("find_memory_type failed for VkImage (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) + ")");
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
                const VkResult alloc_res = vkAllocateMemory(device, &ai, nullptr, &memory);
                if (alloc_res != VK_SUCCESS)
                {
                    report_error("vkAllocateMemory(image) failed (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", VkResult=" + std::to_string(static_cast<int>(alloc_res)) +
                                 " " + vk_result_to_string(alloc_res) + ")");
                    vkDestroyImage(device, image, nullptr);
                    continue;
                }
                const VkResult bind_res = vkBindImageMemory(device, image, memory, 0);
                if (bind_res != VK_SUCCESS)
                {
                    report_error("vkBindImageMemory failed (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", VkResult=" + std::to_string(static_cast<int>(bind_res)) +
                                 " " + vk_result_to_string(bind_res) + ")");
                    vkFreeMemory(device, memory, nullptr);
                    vkDestroyImage(device, image, nullptr);
                    continue;
                }

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
                    if (it != pending_imported_buffers.end() && it->second != VK_NULL_HANDLE)
                    {
                        buffers[physical_id] = it->second;
                    }
                    else
                    {
                        report_error("imported buffer is not bound (logical=" +
                                     std::to_string(static_cast<unsigned>(rep)) +
                                     ", physical=" +
                                     std::to_string(static_cast<unsigned>(physical_id)) +
                                     ")");
                    }
                    continue;
                }

                VkBufferCreateInfo ci = meta.buffer_metas.descs[rep];
                if (ci.sType == 0)
                {
                    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                }

                VkBuffer buffer = VK_NULL_HANDLE;
                const VkResult create_res = vkCreateBuffer(device, &ci, nullptr, &buffer);
                if (create_res != VK_SUCCESS)
                {
                    report_error("vkCreateBuffer failed (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", VkResult=" + std::to_string(static_cast<int>(create_res)) +
                                 " " + vk_result_to_string(create_res) + ")");
                    continue;
                }

                VkMemoryRequirements req{};
                vkGetBufferMemoryRequirements(device, buffer, &req);
                const auto mem_type = find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mem_type == std::numeric_limits<uint32_t>::max())
                {
                    report_error("find_memory_type failed for VkBuffer (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) + ")");
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
                const VkResult alloc_res = vkAllocateMemory(device, &ai, nullptr, &memory);
                if (alloc_res != VK_SUCCESS)
                {
                    report_error("vkAllocateMemory(buffer) failed (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", VkResult=" + std::to_string(static_cast<int>(alloc_res)) +
                                 " " + vk_result_to_string(alloc_res) + ")");
                    vkDestroyBuffer(device, buffer, nullptr);
                    continue;
                }
                const VkResult bind_res = vkBindBufferMemory(device, buffer, memory, 0);
                if (bind_res != VK_SUCCESS)
                {
                    report_error("vkBindBufferMemory failed (logical=" + std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" + std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", VkResult=" + std::to_string(static_cast<int>(bind_res)) +
                                 " " + vk_result_to_string(bind_res) + ")");
                    vkFreeMemory(device, memory, nullptr);
                    vkDestroyBuffer(device, buffer, nullptr);
                    continue;
                }

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

        [[nodiscard]] native_image_handle get_image(image_handle logical) const
        {
            const auto physical = get_physical_image_id(logical);
            if (physical == std::numeric_limits<uint32_t>::max() || physical >= images.size())
            {
                return VK_NULL_HANDLE;
            }
            return images[physical];
        }

        [[nodiscard]] native_buffer_handle get_buffer(buffer_handle logical) const
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

    private:
        void report_error(const char* msg)
        {
            if (msg == nullptr)
            {
                return;
            }

            last_error = msg;
            if (error_callback)
            {
                error_callback(msg);
                return;
            }

            std::cerr << "[render_graph][vk_backend] " << msg << '\n';
        }

        void report_error(const std::string& msg) { report_error(msg.c_str()); }

        static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_filter, VkMemoryPropertyFlags properties)
        {
            VkPhysicalDeviceMemoryProperties mem_props{};
            vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

            const uint32_t safe_count = (mem_props.memoryTypeCount < static_cast<uint32_t>(VK_MAX_MEMORY_TYPES))
                                            ? mem_props.memoryTypeCount
                                            : static_cast<uint32_t>(VK_MAX_MEMORY_TYPES);

            for (uint32_t i = 0; i < safe_count; i++)
            {
                const bool type_ok = (type_filter & (1U << i)) != 0;

#if defined(_MSC_VER)
                __analysis_assume(i < static_cast<uint32_t>(VK_MAX_MEMORY_TYPES));
#endif

#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
                const bool prop_ok = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;

#if defined(__clang__)
#    pragma clang diagnostic pop
#endif
                if (type_ok && prop_ok)
                {
                    return i;
                }
            }
            return std::numeric_limits<uint32_t>::max();
        }

        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;

        // Optional: user-provided error callback. If unset, defaults to stderr.
        error_callback_t error_callback;

        // Stores the last reported error message (best-effort).
        std::string last_error;

        // Mapping from logical handle -> physical id (filled at compile)
        std::vector<resource_handle> logical_to_physical_img_id;
        std::vector<resource_handle> logical_to_physical_buf_id;

        // Physical tables (one entry per physical id)
        std::vector<VkImage> images;
        std::vector<VkDeviceMemory> image_memories;
        std::vector<VkBuffer> buffers;
        std::vector<VkDeviceMemory> buffer_memories;

        // Pending imported bindings (logical -> native)
        std::unordered_map<resource_handle, VkImage> pending_imported_images;
        std::unordered_map<resource_handle, VkBuffer> pending_imported_buffers;
    };
}
