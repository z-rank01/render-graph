#pragma once

#include "render_graph/barrier.h"
#include "render_graph/resource.h"
#include "render_graph/raster.h"
#include "render_graph/submission.h"
#include "vk_barrier_lowering.h"
#include "vk_resource_allocator.h"
#include "vk_resource_lowering.h"
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace render_graph
{
    class vk_graph_executor
    {
    public:
        using image_desc          = render_graph::image_desc;
        using buffer_desc         = render_graph::buffer_desc;
        using native_image_handle = VkImage;
        using native_buffer_handle = VkBuffer;
        using command_context       = VkCommandBuffer;

        using error_callback_t = std::function<void(const char*)>;

        ~vk_graph_executor()
        {
            shutdown();
        }

        void shutdown()
        {
            retire_current_resources();
            collect_retired(std::numeric_limits<uint64_t>::max());
        }

        void set_error_callback(error_callback_t cb) { error_callback = std::move(cb); }

        [[nodiscard]] const std::string& get_last_error() const { return last_error; }
        void clear_error() { last_error.clear(); }
        void set_error(std::string message) { report_error(message); }

        void set_context(VkPhysicalDevice physical_device_in, VkDevice device_in)
        {
            physical_device = physical_device_in;
            device = device_in;
        }

        void set_context(VkPhysicalDevice physical_device_in,
                         VkDevice device_in,
                         VmaAllocator allocator,
                         vk_queue_family_indices queue_families_in = {},
                         uint32_t frames_in_flight_in = 2)
        {
            physical_device = physical_device_in;
            device = device_in;
            queue_families = queue_families_in;
            frames_in_flight = std::max(uint32_t{1}, frames_in_flight_in);
            allocator_dispatch = make_vma_allocator_dispatch(allocator, device);
        }

        void set_context(VkPhysicalDevice physical_device_in,
                         VkDevice device_in,
                         vk_allocator_dispatch dispatch,
                         vk_queue_family_indices queue_families_in = {},
                         uint32_t frames_in_flight_in = 2)
        {
            physical_device = physical_device_in;
            device = device_in;
            queue_families = queue_families_in;
            frames_in_flight = std::max(uint32_t{1}, frames_in_flight_in);
            allocator_dispatch = std::move(dispatch);
        }

        [[nodiscard]] queue_availability available_queue_classes() const noexcept
        {
            return queue_availability{
                .compute = queue_families.compute != VK_QUEUE_FAMILY_IGNORED && queue_families.compute != queue_families.graphics,
                .copy = queue_families.copy != VK_QUEUE_FAMILY_IGNORED && queue_families.copy != queue_families.graphics,
            };
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

        static uint64_t hash_combine(uint64_t seed, uint64_t v) noexcept
        {
            // 64-bit mix (similar to boost::hash_combine)
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }

        static bool is_compatible_native_image(const VkImageCreateInfo& a, const VkImageCreateInfo& b) noexcept
        {
            return a.flags == b.flags && a.imageType == b.imageType && a.format == b.format &&
                   a.extent.width == b.extent.width && a.extent.height == b.extent.height &&
                   a.extent.depth == b.extent.depth && a.mipLevels == b.mipLevels &&
                   a.arrayLayers == b.arrayLayers && a.samples == b.samples && a.tiling == b.tiling &&
                   a.usage == b.usage && a.sharingMode == b.sharingMode;
        }

        static bool is_compatible_native_buffer(const VkBufferCreateInfo& a, const VkBufferCreateInfo& b) noexcept
        {
            return a.flags == b.flags && a.size == b.size && a.usage == b.usage &&
                   a.sharingMode == b.sharingMode;
        }

        [[nodiscard]] static backend_capabilities capabilities() noexcept { return {}; }

        [[nodiscard]] static resource_desc_diagnostic validate_image_desc(const image_desc& desc)
        {
            if (desc.fmt == format::UNDEFINED || lower_vk_format(desc.fmt) == VK_FORMAT_UNDEFINED)
                return {false, "Vulkan lowering does not support the requested image format"};
            if (desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0)
                return {false, "Vulkan image extent components must be non-zero"};
            if (desc.mapping == mapping_policy::persistent && desc.memory == memory_domain::device_local)
                return {false, "persistent mapping requires upload or readback memory"};
            if (desc.memory != memory_domain::device_local &&
                (desc.usage & (image_usage::COLOR_ATTACHMENT | image_usage::DEPTH_STENCIL_ATTACHMENT)) != image_usage::NONE)
                return {false, "Vulkan attachment images require device-local memory"};
            return {};
        }

        [[nodiscard]] static resource_desc_diagnostic validate_buffer_desc(const buffer_desc& desc)
        {
            if (desc.size == 0) return {false, "Vulkan buffer size must be non-zero"};
            if (desc.mapping == mapping_policy::persistent && desc.memory == memory_domain::device_local)
                return {false, "persistent mapping requires upload or readback memory"};
            return {};
        }

        bool emit_barriers(command_context& commands, std::span<const synchronization_op> barriers)
        {
            if (commands == VK_NULL_HANDLE)
            {
                report_error("emit_barriers: command buffer is VK_NULL_HANDLE");
                return false;
            }
            vk_barrier_batch batch;
            const bool built = build_vk_barrier_batch(
                barriers,
                queue_families,
                [&](image_handle logical) { return get_image(logical); },
                [&](buffer_handle logical) { return get_buffer_range(logical); },
                batch);
            if (!built)
            {
                report_error("emit_barriers: synchronization references an unbound native resource");
                return false;
            }
            if (!batch.empty())
            {
                const auto dependency = batch.dependency_info();
                vkCmdPipelineBarrier2(commands, &dependency);
            }
            return true;
        }

        bool begin_raster_pass(command_context& commands, const raster_pass_desc& raster)
        {
            if (commands == VK_NULL_HANDLE)
            {
                return false;
            }
            auto load_op = [](attachment_load_op op)
            {
                switch (op)
                {
                case attachment_load_op::load: return VK_ATTACHMENT_LOAD_OP_LOAD;
                case attachment_load_op::clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
                case attachment_load_op::dont_care: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                }
                return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            };
            auto store_op = [](attachment_store_op op)
            {
                return op == attachment_store_op::store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            };
            auto view_desc = [](const raster_attachment& attachment, VkImageAspectFlags aspects)
            {
                return vk_image_view_desc{
                    .aspects = aspects,
                    .base_mip_level = attachment.subresource.base_mip_level,
                    .mip_level_count = attachment.subresource.mip_level_count == remaining_subresources
                                           ? VK_REMAINING_MIP_LEVELS
                                           : attachment.subresource.mip_level_count,
                    .base_array_layer = attachment.subresource.base_array_layer,
                    .array_layer_count = attachment.subresource.array_layer_count == remaining_subresources
                                             ? VK_REMAINING_ARRAY_LAYERS
                                             : attachment.subresource.array_layer_count,
                };
            };

            std::vector<VkRenderingAttachmentInfo> colors;
            colors.reserve(raster.colors.size());
            for (const auto& attachment : raster.colors)
            {
                VkRenderingAttachmentInfo info{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = get_or_create_image_view(attachment.image, view_desc(attachment, VK_IMAGE_ASPECT_COLOR_BIT)),
                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .resolveMode = VK_RESOLVE_MODE_NONE,
                    .loadOp = load_op(attachment.load),
                    .storeOp = store_op(attachment.store),
                };
                std::copy(attachment.clear.color.begin(), attachment.clear.color.end(), info.clearValue.color.float32);
                if (attachment.resolve_image != invalid_image)
                {
                    auto resolve_desc = view_desc(attachment, VK_IMAGE_ASPECT_COLOR_BIT);
                    resolve_desc.base_mip_level = attachment.resolve_subresource.base_mip_level;
                    resolve_desc.mip_level_count = attachment.resolve_subresource.mip_level_count;
                    resolve_desc.base_array_layer = attachment.resolve_subresource.base_array_layer;
                    resolve_desc.array_layer_count = attachment.resolve_subresource.array_layer_count;
                    info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                    info.resolveImageView = get_or_create_image_view(attachment.resolve_image, resolve_desc);
                    info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
                if (info.imageView == VK_NULL_HANDLE ||
                    (attachment.resolve_image != invalid_image && info.resolveImageView == VK_NULL_HANDLE))
                {
                    return false;
                }
                colors.push_back(info);
            }

            VkRenderingAttachmentInfo depth_info{};
            const VkRenderingAttachmentInfo* depth = nullptr;
            const VkRenderingAttachmentInfo* stencil = nullptr;
            if (raster.has_depth_stencil)
            {
                const auto aspects = lower_vk_aspects(raster.depth_stencil.subresource.aspects,
                                                      static_cast<uint32_t>(image_usage::DEPTH_STENCIL_ATTACHMENT));
                depth_info = VkRenderingAttachmentInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = get_or_create_image_view(raster.depth_stencil.image,
                                                         view_desc(raster.depth_stencil, aspects)),
                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    .loadOp = load_op(raster.depth_stencil.load),
                    .storeOp = store_op(raster.depth_stencil.store),
                };
                depth_info.clearValue.depthStencil = {
                    raster.depth_stencil.clear.depth,
                    raster.depth_stencil.clear.stencil,
                };
                if (depth_info.imageView == VK_NULL_HANDLE)
                {
                    return false;
                }
                if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
                {
                    depth = &depth_info;
                }
                if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0)
                {
                    stencil = &depth_info;
                }
            }

            render_area area = raster.area;
            if ((area.width == 0 || area.height == 0) && (!raster.colors.empty() || raster.has_depth_stencil))
            {
                const auto logical = !raster.colors.empty() ? raster.colors.front().image : raster.depth_stencil.image;
                const auto& desc = logical_image_descs[logical];
                area.width = desc.extent.width;
                area.height = desc.extent.height;
            }
            const VkRenderingInfo rendering{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea = {{area.x, area.y}, {area.width, area.height}},
                .layerCount = raster.layer_count,
                .colorAttachmentCount = static_cast<uint32_t>(colors.size()),
                .pColorAttachments = colors.data(),
                .pDepthAttachment = depth,
                .pStencilAttachment = stencil,
            };
            vkCmdBeginRendering(commands, &rendering);
            return true;
        }

        bool end_raster_pass(command_context& commands)
        {
            if (commands == VK_NULL_HANDLE)
            {
                return false;
            }
            vkCmdEndRendering(commands);
            return true;
        }

        allocation_requirements get_image_allocation_requirements(const image_desc& desc) const noexcept
        {
            const auto native = lower_vk_image_desc(desc);
            return allocator_dispatch.image_requirements ? allocator_dispatch.image_requirements(native)
                                                         : allocation_requirements{.supports_aliasing = false};
        }

        allocation_requirements get_buffer_allocation_requirements(const buffer_desc& desc) const noexcept
        {
            const auto native = lower_vk_buffer_desc(desc);
            return allocator_dispatch.buffer_requirements ? allocator_dispatch.buffer_requirements(native)
                                                          : allocation_requirements{.supports_aliasing = false};
        }

        void bind_imported_image(image_handle logical_image, native_image_handle native_image)
        {
            if (native_image == VK_NULL_HANDLE)
            {
                report_error("bind_imported_image: native_image is VK_NULL_HANDLE (logical=" +
                             std::to_string(static_cast<unsigned>(logical_image)) + ")");
            }
            pending_imported_images[logical_image] = native_image;
            const auto physical = get_physical_image_id(logical_image);
            if (physical != invalid_resource && physical < images.size())
            {
                retire_views_for_image(images[physical]);
                images[physical] = native_image;
            }
        }

        void bind_imported_buffer(buffer_handle logical_buffer, native_buffer_handle native_buffer)
        {
            bind_imported_buffer(logical_buffer, vk_native_buffer_range{native_buffer});
        }

        void bind_imported_buffer(buffer_handle logical_buffer, vk_native_buffer_range native_buffer)
        {
            if (native_buffer.buffer == VK_NULL_HANDLE)
            {
                report_error("bind_imported_buffer: native_buffer is VK_NULL_HANDLE (logical=" +
                             std::to_string(static_cast<unsigned>(logical_buffer)) + ")");
            }
            pending_imported_buffers[logical_buffer] = native_buffer;
            const auto physical = get_physical_buffer_id(logical_buffer);
            if (physical != invalid_resource && physical < buffers.size())
            {
                buffers[physical] = native_buffer.buffer;
            }
        }

        void begin_frame(uint64_t frame_index, uint64_t completed_frame)
        {
            current_frame = frame_index;
            collect_retired(completed_frame);
        }

        void commit_frame() noexcept {}
        void abort_frame() noexcept {}

        [[nodiscard]] VkImageView get_or_create_image_view(image_handle logical, vk_image_view_desc desc)
        {
            const auto image = get_image(logical);
            if (image == VK_NULL_HANDLE || logical >= logical_image_descs.size() || !allocator_dispatch.create_view)
            {
                report_error("get_or_create_image_view: image is unbound or allocator context is missing");
                return VK_NULL_HANDLE;
            }
            const auto& image_desc = logical_image_descs[logical];
            if (desc.format == VK_FORMAT_UNDEFINED)
            {
                desc.format = lower_vk_format(image_desc.fmt);
            }
            else if (desc.format != lower_vk_format(image_desc.fmt) &&
                     (image_desc.flags & image_flags::MUTABLE_FORMAT) == image_flags::NONE)
            {
                report_error("get_or_create_image_view: format override requires VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT");
                return VK_NULL_HANDLE;
            }
            const auto existing = std::ranges::find_if(view_cache, [&](const image_view_entry& entry)
            {
                return entry.image == image && entry.desc == desc;
            });
            if (existing != view_cache.end())
            {
                return existing->view;
            }
            const VkImageViewCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = desc.view_type,
                .format = desc.format,
                .subresourceRange = {
                    .aspectMask = desc.aspects,
                    .baseMipLevel = desc.base_mip_level,
                    .levelCount = desc.mip_level_count,
                    .baseArrayLayer = desc.base_array_layer,
                    .layerCount = desc.array_layer_count,
                },
            };
            VkImageView view = VK_NULL_HANDLE;
            if (!allocator_dispatch.create_view(image, create_info, view))
            {
                report_error("get_or_create_image_view: vkCreateImageView failed");
                return VK_NULL_HANDLE;
            }
            view_cache.push_back(image_view_entry{.image = image, .desc = desc, .view = view});
            return view;
        }

        template <typename MetaTableT>
        void on_compile_resource_allocation(const MetaTableT& meta, const physical_resource_meta& physical_meta)
        {
            if (!allocator_dispatch.valid())
            {
                report_error("on_compile_resource_allocation: VMA allocator context is missing");
                return;
            }
            if (can_reuse_plan(meta, physical_meta))
            {
                logical_to_physical_img_id = physical_meta.handle_to_physical_img_id;
                logical_to_physical_buf_id = physical_meta.handle_to_physical_buf_id;
                logical_image_descs = meta.image_metas.descs;
                rebind_imported_resources(meta, physical_meta);
                return;
            }

            auto old_images = std::move(images);
            auto old_buffers = std::move(buffers);
            auto old_owned_images = std::move(owned_images);
            auto old_owned_buffers = std::move(owned_buffers);
            auto old_image_allocations = std::move(image_allocations);
            auto old_buffer_allocations = std::move(buffer_allocations);
            auto old_image_descs = std::move(physical_image_descs);
            auto old_buffer_descs = std::move(physical_buffer_descs);
            auto old_image_names = std::move(physical_image_names);
            auto old_buffer_names = std::move(physical_buffer_names);
            auto old_physical_image_blocks = std::move(physical_image_blocks);
            auto old_physical_buffer_blocks = std::move(physical_buffer_blocks);
            auto old_image_block_keys = std::move(image_block_keys);
            auto old_buffer_block_keys = std::move(buffer_block_keys);
            auto old_image_block_requirements = std::move(image_block_requirements);
            auto old_buffer_block_requirements = std::move(buffer_block_requirements);
            auto old_views = std::move(view_cache);

            logical_to_physical_img_id = physical_meta.handle_to_physical_img_id;
            logical_to_physical_buf_id = physical_meta.handle_to_physical_buf_id;
            logical_image_descs = meta.image_metas.descs;
            physical_image_descs.clear();
            physical_buffer_descs.clear();
            physical_image_names.clear();
            physical_buffer_names.clear();
            physical_image_blocks.clear();
            physical_buffer_blocks.clear();
            image_block_mapping = physical_meta.handle_to_image_memory_block;
            buffer_block_mapping = physical_meta.handle_to_buffer_memory_block;

            image_block_keys = make_block_keys(meta.image_metas, image_block_mapping, physical_meta.image_memory_blocks.size());
            buffer_block_keys = make_block_keys(meta.buffer_metas, buffer_block_mapping, physical_meta.buffer_memory_blocks.size());
            image_block_requirements = physical_meta.image_memory_blocks;
            buffer_block_requirements = physical_meta.buffer_memory_blocks;

            std::vector<resource_handle> matched_old_image_blocks(image_block_keys.size(), invalid_resource);
            std::vector<bool> used_old_image_blocks(old_image_block_keys.size(), false);
            image_allocations.assign(image_block_keys.size(), nullptr);
            for (resource_handle block = 0; block < image_block_keys.size(); block++)
            {
                for (resource_handle old = 0; old < old_image_block_keys.size(); old++)
                {
                    if (!used_old_image_blocks[old] && image_block_keys[block] == old_image_block_keys[old] &&
                        block < image_block_requirements.size() && old < old_image_block_requirements.size() &&
                        image_block_requirements[block] == old_image_block_requirements[old])
                    {
                        matched_old_image_blocks[block] = old;
                        used_old_image_blocks[old] = true;
                        image_allocations[block] = old_image_allocations[old];
                        old_image_allocations[old] = nullptr;
                        break;
                    }
                }
                if (image_allocations[block] == nullptr &&
                    !allocator_dispatch.allocate(physical_meta.image_memory_blocks[block], image_allocations[block]))
                {
                    report_error("VMA image memory block allocation failed");
                }
            }

            std::vector<resource_handle> matched_old_buffer_blocks(buffer_block_keys.size(), invalid_resource);
            std::vector<bool> used_old_buffer_blocks(old_buffer_block_keys.size(), false);
            buffer_allocations.assign(buffer_block_keys.size(), nullptr);
            for (resource_handle block = 0; block < buffer_block_keys.size(); block++)
            {
                for (resource_handle old = 0; old < old_buffer_block_keys.size(); old++)
                {
                    if (!used_old_buffer_blocks[old] && buffer_block_keys[block] == old_buffer_block_keys[old] &&
                        block < buffer_block_requirements.size() && old < old_buffer_block_requirements.size() &&
                        buffer_block_requirements[block] == old_buffer_block_requirements[old])
                    {
                        matched_old_buffer_blocks[block] = old;
                        used_old_buffer_blocks[old] = true;
                        buffer_allocations[block] = old_buffer_allocations[old];
                        old_buffer_allocations[old] = nullptr;
                        break;
                    }
                }
                if (buffer_allocations[block] == nullptr &&
                    !allocator_dispatch.allocate(physical_meta.buffer_memory_blocks[block], buffer_allocations[block]))
                {
                    report_error("VMA buffer memory block allocation failed");
                }
            }

            images.assign(physical_meta.physical_image_meta.size(), VK_NULL_HANDLE);
            owned_images.assign(images.size(), false);
            for (resource_handle physical = 0; physical < physical_meta.physical_image_meta.size(); physical++)
            {
                const auto logical = physical_meta.physical_image_meta[physical];
                auto create_info = lower_vk_image_desc(meta.image_metas.descs[logical]);
                physical_image_descs.push_back(create_info);
                physical_image_names.push_back(meta.image_metas.names[logical]);
                const auto block = physical_meta.handle_to_image_memory_block[logical];
                physical_image_blocks.push_back(block);
                if (meta.image_metas.is_imported[logical])
                {
                    const auto bound = pending_imported_images.find(logical);
                    if (bound != pending_imported_images.end())
                    {
                        images[physical] = bound->second;
                    }
                    continue;
                }
                if (block == invalid_resource || block >= image_allocations.size() || image_allocations[block] == nullptr)
                {
                    report_error("VMA image has no valid memory block");
                    continue;
                }
                const auto old_block = matched_old_image_blocks[block];
                if (old_block != invalid_resource)
                {
                    for (resource_handle old = 0; old < old_images.size(); old++)
                    {
                        if (old < old_owned_images.size() && old_owned_images[old] &&
                            old < old_physical_image_blocks.size() && old_physical_image_blocks[old] == old_block &&
                            old < old_image_names.size() && old_image_names[old] == meta.image_metas.names[logical] &&
                            old < old_image_descs.size() && is_compatible_native_image(old_image_descs[old], create_info))
                        {
                            images[physical] = old_images[old];
                            owned_images[physical] = true;
                            old_images[old] = VK_NULL_HANDLE;
                            old_owned_images[old] = false;
                            break;
                        }
                    }
                }
                if (images[physical] != VK_NULL_HANDLE)
                {
                    continue;
                }
                create_info.flags |= VK_IMAGE_CREATE_ALIAS_BIT;
                if (create_info.sType == 0)
                {
                    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                }
                if (!allocator_dispatch.create_image(image_allocations[block], create_info, images[physical]))
                {
                    report_error("vmaCreateAliasingImage2 failed");
                    continue;
                }
                owned_images[physical] = true;
            }

            buffers.assign(physical_meta.physical_buffer_meta.size(), VK_NULL_HANDLE);
            owned_buffers.assign(buffers.size(), false);
            for (resource_handle physical = 0; physical < physical_meta.physical_buffer_meta.size(); physical++)
            {
                const auto logical = physical_meta.physical_buffer_meta[physical];
                auto create_info = lower_vk_buffer_desc(meta.buffer_metas.descs[logical]);
                physical_buffer_descs.push_back(create_info);
                physical_buffer_names.push_back(meta.buffer_metas.names[logical]);
                const auto block = physical_meta.handle_to_buffer_memory_block[logical];
                physical_buffer_blocks.push_back(block);
                if (meta.buffer_metas.is_imported[logical])
                {
                    const auto bound = pending_imported_buffers.find(logical);
                    if (bound != pending_imported_buffers.end())
                    {
                        buffers[physical] = bound->second.buffer;
                    }
                    continue;
                }
                if (block == invalid_resource || block >= buffer_allocations.size() || buffer_allocations[block] == nullptr)
                {
                    report_error("VMA buffer has no valid memory block");
                    continue;
                }
                const auto old_block = matched_old_buffer_blocks[block];
                if (old_block != invalid_resource)
                {
                    for (resource_handle old = 0; old < old_buffers.size(); old++)
                    {
                        if (old < old_owned_buffers.size() && old_owned_buffers[old] &&
                            old < old_physical_buffer_blocks.size() && old_physical_buffer_blocks[old] == old_block &&
                            old < old_buffer_names.size() && old_buffer_names[old] == meta.buffer_metas.names[logical] &&
                            old < old_buffer_descs.size() && is_compatible_native_buffer(old_buffer_descs[old], create_info))
                        {
                            buffers[physical] = old_buffers[old];
                            owned_buffers[physical] = true;
                            old_buffers[old] = VK_NULL_HANDLE;
                            old_owned_buffers[old] = false;
                            break;
                        }
                    }
                }
                if (buffers[physical] != VK_NULL_HANDLE)
                {
                    continue;
                }
                if (create_info.sType == 0)
                {
                    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                }
                if (!allocator_dispatch.create_buffer(buffer_allocations[block], create_info, buffers[physical]))
                {
                    report_error("vmaCreateAliasingBuffer2 failed");
                    continue;
                }
                owned_buffers[physical] = true;
            }

            retired_resource_batch retired{.safe_after_frame = current_frame + frames_in_flight};
            for (resource_handle old = 0; old < old_images.size(); old++)
            {
                if (old < old_owned_images.size() && old_owned_images[old] && old_images[old] != VK_NULL_HANDLE)
                {
                    retired.images.push_back(old_images[old]);
                }
            }
            for (resource_handle old = 0; old < old_buffers.size(); old++)
            {
                if (old < old_owned_buffers.size() && old_owned_buffers[old] && old_buffers[old] != VK_NULL_HANDLE)
                {
                    retired.buffers.push_back(old_buffers[old]);
                }
            }
            for (const auto allocation : old_image_allocations)
            {
                if (allocation != nullptr)
                {
                    retired.image_allocations.push_back(allocation);
                }
            }
            for (const auto allocation : old_buffer_allocations)
            {
                if (allocation != nullptr)
                {
                    retired.buffer_allocations.push_back(allocation);
                }
            }
            for (const auto& view : old_views)
            {
                if (std::ranges::find(images, view.image) != images.end())
                {
                    view_cache.push_back(view);
                }
                else
                {
                    retired.views.push_back(view.view);
                }
            }
            if (!retired.images.empty() || !retired.buffers.empty() || !retired.image_allocations.empty() ||
                !retired.buffer_allocations.empty() || !retired.views.empty())
            {
                retired_resources.push_back(std::move(retired));
            }
        }

        [[nodiscard]] resource_handle get_physical_image_id(image_handle logical) const
        {
            if (logical >= logical_to_physical_img_id.size())
            {
                return invalid_resource;
            }
            return logical_to_physical_img_id[logical];
        }

        [[nodiscard]] resource_handle get_physical_buffer_id(buffer_handle logical) const
        {
            if (logical >= logical_to_physical_buf_id.size())
            {
                return invalid_resource;
            }
            return logical_to_physical_buf_id[logical];
        }

        [[nodiscard]] native_image_handle get_image(image_handle logical) const
        {
            const auto physical = get_physical_image_id(logical);
            if (physical == invalid_resource || physical >= images.size())
            {
                return VK_NULL_HANDLE;
            }
            return images[physical];
        }

        [[nodiscard]] native_buffer_handle get_buffer(buffer_handle logical) const
        {
            const auto physical = get_physical_buffer_id(logical);
            if (physical == invalid_resource || physical >= buffers.size())
            {
                return VK_NULL_HANDLE;
            }
            return buffers[physical];
        }

        [[nodiscard]] vk_native_buffer_range get_buffer_range(buffer_handle logical) const
        {
            const auto imported = pending_imported_buffers.find(logical);
            if (imported != pending_imported_buffers.end()) return imported->second;
            return vk_native_buffer_range{get_buffer(logical)};
        }

        // Physical resource creation/lifetime is still user-owned at the engine level.
        // This backend provides a minimal implementation to create transient resources
        // from render-graph allocation results (useful for samples/prototyping).

    private:
        struct image_view_entry
        {
            VkImage image = VK_NULL_HANDLE;
            vk_image_view_desc desc{};
            VkImageView view = VK_NULL_HANDLE;
        };

        struct retired_resource_batch
        {
            uint64_t safe_after_frame = 0;
            std::vector<VkImage> images;
            std::vector<VkBuffer> buffers;
            std::vector<vk_allocation_handle> image_allocations;
            std::vector<vk_allocation_handle> buffer_allocations;
            std::vector<VkImageView> views;
        };

        template <typename ResourceMetaT>
        [[nodiscard]] static std::vector<uint64_t> make_block_keys(const ResourceMetaT& meta,
                                                                   const std::vector<resource_handle>& mapping,
                                                                   size_t block_count)
        {
            std::vector<uint64_t> keys(block_count, 0x243f6a8885a308d3ULL);
            for (resource_handle logical = 0; logical < mapping.size(); logical++)
            {
                const auto block = mapping[logical];
                if (block == invalid_resource || block >= keys.size())
                {
                    continue;
                }
                keys[block] = hash_combine(keys[block], std::hash<std::string>{}(meta.names[logical]));
                keys[block] = hash_combine(keys[block], meta.desc_hashes[logical]);
                keys[block] = hash_combine(keys[block], static_cast<uint64_t>(meta.lifetime_classes[logical]));
            }
            return keys;
        }

        template <typename MetaTableT>
        [[nodiscard]] bool can_reuse_plan(const MetaTableT& meta, const physical_resource_meta& physical_meta) const
        {
            if (images.empty() && buffers.empty())
            {
                return false;
            }
            if (physical_image_descs.size() != physical_meta.physical_image_meta.size() ||
                physical_buffer_descs.size() != physical_meta.physical_buffer_meta.size() ||
                image_block_mapping != physical_meta.handle_to_image_memory_block ||
                buffer_block_mapping != physical_meta.handle_to_buffer_memory_block)
            {
                return false;
            }
            for (size_t physical = 0; physical < physical_image_descs.size(); physical++)
            {
                const auto logical = physical_meta.physical_image_meta[physical];
                if (!is_compatible_native_image(physical_image_descs[physical], lower_vk_image_desc(meta.image_metas.descs[logical])))
                {
                    return false;
                }
            }
            for (size_t physical = 0; physical < physical_buffer_descs.size(); physical++)
            {
                const auto logical = physical_meta.physical_buffer_meta[physical];
                if (!is_compatible_native_buffer(physical_buffer_descs[physical], lower_vk_buffer_desc(meta.buffer_metas.descs[logical])))
                {
                    return false;
                }
            }
            return true;
        }

        template <typename MetaTableT>
        void rebind_imported_resources(const MetaTableT& meta, const physical_resource_meta& physical_meta)
        {
            for (resource_handle physical = 0; physical < physical_meta.physical_image_meta.size(); physical++)
            {
                const auto logical = physical_meta.physical_image_meta[physical];
                if (!meta.image_metas.is_imported[logical])
                {
                    continue;
                }
                const auto bound = pending_imported_images.find(logical);
                if (bound != pending_imported_images.end() && images[physical] != bound->second)
                {
                    retire_views_for_image(images[physical]);
                    images[physical] = bound->second;
                }
            }
            for (resource_handle physical = 0; physical < physical_meta.physical_buffer_meta.size(); physical++)
            {
                const auto logical = physical_meta.physical_buffer_meta[physical];
                if (!meta.buffer_metas.is_imported[logical])
                {
                    continue;
                }
                const auto bound = pending_imported_buffers.find(logical);
                if (bound != pending_imported_buffers.end())
                {
                    buffers[physical] = bound->second.buffer;
                }
            }
        }

        void retire_views_for_image(VkImage image)
        {
            retired_resource_batch batch{.safe_after_frame = current_frame + frames_in_flight};
            auto iterator = view_cache.begin();
            while (iterator != view_cache.end())
            {
                if (iterator->image == image)
                {
                    batch.views.push_back(iterator->view);
                    iterator = view_cache.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
            if (!batch.views.empty())
            {
                retired_resources.push_back(std::move(batch));
            }
        }

        void retire_current_resources()
        {
            retired_resource_batch batch{.safe_after_frame = current_frame + frames_in_flight};
            for (size_t index = 0; index < images.size(); index++)
            {
                if (index < owned_images.size() && owned_images[index] && images[index] != VK_NULL_HANDLE)
                {
                    batch.images.push_back(images[index]);
                }
            }
            for (size_t index = 0; index < buffers.size(); index++)
            {
                if (index < owned_buffers.size() && owned_buffers[index] && buffers[index] != VK_NULL_HANDLE)
                {
                    batch.buffers.push_back(buffers[index]);
                }
            }
            batch.image_allocations = std::move(image_allocations);
            batch.buffer_allocations = std::move(buffer_allocations);
            for (const auto& entry : view_cache)
            {
                batch.views.push_back(entry.view);
            }
            if (!batch.images.empty() || !batch.buffers.empty() || !batch.image_allocations.empty() ||
                !batch.buffer_allocations.empty() || !batch.views.empty())
            {
                retired_resources.push_back(std::move(batch));
            }
            images.clear();
            buffers.clear();
            owned_images.clear();
            owned_buffers.clear();
            image_allocations.clear();
            buffer_allocations.clear();
            view_cache.clear();
            physical_image_descs.clear();
            physical_buffer_descs.clear();
            physical_image_names.clear();
            physical_buffer_names.clear();
            physical_image_blocks.clear();
            physical_buffer_blocks.clear();
            image_block_keys.clear();
            buffer_block_keys.clear();
            image_block_requirements.clear();
            buffer_block_requirements.clear();
        }

        void collect_retired(uint64_t completed_frame)
        {
            auto iterator = retired_resources.begin();
            while (iterator != retired_resources.end())
            {
                if (iterator->safe_after_frame > completed_frame)
                {
                    ++iterator;
                    continue;
                }
                for (const auto view : iterator->views)
                {
                    allocator_dispatch.destroy_view(view);
                }
                for (const auto image : iterator->images)
                {
                    allocator_dispatch.destroy_image(image);
                }
                for (const auto buffer : iterator->buffers)
                {
                    allocator_dispatch.destroy_buffer(buffer);
                }
                for (const auto allocation : iterator->image_allocations)
                {
                    if (allocation != nullptr)
                    {
                        allocator_dispatch.free(allocation);
                    }
                }
                for (const auto allocation : iterator->buffer_allocations)
                {
                    if (allocation != nullptr)
                    {
                        allocator_dispatch.free(allocation);
                    }
                }
                iterator = retired_resources.erase(iterator);
            }
        }

        void report_error(const char* msg)
        {
            if (msg == nullptr)
            {
                return;
            }

            last_error = msg;
            if (error_callback) error_callback(msg);
        }

        void report_error(const std::string& msg) { report_error(msg.c_str()); }

        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        vk_queue_family_indices queue_families{};
        vk_allocator_dispatch allocator_dispatch{};
        uint32_t frames_in_flight = 2;
        uint64_t current_frame = 0;

        // Optional host-owned diagnostic callback. If unset, errors remain available through last_error.
        error_callback_t error_callback;

        // Stores the last reported error message (best-effort).
        std::string last_error;

        // Mapping from logical handle -> physical id (filled at compile)
        std::vector<resource_handle> logical_to_physical_img_id;
        std::vector<resource_handle> logical_to_physical_buf_id;

        // Physical tables (one entry per physical id)
        std::vector<VkImage> images;
        std::vector<VkBuffer> buffers;

        std::vector<bool> owned_images;
        std::vector<bool> owned_buffers;
        std::vector<vk_allocation_handle> image_allocations;
        std::vector<vk_allocation_handle> buffer_allocations;
        std::vector<VkImageCreateInfo> physical_image_descs;
        std::vector<VkBufferCreateInfo> physical_buffer_descs;
        std::vector<std::string> physical_image_names;
        std::vector<std::string> physical_buffer_names;
        std::vector<resource_handle> physical_image_blocks;
        std::vector<resource_handle> physical_buffer_blocks;
        std::vector<image_desc> logical_image_descs;
        std::vector<resource_handle> image_block_mapping;
        std::vector<resource_handle> buffer_block_mapping;
        std::vector<uint64_t> image_block_keys;
        std::vector<uint64_t> buffer_block_keys;
        std::vector<allocation_requirements> image_block_requirements;
        std::vector<allocation_requirements> buffer_block_requirements;
        std::vector<image_view_entry> view_cache;
        std::vector<retired_resource_batch> retired_resources;

        // Pending imported bindings (logical -> native)
        std::unordered_map<resource_handle, VkImage> pending_imported_images;
        std::unordered_map<resource_handle, vk_native_buffer_range> pending_imported_buffers;
    };
}
