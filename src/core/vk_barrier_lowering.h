#pragma once

#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "barrier.h"

namespace render_graph
{
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

    template <typename ImageResolver, typename BufferResolver>
    [[nodiscard]] bool build_vk_barrier_batch(std::span<const synchronization_op> operations,
                                              const vk_queue_family_indices& queue_families,
                                              ImageResolver&& resolve_image,
                                              BufferResolver&& resolve_buffer,
                                              vk_barrier_batch& batch)
    {
        batch.clear();
        for (const auto& operation : operations)
        {
            if (has_intent(operation.intents, synchronization_intent::aliasing))
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

            const auto before = operation.kind == resource_kind::image ? lower_vk_image_state(operation.before)
                                                                       : lower_vk_buffer_state(operation.before);
            const auto after = operation.kind == resource_kind::image ? lower_vk_image_state(operation.after)
                                                                      : lower_vk_buffer_state(operation.after);
            auto src_stages = before.stages;
            auto src_access = before.access;
            auto dst_stages = after.stages;
            auto dst_access = after.access;
            if (operation.phase == synchronization_phase::release)
            {
                dst_stages = VK_PIPELINE_STAGE_2_NONE;
                dst_access = VK_ACCESS_2_NONE;
            }
            else if (operation.phase == synchronization_phase::acquire)
            {
                src_stages = VK_PIPELINE_STAGE_2_NONE;
                src_access = VK_ACCESS_2_NONE;
            }
            uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
            if (has_intent(operation.intents, synchronization_intent::queue_ownership))
            {
                src_queue_family = queue_families.get(operation.before.queue);
                dst_queue_family = queue_families.get(operation.after.queue);
                if (src_queue_family == dst_queue_family)
                {
                    src_queue_family = VK_QUEUE_FAMILY_IGNORED;
                    dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
                }
            }

            if (operation.kind == resource_kind::image)
            {
                const auto image = resolve_image(image_handle{operation.logical});
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
                    .subresourceRange = lower_vk_subresource_range(operation.after),
                });
            }
            else
            {
                const auto buffer = resolve_buffer(buffer_handle{operation.logical});
                if (buffer == VK_NULL_HANDLE)
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
                    .buffer = buffer,
                    .offset = operation.after.buffer_range.offset,
                    .size = operation.after.buffer_range.size == whole_buffer_size ? VK_WHOLE_SIZE
                                                                                   : operation.after.buffer_range.size,
                });
            }
        }
        return true;
    }
}
