#include "render_graph/unit_test/validation_compile_test.h"

#include <algorithm>

#include "render_graph/compile_result.h"
#include "render_graph/system.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        using system_t             = render_graph_system<test_backend>;
        using pass_setup_context   = system_t::pass_setup_context;
        using pass_execute_context = system_t::pass_execute_context;

        void noop_execute(pass_execute_context&) { }

        [[nodiscard]] test_image_desc image_desc(image_usage usage)
        {
            return test_image_desc{
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 32, .height = 32, .depth = 1},
                .usage         = usage,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
            };
        }

        [[nodiscard]] bool has_code(const compile_result& result, compile_error_code code)
        {
            return std::ranges::any_of(result.diagnostics, [code](const compile_diagnostic& diagnostic) { return diagnostic.code == code; });
        }
    }

    void validation_compile_test()
    {
        // Imported resources may be read without an internal producer.
        {
            system_t system;
            system.add_pass(
                "imported_read",
                [](pass_setup_context& ctx)
                {
                    const auto imported = ctx.create_image("imported", image_desc(image_usage::SAMPLED), true);
                    const auto output   = ctx.create_image("output", image_desc(image_usage::COLOR_ATTACHMENT));
                    ctx.read_image(imported, image_usage::SAMPLED);
                    ctx.write_image(output, image_usage::COLOR_ATTACHMENT);
                    ctx.declare_image_output(output);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(result.succeeded());
        }

        // A non-imported image must be written before it is read.
        {
            system_t system;
            system.add_pass(
                "create_only",
                [](pass_setup_context& ctx) { (void)ctx.create_image("created_only", image_desc(image_usage::SAMPLED)); },
                noop_execute);
            system.add_pass(
                "invalid_reader",
                [](pass_setup_context& ctx)
                {
                    ctx.read_image(resource_handle{0}, image_usage::SAMPLED);
                    const auto output = ctx.create_image("output", image_desc(image_usage::COLOR_ATTACHMENT));
                    ctx.write_image(output, image_usage::COLOR_ATTACHMENT);
                    ctx.declare_image_output(output);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(!result.succeeded());
            RG_CHECK(has_code(result, compile_error_code::image_read_before_write));
            RG_CHECK(result.diagnostics.front().pass_name == "invalid_reader");
            RG_CHECK(result.diagnostics.front().resource_name == "created_only");
        }

        // Bad dependency handles are returned as diagnostics before any table is indexed.
        {
            system_t system;
            system.add_pass(
                "bad_handle",
                [](pass_setup_context& ctx)
                {
                    ctx.read_image(resource_handle{123456}, image_usage::SAMPLED);
                    const auto output = ctx.create_image("output", image_desc(image_usage::COLOR_ATTACHMENT));
                    ctx.write_image(output, image_usage::COLOR_ATTACHMENT);
                    ctx.declare_image_output(output);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(!result.succeeded());
            RG_CHECK(has_code(result, compile_error_code::image_read_out_of_range));
        }

        // Missing graph roots and invalid output handles are ordinary compile errors.
        {
            system_t system;
            system.add_pass(
                "no_output",
                [](pass_setup_context& ctx)
                {
                    const auto image = ctx.create_image("unused", image_desc(image_usage::COLOR_ATTACHMENT));
                    ctx.write_image(image, image_usage::COLOR_ATTACHMENT);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(!result.succeeded());
            RG_CHECK(has_code(result, compile_error_code::no_output));
        }

        {
            system_t system;
            system.add_pass(
                "bad_output",
                [](pass_setup_context& ctx) { ctx.declare_buffer_output(resource_handle{77}); },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(!result.succeeded());
            RG_CHECK(has_code(result, compile_error_code::buffer_output_out_of_range));
        }
    }
} // namespace render_graph::unit_test
