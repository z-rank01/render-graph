// Recording of vk_runtime scene commands: turns the *_record descriptors
// (indexed draws, buffer copies, compute dispatches) into vkCmd* calls.

#include "vk_runtime.h"

namespace render_graph
{
    namespace
    {
        // Viewport and scissor follow the pass render area (offset + size)
        // instead of always covering the full swapchain extent.
        void set_area_viewport(VkCommandBuffer commands, render_area area)
        {
            const VkViewport viewport{static_cast<float>(area.x), static_cast<float>(area.y),
                                      static_cast<float>(area.width), static_cast<float>(area.height), 0.0F, 1.0F};
            const VkRect2D scissor{{area.x, area.y}, {area.width, area.height}};
            vkCmdSetViewport(commands, 0, 1, &viewport);
            vkCmdSetScissor(commands, 0, 1, &scissor);
        }
    } // namespace

    // =============================================================================
    // Indexed draw recording
    // =============================================================================

    bool vk_runtime::record_indexed_scene(const vk_indexed_scene_record& desc)
    {
        if (desc.commands == VK_NULL_HANDLE || desc.geometry == VK_NULL_HANDLE || desc.indirect == VK_NULL_HANDLE)
        {
            set_error("record_indexed_scene received an invalid command or buffer handle");
            return false;
        }

        // --- Dynamic state: pass-area viewport and scissor ---
        set_area_viewport(desc.commands, desc.area);

        // Vertex and index data share one buffer, bound once for all groups.
        const VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(desc.commands, 0, 1, &desc.geometry, &vertex_offset);
        vkCmdBindIndexBuffer(desc.commands, desc.geometry, 0, desc.index_type);

        // --- Per-group indirect draws ---
        for (const auto& group : desc.groups)
        {
            if (group.command_count == 0) continue;

            // Resolve the pipeline handle; a stale handle aborts the whole record.
            const VkPipeline native_pipeline = pipeline(group.pipeline);
            const VkPipelineLayout layout = pipeline_layout(group.pipeline);
            if (native_pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE)
            {
                set_error("record_indexed_scene received a stale pipeline handle");
                return false;
            }

            vkCmdBindPipeline(desc.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, native_pipeline);
            vkCmdBindDescriptorSets(desc.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                    &bindless_state_.set, 0, nullptr);
            if (!desc.push_constants.empty())
                vkCmdPushConstants(desc.commands, layout, desc.push_stages, 0,
                                   static_cast<uint32_t>(desc.push_constants.size()), desc.push_constants.data());
            vkCmdDrawIndexedIndirect(desc.commands, desc.indirect, group.command_offset,
                                     group.command_count, desc.indirect_stride);
        }
        return true;
    }

    bool vk_runtime::record_indexed_indirect(const vk_indexed_indirect_record& desc)
    {
        if (desc.commands == VK_NULL_HANDLE)
        {
            set_error("record_indexed_indirect received an invalid command buffer");
            return false;
        }

        // --- Dynamic state: pass-area viewport and scissor ---
        set_area_viewport(desc.commands, desc.area);

        // --- Per-row indirect draws (each row carries its own geometry) ---
        for (const auto& row : desc.rows)
        {
            if (row.draw_count == 0) continue;

            const VkPipeline native_pipeline = pipeline(row.pipeline);
            const VkPipelineLayout layout = pipeline_layout(row.pipeline);
            if (native_pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE ||
                row.vertex_buffer == VK_NULL_HANDLE || row.index_buffer == VK_NULL_HANDLE ||
                row.indirect_buffer == VK_NULL_HANDLE)
            {
                set_error("record_indexed_indirect received a stale pipeline or buffer handle");
                return false;
            }

            vkCmdBindPipeline(desc.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, native_pipeline);
            vkCmdBindDescriptorSets(desc.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                    &bindless_state_.set, 0, nullptr);
            if (!desc.push_constants.empty())
                vkCmdPushConstants(desc.commands, layout, desc.push_stages, 0,
                                   static_cast<uint32_t>(desc.push_constants.size()), desc.push_constants.data());

            vkCmdBindVertexBuffers(desc.commands, 0, 1, &row.vertex_buffer, &row.vertex_offset);
            vkCmdBindIndexBuffer(desc.commands, row.index_buffer, row.index_offset, row.index_type);
            vkCmdDrawIndexedIndirect(desc.commands, row.indirect_buffer, row.indirect_offset,
                                     row.draw_count, row.stride);
        }
        return true;
    }

    // =============================================================================
    // Buffer copy recording
    // =============================================================================

    bool vk_runtime::record_buffer_copies(VkCommandBuffer commands,
                                          std::span<const vk_buffer_copy_command_row> rows)
    {
        if (commands == VK_NULL_HANDLE)
        {
            set_error("record_buffer_copies received an invalid command buffer");
            return false;
        }

        // --- Per-row buffer copies ---
        for (const auto& row : rows)
        {
            if (row.source == VK_NULL_HANDLE || row.destination == VK_NULL_HANDLE || row.size == 0)
            {
                set_error("record_buffer_copies received an invalid buffer range");
                return false;
            }
            const VkBufferCopy copy{row.source_offset, row.destination_offset, row.size};
            vkCmdCopyBuffer(commands, row.source, row.destination, 1, &copy);
        }
        return true;
    }

    // =============================================================================
    // Compute dispatch recording
    // =============================================================================

    bool vk_runtime::record_dispatches(const vk_dispatch_record& desc)
    {
        if (desc.commands == VK_NULL_HANDLE)
        {
            set_error("record_dispatches received an invalid command buffer");
            return false;
        }

        // --- Per-row compute dispatches ---
        for (const auto& row : desc.rows)
        {
            if (row.x == 0 || row.y == 0 || row.z == 0 || row.pipeline.index >= pipeline_table_.rows.size())
            {
                set_error("record_dispatches received an invalid command row");
                return false;
            }

            // A handle is stale unless its table slot is alive with a matching generation.
            const auto& pipeline_row = pipeline_table_.rows[row.pipeline.index];
            if (!pipeline_row.alive || pipeline_row.generation != row.pipeline.generation ||
                pipeline_row.bind_point != VK_PIPELINE_BIND_POINT_COMPUTE)
            {
                set_error("record_dispatches received a stale or non-compute pipeline");
                return false;
            }

            // The per-row push-constant slice must stay inside the shared constant block.
            if (row.push_constant_offset > desc.push_constants.size() ||
                row.push_constant_size > desc.push_constants.size() - row.push_constant_offset)
            {
                set_error("record_dispatches push-constant range is out of bounds");
                return false;
            }

            vkCmdBindPipeline(desc.commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_row.pipeline);
            vkCmdBindDescriptorSets(desc.commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_row.layout,
                                    0, 1, &bindless_state_.set, 0, nullptr);
            if (row.push_constant_size != 0)
                vkCmdPushConstants(desc.commands, pipeline_row.layout, row.push_stages, 0,
                                   row.push_constant_size,
                                   desc.push_constants.data() + row.push_constant_offset);
            vkCmdDispatch(desc.commands, row.x, row.y, row.z);
        }
        return true;
    }
}
