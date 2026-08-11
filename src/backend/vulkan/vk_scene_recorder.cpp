#include "vk_runtime.h"

namespace render_graph
{
    bool vk_runtime::record_indexed_scene(const vk_indexed_scene_record& desc)
    {
        if (desc.commands == VK_NULL_HANDLE || desc.geometry == VK_NULL_HANDLE || desc.indirect == VK_NULL_HANDLE)
        {
            set_error("record_indexed_scene received an invalid command or buffer handle");
            return false;
        }
        const VkViewport viewport{0.0F, 0.0F, static_cast<float>(desc.extent.width),
                                  static_cast<float>(desc.extent.height), 0.0F, 1.0F};
        const VkRect2D scissor{{0, 0}, desc.extent};
        vkCmdSetViewport(desc.commands, 0, 1, &viewport);
        vkCmdSetScissor(desc.commands, 0, 1, &scissor);
        const VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(desc.commands, 0, 1, &desc.geometry, &vertex_offset);
        vkCmdBindIndexBuffer(desc.commands, desc.geometry, 0, desc.index_type);
        for (const auto& group : desc.groups)
        {
            if (group.command_count == 0) continue;
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
}
