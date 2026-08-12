#include "vk_runtime.h"

#include <array>
#include <span>

namespace render_graph
{
    namespace
    {
        uint64_t hash_combine(uint64_t seed, uint64_t value) noexcept
        {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
            return seed;
        }

        uint64_t hash_bytes(uint64_t seed, std::span<const std::byte> bytes) noexcept
        {
            for (const std::byte value : bytes) seed = hash_combine(seed, static_cast<uint8_t>(value));
            return seed;
        }

        uint64_t pipeline_key(const vk_graphics_pipeline_desc& desc) noexcept
        {
            uint64_t key = hash_combine(0, 1);
            for (const auto& shader : desc.shaders)
            {
                key = hash_combine(key, shader.stage);
                key = hash_bytes(key, std::as_bytes(std::span(shader.spirv)));
                for (const char value : shader.entry) key = hash_combine(key, static_cast<uint8_t>(value));
            }
            key = hash_bytes(key, std::as_bytes(std::span(desc.vertex_layout.bindings)));
            key = hash_bytes(key, std::as_bytes(std::span(desc.vertex_layout.attributes)));
            key = hash_combine(key, desc.raster.topology);
            key = hash_combine(key, desc.raster.polygon_mode);
            key = hash_combine(key, desc.raster.cull_mode);
            key = hash_combine(key, desc.raster.front_face);
            key = hash_combine(key, desc.raster.depth_test);
            key = hash_combine(key, desc.raster.depth_write);
            key = hash_combine(key, desc.raster.blend);
            for (const VkFormat format : desc.color_formats) key = hash_combine(key, format);
            key = hash_combine(key, desc.depth_format);
            key = hash_combine(key, desc.samples);
            key = hash_bytes(key, std::as_bytes(std::span(desc.push_constants)));
            return key;
        }

        uint64_t pipeline_key(const vk_compute_pipeline_desc& desc) noexcept
        {
            uint64_t key = hash_combine(0, 2);
            key = hash_combine(key, desc.shader.stage);
            key = hash_bytes(key, std::as_bytes(std::span(desc.shader.spirv)));
            for (const char value : desc.shader.entry) key = hash_combine(key, static_cast<uint8_t>(value));
            return hash_bytes(key, std::as_bytes(std::span(desc.push_constants)));
        }
    } // namespace

