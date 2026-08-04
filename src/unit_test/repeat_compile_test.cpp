#include "render_graph/unit_test/repeat_compile_test.h"

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

        struct graph_resources
        {
            resource_handle intermediate = invalid_resource;
            resource_handle output       = invalid_resource;
        };

        void noop_execute(pass_execute_context&) { }

        [[nodiscard]] test_image_desc image_desc(image_usage usage)
        {
            return test_image_desc{
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = usage,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
            };
        }

        void build_graph(system_t& system, graph_resources& resources)
        {
            system.add_pass(
                "producer",
                [&resources](pass_setup_context& ctx)
                {
                    resources.intermediate = ctx.create_image("intermediate", image_desc(image_usage::COLOR_ATTACHMENT | image_usage::SAMPLED));
                    ctx.write_image(resources.intermediate, image_usage::COLOR_ATTACHMENT);
                },
                noop_execute);

            system.add_pass(
                "consumer",
                [&resources](pass_setup_context& ctx)
                {
                    ctx.read_image(resources.intermediate, image_usage::SAMPLED);
                    resources.output = ctx.create_image("output", image_desc(image_usage::COLOR_ATTACHMENT));
                    ctx.write_image(resources.output, image_usage::COLOR_ATTACHMENT);
                    ctx.declare_image_output(resources.output);
                },
                noop_execute);
        }

        [[nodiscard]] bool equal_barriers(const per_pass_barrier& left, const per_pass_barrier& right)
        {
            return left.pass_begins == right.pass_begins &&
                   left.pass_lengths == right.pass_lengths &&
                   left.types == right.types &&
                   left.kinds == right.kinds &&
                   left.logicals == right.logicals &&
                   left.physicals == right.physicals &&
                   left.src_domains == right.src_domains &&
                   left.dst_domains == right.dst_domains &&
                   left.src_accesses == right.src_accesses &&
                   left.dst_accesses == right.dst_accesses &&
                   left.src_usage_bits == right.src_usage_bits &&
                   left.dst_usage_bits == right.dst_usage_bits &&
                   left.prev_logicals == right.prev_logicals;
        }
    }

    void repeat_compile_test()
    {
        system_t system;
        graph_resources resources;
        build_graph(system, resources);

        const auto first_result = system.compile();
        RG_CHECK(first_result.succeeded());
        const auto first_schedule = system.get_sorted_passes();
        const auto first_barriers = system.get_per_pass_barriers();
        RG_CHECK(system_test_access::image_count(system) == 2);

        const auto second_result = system.compile();
        RG_CHECK(second_result.succeeded());
        RG_CHECK(system_test_access::image_count(system) == 2);
        RG_CHECK(system.get_sorted_passes() == first_schedule);
        RG_CHECK(equal_barriers(system.get_per_pass_barriers(), first_barriers));

        system.clear();
        RG_CHECK(system_test_access::pass_count(system) == 0);
        RG_CHECK(system_test_access::image_count(system) == 0);
        RG_CHECK(system.get_sorted_passes().empty());
        RG_CHECK(system.get_per_pass_barriers().pass_begins.empty());

        graph_resources rebuilt_resources;
        build_graph(system, rebuilt_resources);
        const auto rebuilt_result = system.compile();
        RG_CHECK(rebuilt_result.succeeded());
        RG_CHECK(system.get_sorted_passes() == first_schedule);
        RG_CHECK(equal_barriers(system.get_per_pass_barriers(), first_barriers));
    }
} // namespace render_graph::unit_test
