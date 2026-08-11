#include "render_graph/unit_test/resource_description_lowering_test.h"

#include "render_graph/dx12_resource_lowering.h"
#include "render_graph/metal_resource_lowering.h"
#include "render_graph/system.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"
#include "render_graph/vk_resource_lowering.h"

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

        render_graph_system<test_backend> graph;
        graph.add_copy_pass("InvalidPersistentDeviceBuffer",
                            [](auto& setup)
                            {
                                const auto invalid = setup.create_buffer(buffer_desc{
                                    .size = 64,
                                    .usage = buffer_usage::TRANSFER_DST,
                                    .memory = memory_domain::device_local,
                                    .mapping = mapping_policy::persistent,
                                });
                                setup.write_buffer(invalid, buffer_usage::TRANSFER_DST);
                                setup.declare_buffer_output(invalid);
                            },
                            [](auto&) {});
        const auto result = graph.compile();
        RG_CHECK(!result);
        RG_CHECK(result.diagnostics.front().code == compile_error_code::unsupported_feature);
    }
} // namespace render_graph::unit_test
