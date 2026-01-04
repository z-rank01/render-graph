#include "render_graph/unit_test/deferred_rendering_compile_test.h"

#include "render_graph/system.h"

namespace render_graph::unit_test
{
    namespace
    {
        struct test_state_t
        {
            resource_version_handle gbuffer_albedo  = 0;
            resource_version_handle gbuffer_normal  = 0;
            resource_version_handle gbuffer_depth   = 0;
            resource_version_handle lighting_hdr    = 0;
            resource_version_handle tonemap_ldr     = 0;
            resource_version_handle swapchain_image = 0;
        };

        test_state_t& test_state()
        {
            static test_state_t state{};
            return state;
        }

        void noop_execute(pass_execute_context&) { }

        void gbuffer_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            // Typical GBuffer outputs
            state.gbuffer_albedo = ctx.create_image(image_info{
                .name          = "gbuffer_albedo",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });

            state.gbuffer_normal = ctx.create_image(image_info{
                .name          = "gbuffer_normal",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });

            state.gbuffer_depth = ctx.create_image(image_info{
                .name          = "gbuffer_depth",
                .fmt           = format::D32_SFLOAT,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::DEPTH_STENCIL_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });

            ctx.write_image(state.gbuffer_albedo, image_usage::COLOR_ATTACHMENT);
            ctx.write_image(state.gbuffer_normal, image_usage::COLOR_ATTACHMENT);
            ctx.write_image(state.gbuffer_depth, image_usage::DEPTH_STENCIL_ATTACHMENT);
        }

        void lighting_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            // Read GBuffer, write lighting accumulation
            ctx.read_image(state.gbuffer_albedo, image_usage::SAMPLED);
            ctx.read_image(state.gbuffer_normal, image_usage::SAMPLED);
            ctx.read_image(state.gbuffer_depth, image_usage::SAMPLED);

            state.lighting_hdr = ctx.create_image(image_info{
                .name          = "lighting_hdr",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });

            ctx.write_image(state.lighting_hdr, image_usage::COLOR_ATTACHMENT);
        }

        void tonemap_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            // Read HDR, write LDR
            ctx.read_image(state.lighting_hdr, image_usage::SAMPLED);

            state.tonemap_ldr = ctx.create_image(image_info{
                .name          = "tonemap_ldr",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });

            ctx.write_image(state.tonemap_ldr, image_usage::COLOR_ATTACHMENT);
        }

        void swapchain_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            // Read tonemap result, write to an imported swapchain image.
            ctx.read_image(state.tonemap_ldr, image_usage::SAMPLED);

            state.swapchain_image = ctx.create_image(image_info{
                .name          = "swapchain_backbuffer",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });

            ctx.write_image(state.swapchain_image, image_usage::COLOR_ATTACHMENT);
        }
    } // namespace

    void deferred_rendering_compile_test()
    {
        render_graph_system system;

        // Add passes in a standard deferred order.
        system.add_pass(gbuffer_setup, noop_execute);
        system.add_pass(lighting_setup, noop_execute);
        system.add_pass(tonemap_setup, noop_execute);
        system.add_pass(swapchain_setup, noop_execute);

        system.compile();
    }
} // namespace render_graph::unit_test