    vk_runtime_result vk_runtime::create_graphics_pipeline(const vk_graphics_pipeline_desc& desc,
                                                           vk_pipeline_handle& output)
    {
        if (desc.shaders.empty() || desc.color_formats.empty())
            return {.error = "Graphics pipeline requires shaders and at least one color format"};
        const uint64_t key = pipeline_key(desc);
        for (uint32_t index = 0; index < pipeline_table_.rows.size(); index++)
        {
            const auto& row = pipeline_table_.rows[index];
            if (row.alive && row.key == key)
            {
                output = {index, row.generation};
                pipeline_table_.cache_hits++;
                return {};
            }
        }
        if (pipeline_table_.cache == VK_NULL_HANDLE)
        {
            const VkPipelineCacheCreateInfo cache_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            if (vkCreatePipelineCache(device_table_.device, &cache_info, nullptr, &pipeline_table_.cache) != VK_SUCCESS)
                return {.error = "vkCreatePipelineCache failed"};
        }

        std::vector<VkShaderModule> modules;
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        modules.reserve(desc.shaders.size());
        stages.reserve(desc.shaders.size());
        for (const auto& shader : desc.shaders)
        {
            const VkShaderModuleCreateInfo module_info{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = shader.spirv.size() * sizeof(uint32_t),
                .pCode = shader.spirv.data(),
            };
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device_table_.device, &module_info, nullptr, &module) != VK_SUCCESS)
            {
                for (const auto created : modules) vkDestroyShaderModule(device_table_.device, created, nullptr);
                return {.error = "vkCreateShaderModule failed"};
            }
            modules.push_back(module);
            stages.push_back(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = shader.stage,
                .module = module,
                .pName = shader.entry.c_str(),
            });
        }
        const VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &bindless_state_.layout,
            .pushConstantRangeCount = static_cast<uint32_t>(desc.push_constants.size()),
            .pPushConstantRanges = desc.push_constants.data(),
        };
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device_table_.device, &layout_info, nullptr, &layout) != VK_SUCCESS)
        {
            for (const auto module : modules) vkDestroyShaderModule(device_table_.device, module, nullptr);
            return {.error = "vkCreatePipelineLayout failed"};
        }

        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = static_cast<uint32_t>(desc.vertex_layout.bindings.size()),
            .pVertexBindingDescriptions = desc.vertex_layout.bindings.data(),
            .vertexAttributeDescriptionCount = static_cast<uint32_t>(desc.vertex_layout.attributes.size()),
            .pVertexAttributeDescriptions = desc.vertex_layout.attributes.data(),
        };
        const VkPipelineInputAssemblyStateCreateInfo input_assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = desc.raster.topology,
        };
        const VkPipelineViewportStateCreateInfo viewport{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = desc.raster.polygon_mode,
            .cullMode = desc.raster.cull_mode,
            .frontFace = desc.raster.front_face,
            .lineWidth = 1.0F,
        };
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = desc.samples,
        };
        const VkPipelineDepthStencilStateCreateInfo depth{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = desc.raster.depth_test,
            .depthWriteEnable = desc.raster.depth_write,
            .depthCompareOp = VK_COMPARE_OP_LESS,
        };
        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(desc.color_formats.size());
        for (auto& attachment : blend_attachments)
        {
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            attachment.blendEnable = desc.raster.blend;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = static_cast<uint32_t>(blend_attachments.size()),
            .pAttachments = blend_attachments.data(),
        };
        constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const VkPipelineRenderingCreateInfo rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = static_cast<uint32_t>(desc.color_formats.size()),
            .pColorAttachmentFormats = desc.color_formats.data(),
            .depthAttachmentFormat = desc.depth_format,
        };
        const VkGraphicsPipelineCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &rendering,
            .stageCount = static_cast<uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = layout,
        };
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult created = vkCreateGraphicsPipelines(device_table_.device,
                                                           pipeline_table_.cache,
                                                           1,
                                                           &create_info,
                                                           nullptr,
                                                           &pipeline);
        for (const auto module : modules) vkDestroyShaderModule(device_table_.device, module, nullptr);
        if (created != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(device_table_.device, layout, nullptr);
            return {.error = "vkCreateGraphicsPipelines failed with VkResult " + std::to_string(created)};
        }
        const uint32_t index = static_cast<uint32_t>(pipeline_table_.rows.size());
        pipeline_table_.rows.push_back({.key = key, .pipeline = pipeline, .layout = layout,
                                        .bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS, .alive = true});
        output = {index, pipeline_table_.rows[index].generation};
        pipeline_table_.creations++;
        return {};
    }

    vk_runtime_result vk_runtime::create_compute_pipeline(const vk_compute_pipeline_desc& desc,
                                                           vk_pipeline_handle& output)
    {
        if (desc.shader.stage != VK_SHADER_STAGE_COMPUTE_BIT || desc.shader.spirv.empty())
            return {.error = "Compute pipeline requires one compute SPIR-V shader"};
        const uint64_t key = pipeline_key(desc);
        for (uint32_t index = 0; index < pipeline_table_.rows.size(); ++index)
        {
            const auto& row = pipeline_table_.rows[index];
            if (row.alive && row.key == key && row.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE)
            {
                output = {index, row.generation};
                pipeline_table_.cache_hits++;
                return {};
            }
        }
        if (pipeline_table_.cache == VK_NULL_HANDLE)
        {
            const VkPipelineCacheCreateInfo cache_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            if (vkCreatePipelineCache(device_table_.device, &cache_info, nullptr, &pipeline_table_.cache) != VK_SUCCESS)
                return {.error = "vkCreatePipelineCache failed"};
        }
        const VkShaderModuleCreateInfo module_info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = desc.shader.spirv.size() * sizeof(uint32_t),
            .pCode = desc.shader.spirv.data(),
        };
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_table_.device, &module_info, nullptr, &module) != VK_SUCCESS)
            return {.error = "vkCreateShaderModule failed for compute pipeline"};
        const VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &bindless_state_.layout,
            .pushConstantRangeCount = static_cast<uint32_t>(desc.push_constants.size()),
            .pPushConstantRanges = desc.push_constants.data(),
        };
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device_table_.device, &layout_info, nullptr, &layout) != VK_SUCCESS)
        {
            vkDestroyShaderModule(device_table_.device, module, nullptr);
            return {.error = "vkCreatePipelineLayout failed for compute pipeline"};
        }
        const VkPipelineShaderStageCreateInfo shader{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = desc.shader.entry.c_str(),
        };
        const VkComputePipelineCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = shader,
            .layout = layout,
        };
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult created = vkCreateComputePipelines(device_table_.device, pipeline_table_.cache,
                                                          1, &create_info, nullptr, &pipeline);
        vkDestroyShaderModule(device_table_.device, module, nullptr);
        if (created != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(device_table_.device, layout, nullptr);
            return {.error = "vkCreateComputePipelines failed with VkResult " + std::to_string(created)};
        }
        const uint32_t index = static_cast<uint32_t>(pipeline_table_.rows.size());
        pipeline_table_.rows.push_back({.key = key, .pipeline = pipeline, .layout = layout,
                                        .bind_point = VK_PIPELINE_BIND_POINT_COMPUTE, .alive = true});
        output = {index, pipeline_table_.rows[index].generation};
        pipeline_table_.creations++;
        return {};
    }

    VkPipeline vk_runtime::pipeline(vk_pipeline_handle handle) const noexcept
    {
        if (handle.index >= pipeline_table_.rows.size()) return VK_NULL_HANDLE;
        const auto& row = pipeline_table_.rows[handle.index];
        return row.alive && row.generation == handle.generation ? row.pipeline : VK_NULL_HANDLE;
    }

    VkPipelineLayout vk_runtime::pipeline_layout(vk_pipeline_handle handle) const noexcept
    {
        if (handle.index >= pipeline_table_.rows.size()) return VK_NULL_HANDLE;
        const auto& row = pipeline_table_.rows[handle.index];
        return row.alive && row.generation == handle.generation ? row.layout : VK_NULL_HANDLE;
    }

    void vk_runtime::destroy_pipeline(vk_pipeline_handle handle) noexcept
    {
        if (handle.index >= pipeline_table_.rows.size()) return;
        auto& row = pipeline_table_.rows[handle.index];
        if (!row.alive || row.generation != handle.generation) return;
        if (device_table_.device != VK_NULL_HANDLE)
        {
            if (row.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_table_.device, row.pipeline, nullptr);
            if (row.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_table_.device, row.layout, nullptr);
        }
        row.pipeline = VK_NULL_HANDLE;
        row.layout = VK_NULL_HANDLE;
        row.alive = false;
        ++row.generation;
    }

    void vk_runtime::destroy_pipelines() noexcept
    {
        if (device_table_.device == VK_NULL_HANDLE) return;
        for (auto& row : pipeline_table_.rows)
        {
            if (row.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_table_.device, row.pipeline, nullptr);
            if (row.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_table_.device, row.layout, nullptr);
        }
        if (pipeline_table_.cache != VK_NULL_HANDLE)
            vkDestroyPipelineCache(device_table_.device, pipeline_table_.cache, nullptr);
        pipeline_table_ = {};
    }
} // namespace render_graph
