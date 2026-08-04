#include "render_graph/unit_test/vulkan_sample_graph_test.h"

#include <algorithm>

#include "render_graph/system.h"
#include "render_graph/unit_test/system_test_access.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    void vulkan_sample_graph_test()
    {
        using system_t = render_graph_system<test_backend>;
        using setup_context = system_t::pass_setup_context;
        using execute_context = system_t::pass_execute_context;

        bool upload_pending = true;
        uint32_t upload_count = 0;

        for (uint32_t frame = 0; frame < 2; frame++)
        {
            system_t rg;
            buffer_handle staging{};
            buffer_handle local{};
            buffer_handle uniform{};
            image_handle swapchain{};
            image_handle depth{};

            const test_buffer_desc staging_desc{.size = 4096, .usage = buffer_usage::TRANSFER_SRC};
            const test_buffer_desc local_desc{
                .size = 4096,
                .usage = buffer_usage::TRANSFER_DST | buffer_usage::VERTEX_BUFFER | buffer_usage::INDEX_BUFFER,
            };
            const test_buffer_desc uniform_desc{.size = 256, .usage = buffer_usage::UNIFORM_BUFFER};
            const test_image_desc swapchain_desc{
                .fmt = format::B8G8R8A8_UNORM,
                .extent = {.width = 1280, .height = 720, .depth = 1},
                .usage = image_usage::COLOR_ATTACHMENT | image_usage::PRESENT,
            };
            const test_image_desc depth_desc{
                .fmt = format::D32_SFLOAT,
                .extent = {.width = 1280, .height = 720, .depth = 1},
                .usage = image_usage::DEPTH_STENCIL_ATTACHMENT,
            };

            if (upload_pending)
            {
                rg.add_copy_pass("UploadPass", [&](setup_context& ctx)
                {
                    staging = ctx.create_buffer("MeshStaging", staging_desc, resource_lifetime_class::imported);
                    local = ctx.create_buffer("MeshLocal", local_desc, resource_lifetime_class::imported);
                    const buffer_access_desc transfer_src{.usage = buffer_usage::TRANSFER_SRC, .domain = pipeline_domain::copy};
                    ctx.set_initial_state(staging, transfer_src, access_type::read, contents_policy::preserve);
                    ctx.read_buffer(staging, transfer_src);
                    ctx.write_buffer(local, buffer_access_desc{.usage = buffer_usage::TRANSFER_DST, .domain = pipeline_domain::copy});
                }, [&](execute_context& ctx)
                {
                    upload_count++;
                    ctx.commands().record_user_command();
                });
            }

            const auto draw = rg.add_raster_pass("DrawPass", [&](setup_context& ctx)
            {
                if (!upload_pending)
                {
                    local = ctx.create_buffer("MeshLocal", local_desc, resource_lifetime_class::imported);
                    ctx.set_initial_state(
                        local,
                        buffer_access_desc{
                            .usage = buffer_usage::VERTEX_BUFFER | buffer_usage::INDEX_BUFFER,
                            .domain = pipeline_domain::graphics,
                        },
                        access_type::read,
                        contents_policy::preserve);
                }
                uniform = ctx.create_buffer("FrameUniform", uniform_desc, resource_lifetime_class::imported);
                ctx.set_initial_state(uniform,
                                      buffer_access_desc{.usage = buffer_usage::UNIFORM_BUFFER, .domain = pipeline_domain::graphics},
                                      access_type::read,
                                      contents_policy::preserve);
                ctx.read_buffer(local,
                                buffer_access_desc{
                                    .usage = buffer_usage::VERTEX_BUFFER | buffer_usage::INDEX_BUFFER,
                                    .domain = pipeline_domain::graphics,
                                });
                ctx.read_buffer(uniform,
                                buffer_access_desc{.usage = buffer_usage::UNIFORM_BUFFER, .domain = pipeline_domain::graphics});

                swapchain = ctx.create_image("Swapchain", swapchain_desc, resource_lifetime_class::imported);
                const image_access_desc present{.usage = image_usage::PRESENT, .domain = pipeline_domain::graphics};
                ctx.set_initial_state(swapchain, present, access_type::read, contents_policy::preserve);
                ctx.set_final_state(swapchain, present, access_type::read);

                depth = ctx.create_image("Depth", depth_desc, resource_lifetime_class::transient);
                ctx.set_render_area({.width = 1280, .height = 720});
                ctx.add_color_attachment(swapchain, attachment_load_op::clear, attachment_store_op::store);
                ctx.set_depth_stencil_attachment(depth, attachment_load_op::clear, attachment_store_op::dont_care);
                ctx.declare_image_output(swapchain);
            }, [](execute_context& ctx) { ctx.commands().record_user_command(); });

            RG_CHECK(rg.compile().succeeded());
            const auto& lifetimes = system_test_access::resource_lifetimes(rg);
            RG_CHECK(lifetimes.image_first_used_pass[depth] == draw);
            RG_CHECK(lifetimes.image_last_used_pass[depth] == draw);

            const auto& plan = rg.get_synchronization_plan();
            RG_CHECK(plan.epilogue_length == 1);
            const auto& present = plan.ops[plan.epilogue_begin];
            RG_CHECK(present.logical == swapchain);
            RG_CHECK(present.after.usage_bits == static_cast<uint32_t>(image_usage::PRESENT));
            const auto& submissions = rg.get_submission_plan();
            RG_CHECK(std::ranges::count_if(submissions.batches, [](const auto& batch)
            {
                return batch.waits_for_external_acquire;
            }) == 1);
            RG_CHECK(std::ranges::count_if(submissions.batches, [](const auto& batch)
            {
                return batch.signals_external_present;
            }) == 1);

            test_command_context commands;
            RG_CHECK(rg.execute(commands).succeeded());
            RG_CHECK(std::ranges::count_if(commands.records, [](const test_command_record& record)
            {
                return record.kind == test_command_kind::begin_rendering;
            }) == 1);

            upload_pending = false;
        }

        RG_CHECK(upload_count == 1);
    }
}
