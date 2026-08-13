// End-to-end sample graph test: a small raster scene (mesh buffer, depth
// target, swapchain color) compiles with the injected upload pass and the
// expected synchronization epilogue is produced.
#include "vulkan_sample_graph_test.h"

#include <array>

#include "render_graph/compiler.h"
#include "test_check.h"

namespace render_graph::unit_test
{
    void vulkan_sample_graph_test()
    {
        // --- Scene description: mesh, swapchain color, transient depth ---
        const std::array resources{
            frame_resource_row{.source = frame_resource_source::transient_buffer, .name = "mesh",
                .buffer_description = {.size = 4096,
                    .usage = buffer_usage::TRANSFER_DST | buffer_usage::VERTEX_BUFFER | buffer_usage::INDEX_BUFFER}},
            frame_resource_row{.source = frame_resource_source::swapchain_image, .name = "swapchain"},
            frame_resource_row{.source = frame_resource_source::transient_image, .name = "depth",
                .image_description = {.fmt = format::D32_SFLOAT, .extent = {1280, 720, 1},
                    .usage = image_usage::DEPTH_STENCIL_ATTACHMENT}},
        };
        const std::array buffers{frame_buffer_access_row{
            .resource = {0}, .usage = buffer_usage::VERTEX_BUFFER | buffer_usage::INDEX_BUFFER,
            .access = access_type::read}};
        const std::array attachments{
            frame_attachment_row{.resource = {1}, .kind = frame_attachment_kind::color},
            frame_attachment_row{.resource = {2}, .kind = frame_attachment_kind::depth_stencil},
        };
        const std::array passes{frame_pass_row{
            .name = "DrawPass", .kind = pass_kind::raster,
            .buffer_accesses = {0, 1}, .attachments = {0, 2}}};
        const frame_plan frame{.cache_key = 7, .resources = resources, .passes = passes,
                               .buffer_accesses = buffers, .attachments = attachments};
        const graph_compile_request request{
            .frame = &frame,
            .environment = {.extent = {1280, 720, 1}, .color_format = format::B8G8R8A8_UNORM,
                            .swapchain_initialized = true, .queues = {.compute = false, .copy = false}},
            .inject_stable_upload_pass = true,
            .upload_buffer_desc = {.size = 4096, .usage = buffer_usage::TRANSFER_SRC,
                                   .memory = memory_domain::upload, .mapping = mapping_policy::persistent},
        };

        // --- Compile and verify the resulting plan ---
        const auto output = compile_graph(request);
        RG_CHECK(output.succeeded());
        RG_CHECK(output.plan.passes.size() == 2);
        RG_CHECK(output.plan.passes.is_backend_upload(0));
        RG_CHECK(output.plan.passes.color_counts[1] == 1);
        RG_CHECK(output.plan.passes.depth_indices[1] != render_graph::invalid_depth_index);
        const auto depth = output.plan.frame_images[2];
        RG_CHECK(output.plan.lifetimes.image_first_used_pass[depth] == pass_handle{1});
        RG_CHECK(output.plan.lifetimes.image_last_used_pass[depth] == pass_handle{1});
        RG_CHECK(output.plan.synchronization.epilogue_length == 1);
        const auto& present = output.plan.synchronization.ops[output.plan.synchronization.epilogue_begin];
        RG_CHECK(present.after.usage_bits == static_cast<uint32_t>(image_usage::PRESENT));
        RG_CHECK(output.plan.submissions.batches.size() == 1);
        RG_CHECK(output.plan.submissions.batches.front().waits_for_external_acquire);
        RG_CHECK(output.plan.submissions.batches.front().signals_external_present);
    }
}
