#include "render_graph/unit_test/ordered_subresource_compile_test.h"

#include <algorithm>
#include <type_traits>

#include "render_graph/system.h"
#include "render_graph/unit_test/system_test_access.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        using system_t             = render_graph_system<test_backend>;
        using pass_setup_context   = system_t::pass_setup_context;
        using pass_execute_context = system_t::pass_execute_context;

        static_assert(!std::is_convertible_v<image_handle, buffer_handle>);
        static_assert(!std::is_convertible_v<buffer_handle, image_handle>);
        static_assert(std::is_same_v<decltype(std::declval<pass_setup_context>().create_image(test_image_desc{})), image_handle>);
        static_assert(std::is_same_v<decltype(std::declval<pass_setup_context>().create_buffer(test_buffer_desc{})), buffer_handle>);

        void noop_execute(pass_execute_context&) { }

        [[nodiscard]] test_image_desc make_image_desc()
        {
            return test_image_desc{
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT | image_usage::SAMPLED,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 4,
                .array_layers  = 2,
                .sample_counts = 1,
            };
        }

        [[nodiscard]] image_access_desc image_state(image_usage usage, uint32_t mip)
        {
            return image_access_desc{
                .usage = usage,
                .domain = pipeline_domain::graphics,
                .subresource = image_subresource_range{
                    .aspects = image_aspect::color,
                    .base_mip_level = mip,
                    .mip_level_count = 1,
                    .base_array_layer = 0,
                    .array_layer_count = 1,
                },
            };
        }

        [[nodiscard]] image_access_desc image_state(image_usage usage, const image_subresource_range& range)
        {
            return image_access_desc{.usage = usage, .domain = pipeline_domain::graphics, .subresource = range};
        }

        [[nodiscard]] buffer_access_desc buffer_state(buffer_usage usage, uint64_t offset, uint64_t size)
        {
            return buffer_access_desc{
                .usage = usage,
                .domain = pipeline_domain::compute,
                .bytes = buffer_byte_range{.offset = offset, .size = size},
            };
        }

        [[nodiscard]] bool has_code(const compile_result& result, compile_error_code code)
        {
            return std::ranges::any_of(result.diagnostics, [code](const auto& diagnostic) { return diagnostic.code == code; });
        }

        void verify_image_overlap(const image_subresource_range& writer_range,
                                  const image_subresource_range& reader_range,
                                  bool overlap_expected)
        {
            system_t system;
            image_handle shared = invalid_image;

            system.add_pass(
                "mip0_writer",
                [&shared, writer_range](pass_setup_context& ctx)
                {
                    shared = ctx.create_image("shared", make_image_desc(), true);
                    ctx.write_image(shared, image_state(image_usage::COLOR_ATTACHMENT, writer_range));
                },
                noop_execute);
            system.add_pass(
                "reader",
                [&shared, reader_range](pass_setup_context& ctx)
                {
                    ctx.read_image(shared, image_state(image_usage::SAMPLED, reader_range));
                    const auto output = ctx.create_image("output", make_image_desc());
                    ctx.write_image(output, image_state(image_usage::COLOR_ATTACHMENT, 0));
                    ctx.declare_image_output(output);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(result.succeeded());
            const auto& active = system.get_active_pass_flags();
            RG_CHECK(active.size() == 2);
            RG_CHECK(active[1]);
            RG_CHECK(active[0] == overlap_expected);
            RG_CHECK(system.get_dag().adjacency_list.size() == (overlap_expected ? 1u : 0u));
        }

        void verify_buffer_overlap(bool overlap_expected)
        {
            system_t system;
            buffer_handle shared = invalid_buffer;

            system.add_pass(
                "range_writer",
                [&shared](pass_setup_context& ctx)
                {
                    shared = ctx.create_buffer("shared", test_buffer_desc{.size = 256, .usage = buffer_usage::STORAGE_BUFFER}, true);
                    ctx.write_buffer(shared, buffer_state(buffer_usage::STORAGE_BUFFER, 0, 64));
                },
                noop_execute);
            system.add_pass(
                "range_reader",
                [&shared, overlap_expected](pass_setup_context& ctx)
                {
                    ctx.read_buffer(shared, buffer_state(buffer_usage::STORAGE_BUFFER, overlap_expected ? 32u : 64u, 64));
                    const auto output = ctx.create_buffer("output", test_buffer_desc{.size = 32, .usage = buffer_usage::STORAGE_BUFFER});
                    ctx.write_buffer(output, buffer_state(buffer_usage::STORAGE_BUFFER, 0, 32));
                    ctx.declare_buffer_output(output);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(result.succeeded());
            const auto& active = system.get_active_pass_flags();
            RG_CHECK(active.size() == 2);
            RG_CHECK(active[1]);
            RG_CHECK(active[0] == overlap_expected);
            RG_CHECK(system.get_dag().adjacency_list.size() == (overlap_expected ? 1u : 0u));
        }
    }

    void ordered_subresource_compile_test()
    {
        // write -> read in one pass is legal and remains visible in declaration order.
        {
            system_t system;
            system.add_pass(
                "write_then_read",
                [](pass_setup_context& ctx)
                {
                    const auto image = ctx.create_image("ordered", make_image_desc());
                    ctx.write_image(image, image_state(image_usage::COLOR_ATTACHMENT, 0));
                    ctx.read_image(image, image_state(image_usage::SAMPLED, 0));
                    ctx.declare_image_output(image);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(result.succeeded());
            const auto& accesses = system_test_access::ordered_access_stream(system);
            RG_CHECK(accesses.events.size() == 2);
            RG_CHECK(accesses.events[0].access == access_type::write);
            RG_CHECK(accesses.events[1].access == access_type::read);
        }

        // The inverse order is still a read-before-write error for a transient image.
        {
            system_t system;
            system.add_pass(
                "read_then_write",
                [](pass_setup_context& ctx)
                {
                    const auto image = ctx.create_image("ordered", make_image_desc());
                    ctx.read_image(image, image_state(image_usage::SAMPLED, 0));
                    ctx.write_image(image, image_state(image_usage::COLOR_ATTACHMENT, 0));
                    ctx.declare_image_output(image);
                },
                noop_execute);

            const auto result = system.compile();
            RG_CHECK(!result.succeeded());
            RG_CHECK(has_code(result, compile_error_code::image_read_before_write));
        }

        const image_subresource_range mip0{
            .aspects = image_aspect::color,
            .base_mip_level = 0,
            .mip_level_count = 1,
            .base_array_layer = 0,
            .array_layer_count = 1,
        };
        const image_subresource_range mip1 = {
            .aspects = image_aspect::color,
            .base_mip_level = 1,
            .mip_level_count = 1,
            .base_array_layer = 0,
            .array_layer_count = 1,
        };
        const image_subresource_range layer1 = {
            .aspects = image_aspect::color,
            .base_mip_level = 0,
            .mip_level_count = 1,
            .base_array_layer = 1,
            .array_layer_count = 1,
        };
        const image_subresource_range depth = {
            .aspects = image_aspect::depth,
            .base_mip_level = 0,
            .mip_level_count = 1,
            .base_array_layer = 0,
            .array_layer_count = 1,
        };
        const image_subresource_range broad = {
            .aspects = image_aspect::color,
            .base_mip_level = 0,
            .mip_level_count = 2,
            .base_array_layer = 0,
            .array_layer_count = 2,
        };
        const image_subresource_range inner = {
            .aspects = image_aspect::color,
            .base_mip_level = 1,
            .mip_level_count = 1,
            .base_array_layer = 1,
            .array_layer_count = 1,
        };

        verify_image_overlap(mip0, mip1, false);
        verify_image_overlap(mip0, mip0, true);
        verify_image_overlap(mip0, layer1, false);
        verify_image_overlap(mip0, depth, false);
        verify_image_overlap(broad, inner, true);
        verify_buffer_overlap(false);
        verify_buffer_overlap(true);

        // A partial preceding write does not initialize the entire requested range.
        {
            system_t system;
            system.add_pass(
                "partial_buffer_write",
                [](pass_setup_context& ctx)
                {
                    const auto buffer = ctx.create_buffer(
                        "partial", test_buffer_desc{.size = 256, .usage = buffer_usage::STORAGE_BUFFER});
                    ctx.write_buffer(buffer, buffer_state(buffer_usage::STORAGE_BUFFER, 0, 64));
                    ctx.read_buffer(buffer, buffer_state(buffer_usage::STORAGE_BUFFER, 0, 128));
                    ctx.declare_buffer_output(buffer);
                },
                noop_execute);
            const auto result = system.compile();
            RG_CHECK(!result.succeeded());
            RG_CHECK(has_code(result, compile_error_code::buffer_read_before_write));
        }

        // Access ranges are validated against the backend resource descriptions.
        {
            system_t system;
            system.add_pass(
                "invalid_ranges",
                [](pass_setup_context& ctx)
                {
                    const auto image = ctx.create_image("image", make_image_desc(), true);
                    ctx.read_image(image, image_state(image_usage::SAMPLED, 4));
                    const auto buffer = ctx.create_buffer(
                        "buffer", test_buffer_desc{.size = 256, .usage = buffer_usage::STORAGE_BUFFER}, true);
                    ctx.read_buffer(buffer, buffer_state(buffer_usage::STORAGE_BUFFER, 240, 32));
                    ctx.declare_image_output(image);
                },
                noop_execute);
            const auto result = system.compile();
            RG_CHECK(!result.succeeded());
            RG_CHECK(has_code(result, compile_error_code::invalid_image_subresource_range));
            RG_CHECK(has_code(result, compile_error_code::invalid_buffer_byte_range));
        }
    }
} // namespace render_graph::unit_test
