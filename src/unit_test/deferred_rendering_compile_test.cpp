#include "render_graph/unit_test/deferred_rendering_compile_test.h"

#include "render_graph/system.h" // IWYU pragma: keep
#include "render_graph/unit_test/test_backend.h" // IWYU pragma: keep
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        using system_t = render_graph_system<test_backend>;
        using pass_setup_context = system_t::pass_setup_context;
        using pass_execute_context = system_t::pass_execute_context;

        test_image_desc make_image_desc(format fmt, extent_3d extent, image_usage usage)
        {
            return test_image_desc{
                .fmt           = fmt,
                .extent        = extent,
                .usage         = usage,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
            };
        }

        struct test_state_t
        {
            resource_handle gbuffer_albedo  = 0;
            resource_handle gbuffer_normal  = 0;
            resource_handle gbuffer_depth   = 0;
            resource_handle lighting_hdr    = 0;
            resource_handle tonemap_ldr     = 0;
            resource_handle swapchain_image = 0;
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
            state.gbuffer_albedo = ctx.create_image(
                "gbuffer_albedo",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);

            state.gbuffer_normal = ctx.create_image(
                "gbuffer_normal",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);

            state.gbuffer_depth = ctx.create_image(
                "gbuffer_depth",
                make_image_desc(
                    format::D32_SFLOAT,
                    {.width = 1280, .height = 720, .depth = 1},
                    image_usage::DEPTH_STENCIL_ATTACHMENT),
                false);

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

            state.lighting_hdr = ctx.create_image(
                "lighting_hdr",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);

            ctx.write_image(state.lighting_hdr, image_usage::COLOR_ATTACHMENT);
        }

        void tonemap_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            // Read HDR, write LDR
            ctx.read_image(state.lighting_hdr, image_usage::SAMPLED);

            state.tonemap_ldr = ctx.create_image(
                "tonemap_ldr",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);

            ctx.write_image(state.tonemap_ldr, image_usage::COLOR_ATTACHMENT);
        }

        void swapchain_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            // Read tonemap result, write to an imported swapchain image.
            ctx.read_image(state.tonemap_ldr, image_usage::SAMPLED);

            state.swapchain_image = ctx.create_image(
                "swapchain_backbuffer",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                true);

            ctx.write_image(state.swapchain_image, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.swapchain_image);
        }
    } // namespace

    void deferred_rendering_compile_test()
    {
        system_t system;

        // Add passes in a standard deferred order.
        system.add_pass(gbuffer_setup, noop_execute);
        system.add_pass(lighting_setup, noop_execute);
        system.add_pass(tonemap_setup, noop_execute);
        system.add_pass(swapchain_setup, noop_execute);

        system.compile();
        const auto& schedule = system.get_sorted_passes();
        RG_CHECK(schedule.size() == 4);
        RG_CHECK(schedule[0] == 0 && schedule[1] == 1 && schedule[2] == 2 && schedule[3] == 3);
    }
} // namespace render_graph::unit_test
