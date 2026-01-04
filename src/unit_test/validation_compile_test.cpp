#include "render_graph/unit_test/validation_compile_test.h"

#include "render_graph/system.h"

namespace render_graph::unit_test
{
    namespace
    {
        void noop_execute(pass_execute_context&) { }

        enum class validation_case
        {
            imported_read_ok = 0,
            read_before_write_created_resource,
            read_out_of_range_handle,
        };

        constexpr validation_case k_case = validation_case::read_out_of_range_handle;

        // Case 0: imported resource can be read without any internal writer.
        void setup_imported_read_ok(pass_setup_context& ctx)
        {
            const auto imported_tex = ctx.create_image(image_info{
                .name          = "imported_only_read",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 32, .height = 32, .depth = 1},
                .usage         = image_usage::SAMPLED,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });

            // Legal: imported resource has no internal producer.
            ctx.read_image(imported_tex, image_usage::SAMPLED);

            // Produce a real output so StepE output validation passes.
            const auto out_img = ctx.create_image(image_info{
                .name          = "out",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 32, .height = 32, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(out_img, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(out_img);
        }

        // Case 1: create (non-imported) but never write, then read -> should assert.
        void setup_create_only(pass_setup_context& ctx)
        {
            const auto created_only = ctx.create_image(image_info{
                .name          = "created_only",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 16, .height = 16, .depth = 1},
                .usage         = image_usage::SAMPLED,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });

            // Intentionally do not write created_only.
            (void)created_only;
        }

        void setup_read_before_write(pass_setup_context& ctx)
        {
            // This assumes setup_create_only ran before and created handle 0.
            ctx.read_image(static_cast<resource_handle>(0), image_usage::SAMPLED);

            // Keep this pass active with a valid output.
            const auto out_img = ctx.create_image(image_info{
                .name          = "out",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 16, .height = 16, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(out_img, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(out_img);
        }

        // Case 2: read a non-existent handle -> should assert out-of-range.
        void setup_read_out_of_range(pass_setup_context& ctx)
        {
            constexpr resource_handle k_bad_handle = 123456;
            ctx.read_image(k_bad_handle, image_usage::SAMPLED);

            // Keep this pass active with a valid output.
            const auto out_img = ctx.create_image(image_info{
                .name          = "out",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 8, .height = 8, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(out_img, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(out_img);
        }

    } // namespace

    void validation_compile_test()
    {
        render_graph_system system;

        switch (k_case)
        {
        case validation_case::imported_read_ok:
            system.add_pass(setup_imported_read_ok, noop_execute);
            break;

        case validation_case::read_before_write_created_resource:
            system.add_pass(setup_create_only, noop_execute);
            system.add_pass(setup_read_before_write, noop_execute);
            break;

        case validation_case::read_out_of_range_handle:
            system.add_pass(setup_read_out_of_range, noop_execute);
            break;
        }

        // Expected behavior:
        // - imported_read_ok: compile succeeds
        // - read_before_write_created_resource: hits assert in StepE
        // - read_out_of_range_handle: hits assert in StepE
        system.compile();

        (void)system;
    }
} // namespace render_graph::unit_test
