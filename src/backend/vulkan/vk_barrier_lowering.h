// Lowers abstract render-graph synchronization ops into native Vulkan
// VkMemoryBarrier2 / VkBufferMemoryBarrier2 / VkImageMemoryBarrier2 batches
// (synchronization2). Header-only; consumed by the Vulkan backend.
#pragma once

#include <type_traits>
#include <vector>

#include <vulkan/vulkan.h>

#include "render_graph/barrier.h"

namespace render_graph
{
    // =============================================================================
    // Native payload types
    // =============================================================================

    struct vk_native_buffer_range
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize base_offset = 0;
        VkDeviceSize size = VK_WHOLE_SIZE;

        vk_native_buffer_range() = default;
        vk_native_buffer_range(VkBuffer native) : buffer(native) {}
        vk_native_buffer_range(VkBuffer native, VkDeviceSize offset, VkDeviceSize range_size)
            : buffer(native), base_offset(offset), size(range_size) {}
    };

    struct vk_queue_family_indices
    {
        uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
        uint32_t compute = VK_QUEUE_FAMILY_IGNORED;
        uint32_t copy = VK_QUEUE_FAMILY_IGNORED;

        [[nodiscard]] uint32_t get(queue_class queue) const noexcept
        {
            switch (queue)
            {
            case queue_class::graphics: return graphics;
            case queue_class::compute: return compute;
            case queue_class::copy: return copy;
            }
            return VK_QUEUE_FAMILY_IGNORED;
        }
    };

    struct vk_lowered_state
    {
        VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 access = VK_ACCESS_2_NONE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct vk_barrier_batch
    {
        std::vector<VkMemoryBarrier2> memory_barriers;
        std::vector<VkBufferMemoryBarrier2> buffer_barriers;
        std::vector<VkImageMemoryBarrier2> image_barriers;

        void clear()
        {
            memory_barriers.clear();
            buffer_barriers.clear();
            image_barriers.clear();
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return memory_barriers.empty() && buffer_barriers.empty() && image_barriers.empty();
        }

        [[nodiscard]] VkDependencyInfo dependency_info() const noexcept
        {
            return VkDependencyInfo{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .dependencyFlags = 0,
                .memoryBarrierCount = static_cast<uint32_t>(memory_barriers.size()),
                .pMemoryBarriers = memory_barriers.data(),
                .bufferMemoryBarrierCount = static_cast<uint32_t>(buffer_barriers.size()),
                .pBufferMemoryBarriers = buffer_barriers.data(),
                .imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers.size()),
                .pImageMemoryBarriers = image_barriers.data(),
            };
        }
    };

    // =============================================================================
    // State lowering helpers
    // =============================================================================

    [[nodiscard]] inline VkPipelineStageFlags2 vk_shader_stages(pipeline_domain domain) noexcept
    {
        switch (domain)
        {
        case pipeline_domain::graphics: return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        case pipeline_domain::compute: return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case pipeline_domain::copy: return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case pipeline_domain::any: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }

    [[nodiscard]] inline bool state_reads(access_type access) noexcept
    {
        return access == access_type::read || access == access_type::read_write;
    }

    [[nodiscard]] inline bool state_writes(access_type access) noexcept
    {
        return access == access_type::write || access == access_type::read_write;
    }

    // Images: usages are checked in priority order and the first match wins,
    // so a state carries exactly one layout.
    [[nodiscard]] inline vk_lowered_state lower_vk_image_state(const abstract_resource_state& state) noexcept
    {
        const auto usage = static_cast<image_usage>(state.usage_bits);
        vk_lowered_state lowered{};
        if (usage == image_usage::NONE)
        {
            return lowered;
        }
        if ((usage & image_usage::PRESENT) != image_usage::NONE)
        {
            lowered.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            lowered.stages = VK_PIPELINE_STAGE_2_NONE;
            return lowered;
        }
        if ((usage & image_usage::STORAGE) != image_usage::NONE)
        {
            lowered.layout = VK_IMAGE_LAYOUT_GENERAL;
            lowered.stages = vk_shader_stages(state.domain);
            if (state_reads(state.access))
            {
                lowered.access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            }
            if (state_writes(state.access))
            {
                lowered.access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            }
            return lowered;
        }
        if ((usage & image_usage::COLOR_ATTACHMENT) != image_usage::NONE)
        {
            lowered.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            lowered.stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            if (state_reads(state.access))
            {
                lowered.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
            }
            if (state_writes(state.access))
            {
                lowered.access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            }
            return lowered;
        }
        if ((usage & image_usage::DEPTH_STENCIL_ATTACHMENT) != image_usage::NONE)
        {
            lowered.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            lowered.stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            if (state_reads(state.access))
            {
                lowered.access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            }
            if (state_writes(state.access))
            {
                lowered.access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            }
            return lowered;
        }
        if ((usage & image_usage::TRANSFER_DST) != image_usage::NONE)
        {
            lowered.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            lowered.stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            lowered.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            return lowered;
        }
        if ((usage & image_usage::TRANSFER_SRC) != image_usage::NONE)
        {
            lowered.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            lowered.stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            lowered.access = VK_ACCESS_2_TRANSFER_READ_BIT;
            return lowered;
        }
        if ((usage & image_usage::SAMPLED) != image_usage::NONE)
        {
            lowered.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            lowered.stages = vk_shader_stages(state.domain);
            lowered.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        return lowered;
    }

    // Buffers: unlike images, all usage bits accumulate into one stage/access set.
    [[nodiscard]] inline vk_lowered_state lower_vk_buffer_state(const abstract_resource_state& state) noexcept
    {
        const auto usage = static_cast<buffer_usage>(state.usage_bits);
        vk_lowered_state lowered{};
        lowered.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((usage & buffer_usage::TRANSFER_DST) != buffer_usage::NONE)
        {
            lowered.stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            lowered.access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
        if ((usage & buffer_usage::TRANSFER_SRC) != buffer_usage::NONE)
        {
            lowered.stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            lowered.access |= VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        if ((usage & buffer_usage::VERTEX_BUFFER) != buffer_usage::NONE)
        {
            lowered.stages |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
            lowered.access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        }
        if ((usage & buffer_usage::INDEX_BUFFER) != buffer_usage::NONE)
        {
            lowered.stages |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            lowered.access |= VK_ACCESS_2_INDEX_READ_BIT;
        }
        if ((usage & buffer_usage::INDIRECT_BUFFER) != buffer_usage::NONE)
        {
            lowered.stages |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            lowered.access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        if ((usage & buffer_usage::UNIFORM_BUFFER) != buffer_usage::NONE)
        {
            lowered.stages |= vk_shader_stages(state.domain);
            lowered.access |= VK_ACCESS_2_UNIFORM_READ_BIT;
        }
        if ((usage & buffer_usage::STORAGE_BUFFER) != buffer_usage::NONE)
        {
            lowered.stages |= vk_shader_stages(state.domain);
            if (state_reads(state.access))
            {
                lowered.access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            }
            if (state_writes(state.access))
            {
                lowered.access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            }
        }
        return lowered;
    }

    [[nodiscard]] inline VkImageAspectFlags lower_vk_aspects(image_aspect aspects, uint32_t usage_bits) noexcept
    {
        if (aspects == image_aspect::all)
        {
            return (usage_bits & static_cast<uint32_t>(image_usage::DEPTH_STENCIL_ATTACHMENT)) != 0
                       ? VK_IMAGE_ASPECT_DEPTH_BIT
                       : VK_IMAGE_ASPECT_COLOR_BIT;
        }
        VkImageAspectFlags result = 0;
        if ((aspects & image_aspect::color) != image_aspect::none)
        {
            result |= VK_IMAGE_ASPECT_COLOR_BIT;
        }
        if ((aspects & image_aspect::depth) != image_aspect::none)
        {
            result |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((aspects & image_aspect::stencil) != image_aspect::none)
        {
            result |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return result;
    }

    [[nodiscard]] inline VkImageSubresourceRange lower_vk_subresource_range(const abstract_resource_state& state) noexcept
    {
        return VkImageSubresourceRange{
            .aspectMask = lower_vk_aspects(state.image_range.aspects, state.usage_bits),
            .baseMipLevel = state.image_range.base_mip_level,
            .levelCount = state.image_range.mip_level_count == remaining_subresources ? VK_REMAINING_MIP_LEVELS
                                                                                      : state.image_range.mip_level_count,
            .baseArrayLayer = state.image_range.base_array_layer,
            .layerCount = state.image_range.array_layer_count == remaining_subresources ? VK_REMAINING_ARRAY_LAYERS
                                                                                         : state.image_range.array_layer_count,
        };
    }

    // =============================================================================
    // Barrier batch building
    // =============================================================================

    // Lowers one contiguous op segment of a kind-split op table and appends it
    // to `batch`; returns false on the first op that cannot be resolved or is
    // out of range (batch is partial). The kind is fixed by the table type, so
    // there is no runtime kind dispatch.
    template <typename OpTable, typename ImageResolver, typename BufferResolver>
    [[nodiscard]] bool build_vk_barrier_batch(const OpTable& ops, uint32_t begin, uint32_t length,
                                              const vk_queue_family_indices& queue_families,
                                              const ImageResolver& resolve_image,
                                              const BufferResolver& resolve_buffer,
                                              vk_barrier_batch& batch)
    {
        batch.clear();
        for (uint32_t row = begin; row < begin + length; ++row)
        {
            // --- Aliasing intent: one global memory barrier covers it ---
            if (has_intent(ops.intents[row], synchronization_intent::aliasing))
            {
                batch.memory_barriers.push_back(VkMemoryBarrier2{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .pNext = nullptr,
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                });
                continue;
            }

            // --- Reassemble the abstract before/after states from the columns ---
            abstract_resource_state before_state;
            before_state.usage_bits = ops.before_usage_bits[row];
            before_state.access     = ops.before_accesses[row];
            before_state.domain     = ops.before_domains[row];
            before_state.queue      = ops.before_queues[row];
            abstract_resource_state after_state;
            after_state.usage_bits = ops.after_usage_bits[row];
            after_state.access     = ops.after_accesses[row];
            after_state.domain     = ops.after_domains[row];
            after_state.queue      = ops.after_queues[row];
            if constexpr (std::is_same_v<OpTable, image_sync_op_table>)
            {
                before_state.image_range = ops.before_ranges[row];
                after_state.image_range  = ops.after_ranges[row];
            }
            else
            {
                before_state.buffer_range = ops.before_ranges[row];
                after_state.buffer_range  = ops.after_ranges[row];
            }

            // --- Lower abstract before/after states ---
            vk_lowered_state before;
            vk_lowered_state after;
            if constexpr (std::is_same_v<OpTable, image_sync_op_table>)
            {
                before = lower_vk_image_state(before_state);
                after  = lower_vk_image_state(after_state);
            }
            else
            {
                before = lower_vk_buffer_state(before_state);
                after  = lower_vk_buffer_state(after_state);
            }
            auto src_stages = before.stages;
            auto src_access = before.access;
            auto dst_stages = after.stages;
            auto dst_access = after.access;
            // --- Release/acquire: keep only the relevant half of the dependency ---
            if (ops.phases[row] == synchronization_phase::release)
            {
                dst_stages = VK_PIPELINE_STAGE_2_NONE;
                dst_access = VK_ACCESS_2_NONE;
            }
            else if (ops.phases[row] == synchronization_phase::acquire)
            {
                src_stages = VK_PIPELINE_STAGE_2_NONE;
                src_access = VK_ACCESS_2_NONE;
            }

            // --- Queue family ownership transfer ---
            uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
            if (has_intent(ops.intents[row], synchronization_intent::queue_ownership))
            {
                src_queue_family = queue_families.get(ops.before_queues[row]);
                dst_queue_family = queue_families.get(ops.after_queues[row]);
                if (src_queue_family == dst_queue_family)
                {
                    src_queue_family = VK_QUEUE_FAMILY_IGNORED;
                    dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
                }
            }

            if constexpr (std::is_same_v<OpTable, image_sync_op_table>)
            {
                const auto image = resolve_image(image_handle{ops.logicals[row]});
                if (image == VK_NULL_HANDLE)
                {
                    return false;
                }
                batch.image_barriers.push_back(VkImageMemoryBarrier2{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext = nullptr,
                    .srcStageMask = src_stages,
                    .srcAccessMask = src_access,
                    .dstStageMask = dst_stages,
                    .dstAccessMask = dst_access,
                    .oldLayout = before.layout,
                    .newLayout = after.layout,
                    .srcQueueFamilyIndex = src_queue_family,
                    .dstQueueFamilyIndex = dst_queue_family,
                    .image = image,
                    .subresourceRange = lower_vk_subresource_range(after_state),
                });
            }
            else
            {
                const vk_native_buffer_range buffer = resolve_buffer(buffer_handle{ops.logicals[row]});
                if (buffer.buffer == VK_NULL_HANDLE)
                {
                    return false;
                }
                const auto logical_offset = ops.after_ranges[row].offset;
                const auto logical_size = ops.after_ranges[row].size;
                // The logical range must fit inside the resolved native range.
                if (logical_offset > buffer.size ||
                    (logical_size != whole_buffer_size && logical_size > buffer.size - logical_offset))
                {
                    return false;
                }
                batch.buffer_barriers.push_back(VkBufferMemoryBarrier2{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .pNext = nullptr,
                    .srcStageMask = src_stages,
                    .srcAccessMask = src_access,
                    .dstStageMask = dst_stages,
                    .dstAccessMask = dst_access,
                    .srcQueueFamilyIndex = src_queue_family,
                    .dstQueueFamilyIndex = dst_queue_family,
                    .buffer = buffer.buffer,
                    .offset = buffer.base_offset + logical_offset,
                    .size = logical_size == whole_buffer_size ? buffer.size - logical_offset : logical_size,
                });
            }
        }
        return true;
    }
}
