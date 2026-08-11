#include "vk_runtime.h"

#include "vk_resource_lowering.h"

#include <algorithm>
#include <cstring>

namespace render_graph
{
    namespace
    {
        constexpr VkDeviceSize upload_arena_capacity = 64ull * 1024ull * 1024ull;

        [[nodiscard]] VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) noexcept
        {
            return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
        }

        void merge_free_spans(std::vector<vk_buffer_span>& spans)
        {
            std::ranges::sort(spans, {}, &vk_buffer_span::offset);
            std::size_t kept = 0;
            for (const auto span : spans)
            {
                if (span.size == 0) continue;
                if (kept != 0 && spans[kept - 1].offset + spans[kept - 1].size >= span.offset)
                {
                    const VkDeviceSize end = std::max(spans[kept - 1].offset + spans[kept - 1].size,
                                                      span.offset + span.size);
                    spans[kept - 1].size = end - spans[kept - 1].offset;
                }
                else
                {
                    spans[kept++] = span;
                }
            }
            spans.resize(kept);
        }
    } // namespace

    vk_buffer_resource_row* vk_runtime::find_buffer(vk_buffer_resource_handle handle) noexcept
    {
        if (handle.index >= resource_table_.buffers.size()) return nullptr;
        auto& row = resource_table_.buffers[handle.index];
        return row.alive && row.generation == handle.generation ? &row : nullptr;
    }

    const vk_buffer_resource_row* vk_runtime::find_buffer(vk_buffer_resource_handle handle) const noexcept
    {
        if (handle.index >= resource_table_.buffers.size()) return nullptr;
        const auto& row = resource_table_.buffers[handle.index];
        return row.alive && row.generation == handle.generation ? &row : nullptr;
    }

    vk_image_resource_row* vk_runtime::find_image(vk_image_resource_handle handle) noexcept
    {
        if (handle.index >= resource_table_.images.size()) return nullptr;
        auto& row = resource_table_.images[handle.index];
        return row.alive && row.generation == handle.generation ? &row : nullptr;
    }

    const vk_image_resource_row* vk_runtime::find_image(vk_image_resource_handle handle) const noexcept
    {
        if (handle.index >= resource_table_.images.size()) return nullptr;
        const auto& row = resource_table_.images[handle.index];
        return row.alive && row.generation == handle.generation ? &row : nullptr;
    }

    vk_runtime_result vk_runtime::create_buffer(const buffer_desc& desc, vk_buffer_resource_handle& output)
    {
        if (device_table_.allocator == VK_NULL_HANDLE) return {.error = "Vulkan allocator is not initialized"};
        if (desc.size == 0) return {.error = "Cannot create a zero-sized Vulkan buffer"};
        const VkBufferCreateInfo create_info = lower_vk_buffer_desc(desc);
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = desc.memory == memory_domain::device_local
                                    ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
                                    : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        if (desc.memory == memory_domain::device_local)
        {
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
        else
        {
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                    (desc.memory == memory_domain::upload
                                         ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                         : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
        }
        if (desc.allocation == allocation_policy::dedicated) allocation_info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        vk_buffer_resource_row row{
            .desc = desc,
            .generation = 1,
            .alive = true,
        };
        const VkResult created = vmaCreateBuffer(device_table_.allocator,
                                                 &create_info,
                                                 &allocation_info,
                                                 &row.buffer,
                                                 &row.allocation,
                                                 &row.allocation_info);
        if (created != VK_SUCCESS)
            return {.error = "vmaCreateBuffer failed with VkResult " + std::to_string(created)};
        row.mapped = row.allocation_info.pMappedData;

        uint32_t index = UINT32_MAX;
        for (uint32_t candidate = 0; candidate < resource_table_.buffers.size(); candidate++)
        {
            if (!resource_table_.buffers[candidate].alive && resource_table_.buffers[candidate].buffer == VK_NULL_HANDLE)
            {
                index = candidate;
                row.generation = resource_table_.buffers[candidate].generation;
                resource_table_.buffers[candidate] = std::move(row);
                break;
            }
        }
        if (index == UINT32_MAX)
        {
            index = static_cast<uint32_t>(resource_table_.buffers.size());
            resource_table_.buffers.push_back(std::move(row));
        }
        output = {.index = index, .generation = resource_table_.buffers[index].generation};
        allocation_table_.buffer_allocations.push_back(resource_table_.buffers[index].allocation);
        return {};
    }

    VkBuffer vk_runtime::buffer(vk_buffer_resource_handle handle) const noexcept
    {
        const auto* row = find_buffer(handle);
        return row != nullptr ? row->buffer : VK_NULL_HANDLE;
    }

    void* vk_runtime::mapped_buffer(vk_buffer_resource_handle handle) const noexcept
    {
        const auto* row = find_buffer(handle);
        return row != nullptr ? row->mapped : nullptr;
    }

    bool vk_runtime::update_buffer(vk_buffer_resource_handle handle,
                                   VkDeviceSize offset,
                                   std::span<const std::byte> bytes)
    {
        auto* row = find_buffer(handle);
        if (row == nullptr || row->mapped == nullptr || offset + bytes.size() > row->desc.size)
        {
            set_error("update_buffer received an invalid or unmapped range");
            return false;
        }
        std::memcpy(static_cast<std::byte*>(row->mapped) + offset, bytes.data(), bytes.size());
        if (vmaFlushAllocation(device_table_.allocator, row->allocation, offset, bytes.size()) != VK_SUCCESS)
        {
            set_error("vmaFlushAllocation failed");
            return false;
        }
        return true;
    }

    bool vk_runtime::read_buffer(vk_buffer_resource_handle handle, VkDeviceSize offset, std::span<std::byte> bytes)
    {
        auto* row = find_buffer(handle);
        if (row == nullptr || row->mapped == nullptr || offset + bytes.size() > row->desc.size)
        {
            set_error("read_buffer received an invalid or unmapped range");
            return false;
        }
        if (vmaInvalidateAllocation(device_table_.allocator, row->allocation, offset, bytes.size()) != VK_SUCCESS)
        {
            set_error("vmaInvalidateAllocation failed");
            return false;
        }
        std::memcpy(bytes.data(), static_cast<const std::byte*>(row->mapped) + offset, bytes.size());
        return true;
    }

    bool vk_runtime::allocate_buffer_slice(vk_buffer_resource_handle handle,
                                           VkDeviceSize size,
                                           VkDeviceSize alignment,
                                           vk_buffer_slice& output)
    {
        auto* row = find_buffer(handle);
        if (row == nullptr || size == 0)
        {
            set_error("allocate_buffer_slice received an invalid buffer or size");
            return false;
        }
        for (std::size_t index = 0; index < row->free_spans.size(); index++)
        {
            const auto span = row->free_spans[index];
            const VkDeviceSize offset = align_up(span.offset, alignment);
            const VkDeviceSize end = offset + size;
            if (end > span.offset + span.size) continue;
            row->free_spans.erase(row->free_spans.begin() + static_cast<std::ptrdiff_t>(index));
            if (offset > span.offset) row->free_spans.push_back({span.offset, offset - span.offset});
            if (end < span.offset + span.size) row->free_spans.push_back({end, span.offset + span.size - end});
            output = {.buffer = handle, .offset = offset, .size = size};
            return true;
        }
        const VkDeviceSize offset = align_up(row->cursor, alignment);
        if (offset + size > row->desc.size)
        {
            set_error("Vulkan buffer arena capacity exceeded");
            return false;
        }
        row->cursor = offset + size;
        output = {.buffer = handle, .offset = offset, .size = size};
        return true;
    }

    bool vk_runtime::ensure_upload_arena()
    {
        if (find_buffer(resource_table_.upload_arena) != nullptr) return true;
        vk_buffer_resource_handle arena;
        const auto created = create_buffer(buffer_desc{
            .size = upload_arena_capacity,
            .usage = buffer_usage::TRANSFER_SRC,
            .memory = memory_domain::upload,
            .mapping = mapping_policy::persistent,
            .aliasing = aliasing_policy::forbidden,
            .lifetime = resource_lifetime_class::persistent,
        }, arena);
        if (!created)
        {
            set_error(created.error);
            return false;
        }
        resource_table_.upload_arena = arena;
        return true;
    }

    bool vk_runtime::stage_buffer_upload(vk_buffer_slice destination, std::span<const std::byte> bytes)
    {
        if (bytes.empty() || bytes.size() > destination.size || find_buffer(destination.buffer) == nullptr || !ensure_upload_arena())
        {
            set_error("stage_buffer_upload received an invalid range");
            return false;
        }
        vk_buffer_slice staging;
        if (!allocate_buffer_slice(resource_table_.upload_arena, bytes.size(), 16, staging) ||
            !update_buffer(staging.buffer, staging.offset, bytes)) return false;
        resource_table_.pending_buffer_copies.push_back(vk_buffer_copy_row{
            .source = staging.buffer,
            .destination = destination.buffer,
            .region = {.srcOffset = staging.offset, .dstOffset = destination.offset, .size = bytes.size()},
            .staging_slice = staging,
        });
        return true;
    }

    vk_runtime_result vk_runtime::create_image(const image_desc& desc, vk_image_resource_handle& output)
    {
        if (device_table_.allocator == VK_NULL_HANDLE) return {.error = "Vulkan allocator is not initialized"};
        if (desc.fmt == format::UNDEFINED || desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0)
            return {.error = "Cannot create an invalid Vulkan image description"};
        const VkImageCreateInfo create_info = lower_vk_image_desc(desc);
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = desc.memory == memory_domain::device_local
                                    ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
                                    : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        if (desc.memory == memory_domain::device_local) allocation_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        else
        {
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            allocation_info.flags = desc.memory == memory_domain::upload
                                        ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                        : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        }
        if (desc.allocation == allocation_policy::dedicated) allocation_info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        vk_image_resource_row row{.desc = desc, .alive = true};
        const VkResult created = vmaCreateImage(device_table_.allocator,
                                                &create_info,
                                                &allocation_info,
                                                &row.image,
                                                &row.allocation,
                                                &row.allocation_info);
        if (created != VK_SUCCESS) return {.error = "vmaCreateImage failed with VkResult " + std::to_string(created)};
        uint32_t index = UINT32_MAX;
        for (uint32_t candidate = 0; candidate < resource_table_.images.size(); candidate++)
        {
            if (!resource_table_.images[candidate].alive && resource_table_.images[candidate].image == VK_NULL_HANDLE)
            {
                index = candidate;
                row.generation = resource_table_.images[candidate].generation;
                resource_table_.images[candidate] = std::move(row);
                break;
            }
        }
        if (index == UINT32_MAX)
        {
            index = static_cast<uint32_t>(resource_table_.images.size());
            resource_table_.images.push_back(std::move(row));
        }
        output = {.index = index, .generation = resource_table_.images[index].generation};
        allocation_table_.image_allocations.push_back(resource_table_.images[index].allocation);
        return {};
    }

    VkImage vk_runtime::image(vk_image_resource_handle handle) const noexcept
    {
        const auto* row = find_image(handle);
        return row != nullptr ? row->image : VK_NULL_HANDLE;
    }

    bool vk_runtime::stage_image_upload(vk_image_resource_handle destination,
                                        uint32_t mip_level,
                                        uint32_t array_layer,
                                        extent_3d extent,
                                        std::span<const std::byte> bytes)
    {
        const auto* image_row = find_image(destination);
        if (image_row == nullptr || bytes.empty() || mip_level >= image_row->desc.mip_levels ||
            array_layer >= image_row->desc.array_layers || !ensure_upload_arena())
        {
            set_error("stage_image_upload received an invalid image or subresource");
            return false;
        }
        vk_buffer_slice staging;
        if (!allocate_buffer_slice(resource_table_.upload_arena, bytes.size(), 16, staging) ||
            !update_buffer(staging.buffer, staging.offset, bytes)) return false;
        const VkImageAspectFlags aspect = image_row->desc.fmt == format::D32_SFLOAT
                                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                                              : VK_IMAGE_ASPECT_COLOR_BIT;
        resource_table_.pending_image_copies.push_back(vk_image_copy_row{
            .source = staging.buffer,
            .destination = destination,
            .region = {
                .bufferOffset = staging.offset,
                .imageSubresource = {
                    .aspectMask = aspect,
                    .mipLevel = mip_level,
                    .baseArrayLayer = array_layer,
                    .layerCount = 1,
                },
                .imageExtent = {extent.width, extent.height, extent.depth},
            },
            .staging_slice = staging,
        });
        return true;
    }

    bool vk_runtime::has_pending_uploads() const noexcept
    {
        return !resource_table_.pending_buffer_copies.empty() || !resource_table_.pending_image_copies.empty();
    }

    bool vk_runtime::record_pending_uploads(VkCommandBuffer commands)
    {
        for (const auto& copy : resource_table_.pending_buffer_copies)
        {
            const VkBuffer source = buffer(copy.source);
            const VkBuffer destination = buffer(copy.destination);
            if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE)
            {
                set_error("Pending upload references a retired buffer");
                return false;
            }
            vkCmdCopyBuffer(commands, source, destination, 1, &copy.region);
        }
        for (const auto& copy : resource_table_.pending_image_copies)
        {
            const VkBuffer source = buffer(copy.source);
            const VkImage destination = image(copy.destination);
            if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE)
            {
                set_error("Pending image upload references a retired resource");
                return false;
            }
            const VkImageMemoryBarrier2 to_transfer{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image = destination,
                .subresourceRange = {copy.region.imageSubresource.aspectMask,
                                     copy.region.imageSubresource.mipLevel, 1,
                                     copy.region.imageSubresource.baseArrayLayer, 1},
            };
            const VkDependencyInfo before{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &to_transfer,
            };
            vkCmdPipelineBarrier2(commands, &before);
            vkCmdCopyBufferToImage(commands,
                                   source,
                                   destination,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1,
                                   &copy.region);
            const VkImageMemoryBarrier2 to_sampled{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = destination,
                .subresourceRange = to_transfer.subresourceRange,
            };
            const VkDependencyInfo after{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &to_sampled,
            };
            vkCmdPipelineBarrier2(commands, &after);
        }
        return true;
    }

    void vk_runtime::commit_pending_uploads(uint64_t submission)
    {
        for (const auto& copy : resource_table_.pending_buffer_copies)
            resource_table_.retired_buffer_slices.push_back({submission, copy.staging_slice});
        for (const auto& copy : resource_table_.pending_image_copies)
            resource_table_.retired_buffer_slices.push_back({submission, copy.staging_slice});
        resource_table_.pending_buffer_copies.clear();
        resource_table_.pending_image_copies.clear();
    }

    void vk_runtime::release_buffer_slice(vk_buffer_slice slice, uint64_t safe_after_submission)
    {
        if (slice.buffer && slice.size != 0)
            resource_table_.retired_buffer_slices.push_back({safe_after_submission, slice});
    }

    void vk_runtime::destroy_buffer(vk_buffer_resource_handle handle, uint64_t safe_after_submission)
    {
        if (find_buffer(handle) != nullptr) resource_table_.retired_buffers.push_back({safe_after_submission, handle});
    }

    void vk_runtime::destroy_image(vk_image_resource_handle handle, uint64_t safe_after_submission)
    {
        if (find_image(handle) != nullptr) resource_table_.retired_images.push_back({safe_after_submission, handle});
    }

    void vk_runtime::collect_buffer_slices()
    {
        std::size_t kept = 0;
        for (std::size_t index = 0; index < resource_table_.retired_buffer_slices.size(); index++)
        {
            auto& retired = resource_table_.retired_buffer_slices[index];
            if (retired.safe_after_submission <= frame_table_.completed_submission)
            {
                if (auto* row = find_buffer(retired.slice.buffer))
                {
                    row->free_spans.push_back({retired.slice.offset, retired.slice.size});
                    merge_free_spans(row->free_spans);
                }
            }
            else resource_table_.retired_buffer_slices[kept++] = retired;
        }
        resource_table_.retired_buffer_slices.resize(kept);

        kept = 0;
        for (std::size_t index = 0; index < resource_table_.retired_buffers.size(); index++)
        {
            auto& retired = resource_table_.retired_buffers[index];
            if (retired.safe_after_submission <= frame_table_.completed_submission)
            {
                if (auto* row = find_buffer(retired.buffer))
                {
                    vmaDestroyBuffer(device_table_.allocator, row->buffer, row->allocation);
                    row->buffer = VK_NULL_HANDLE;
                    row->allocation = VK_NULL_HANDLE;
                    row->allocation_info = {};
                    row->mapped = nullptr;
                    row->alive = false;
                    row->generation++;
                    row->cursor = 0;
                    row->free_spans.clear();
                }
            }
            else resource_table_.retired_buffers[kept++] = retired;
        }
        resource_table_.retired_buffers.resize(kept);

        kept = 0;
        for (std::size_t index = 0; index < resource_table_.retired_images.size(); index++)
        {
            auto& retired = resource_table_.retired_images[index];
            if (retired.safe_after_submission <= frame_table_.completed_submission)
            {
                if (auto* row = find_image(retired.image))
                {
                    vmaDestroyImage(device_table_.allocator, row->image, row->allocation);
                    row->image = VK_NULL_HANDLE;
                    row->allocation = VK_NULL_HANDLE;
                    row->allocation_info = {};
                    row->alive = false;
                    row->generation++;
                }
            }
            else resource_table_.retired_images[kept++] = retired;
        }
        resource_table_.retired_images.resize(kept);
    }

    void vk_runtime::destroy_resources() noexcept
    {
        if (device_table_.allocator == VK_NULL_HANDLE) return;
        for (auto& row : resource_table_.buffers)
        {
            if (row.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(device_table_.allocator, row.buffer, row.allocation);
        }
        for (auto& row : resource_table_.images)
        {
            if (row.image != VK_NULL_HANDLE) vmaDestroyImage(device_table_.allocator, row.image, row.allocation);
        }
        resource_table_ = {};
        allocation_table_ = {};
    }
} // namespace render_graph
