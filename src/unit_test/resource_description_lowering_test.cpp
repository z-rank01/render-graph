#include "resource_description_lowering_test.h"

#include <array>

#include "dx12_resource_lowering.h"
#include "metal_resource_lowering.h"
#include "render_graph/compiler.h"
#include "test_check.h"
#include "vk_resource_lowering.h"

namespace render_graph::unit_test
{
    void resource_description_lowering_test()
    {
        const image_desc image{
            .fmt = format::R8G8B8A8_SRGB,
            .extent = {1024, 512, 1},
            .usage = image_usage::TRANSFER_DST | image_usage::SAMPLED,
            .mip_levels = 5,
            .memory = memory_domain::device_local,
            .lifetime = resource_lifetime_class::persistent,
        };
        const auto vk_image = lower_vk_image_desc(image);
        RG_CHECK(vk_image.format == VK_FORMAT_R8G8B8A8_SRGB);
        RG_CHECK(vk_image.extent.width == 1024);
        RG_CHECK((vk_image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
        RG_CHECK(normalize_vk_image_desc(vk_image).fmt == image.fmt);

        const buffer_desc upload{
            .size = 4096,
            .usage = buffer_usage::TRANSFER_SRC | buffer_usage::UNIFORM_BUFFER,
            .memory = memory_domain::upload,
            .mapping = mapping_policy::persistent,
            .lifetime = resource_lifetime_class::persistent,
        };
        const auto vk_buffer = lower_vk_buffer_desc(upload);
        RG_CHECK(vk_buffer.size == upload.size);
        RG_CHECK((vk_buffer.usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) != 0);

        const auto dx_image = lower_dx12_image_desc(image);
        RG_CHECK(dx_image.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        RG_CHECK(dx_image.Width == image.extent.width);
        RG_CHECK(lower_dx12_heap_type(upload.memory) == D3D12_HEAP_TYPE_UPLOAD);

        const auto metal_buffer = lower_metal_buffer_desc(upload);
        RG_CHECK(metal_buffer.storage == metal_storage_mode::shared_memory);
        RG_CHECK(metal_buffer.persistently_mapped);
        RG_CHECK(lower_metal_image_desc(image).pixel_format == metal_pixel_format::rgba8_srgb);

        const std::array contract_resources{
            frame_resource_row{.source = frame_resource_source::transient_buffer, .name = "upload",
                .buffer_description = upload},
            frame_resource_row{.source = frame_resource_source::transient_image, .name = "texture",
                .image_description = image},
            frame_resource_row{.source = frame_resource_source::swapchain_image, .name = "swapchain"},
        };
        const std::array contract_buffer_accesses{frame_buffer_access_row{
            .resource = {0}, .usage = buffer_usage::TRANSFER_SRC, .access = access_type::read}};
        const std::array contract_image_accesses{frame_image_access_row{
            .resource = {1}, .usage = image_usage::TRANSFER_DST, .access = access_type::write}};
        const std::array contract_attachments{frame_attachment_row{.resource = {2}}};
        const std::array contract_passes{
            frame_pass_row{.name = "upload", .kind = pass_kind::copy,
                .buffer_accesses = {0, 1}, .image_accesses = {0, 1}},
            frame_pass_row{.name = "present", .kind = pass_kind::raster,
                .attachments = {0, 1}},
        };
        const frame_plan contract_plan{
            .resources = contract_resources,
            .passes = contract_passes,
            .buffer_accesses = contract_buffer_accesses,
            .image_accesses = contract_image_accesses,
            .attachments = contract_attachments,
        };
        const auto compiled = compile_graph({.frame = &contract_plan, .environment = {
            .extent = {64, 64, 1}, .color_format = format::B8G8R8A8_UNORM}});
        RG_CHECK(compiled);
        const auto& compiled_buffer = compiled.plan.resources.buffer_metas.descs[
            compiled.plan.frame_buffers[0].value];
        const auto& compiled_image = compiled.plan.resources.image_metas.descs[
            compiled.plan.frame_images[1].value];
        RG_CHECK(lower_vk_buffer_desc(compiled_buffer).size == upload.size);
        RG_CHECK(lower_dx12_buffer_desc(compiled_buffer).Width == upload.size);
        RG_CHECK(lower_metal_buffer_desc(compiled_buffer).size == upload.size);
        RG_CHECK(lower_vk_image_desc(compiled_image).format == VK_FORMAT_R8G8B8A8_SRGB);
        RG_CHECK(lower_dx12_image_desc(compiled_image).Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        RG_CHECK(lower_metal_image_desc(compiled_image).pixel_format == metal_pixel_format::rgba8_srgb);

        const std::array resources{
            frame_resource_row{.source = frame_resource_source::transient_buffer, .name = "invalid",
                .buffer_description = {.size = 64, .usage = buffer_usage::TRANSFER_DST,
                    .memory = memory_domain::device_local, .mapping = mapping_policy::persistent}},
            frame_resource_row{.source = frame_resource_source::swapchain_image, .name = "swapchain"},
        };
        const std::array buffer_accesses{frame_buffer_access_row{
            .resource = {0}, .usage = buffer_usage::TRANSFER_DST, .access = access_type::write}};
        const std::array attachments{frame_attachment_row{.resource = {1}}};
        const std::array passes{
            frame_pass_row{.name = "invalid", .kind = pass_kind::copy, .buffer_accesses = {0, 1}},
            frame_pass_row{.name = "present", .kind = pass_kind::raster, .attachments = {0, 1}},
        };
        const frame_plan plan{.resources = resources, .passes = passes,
                              .buffer_accesses = buffer_accesses, .attachments = attachments};
        const auto result = compile_graph({.frame = &plan, .environment = {
            .extent = {64, 64, 1}, .color_format = format::B8G8R8A8_UNORM}});
        RG_CHECK(!result);
        RG_CHECK(result.result.diagnostics.front().code == compile_error_code::unsupported_feature);
    }
} // namespace render_graph::unit_test
