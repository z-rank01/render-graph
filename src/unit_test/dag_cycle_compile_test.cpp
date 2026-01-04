#include "render_graph/unit_test/dag_cycle_compile_test.h"

#include <cassert>

#include "render_graph/system.h"

namespace render_graph::unit_test
{
    namespace
    {
        // 0 = should pass, 1 = should assert (intentional failure)
        constexpr int k_case = 0;

        void noop_execute(pass_execute_context&) { }

        // Acyclic: 0 -> 1 -> 2
        void pass0_setup(pass_setup_context& ctx)
        {
            const auto img_a = ctx.create_image(image_info{
                .name          = "img_a",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(img_a, image_usage::COLOR_ATTACHMENT);
        }

        void pass1_setup(pass_setup_context& ctx)
        {
            // read img_a (handle 0) and write img_b
            ctx.read_image(0, image_usage::SAMPLED);
            const auto img_b = ctx.create_image(image_info{
                .name          = "img_b",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(img_b, image_usage::COLOR_ATTACHMENT);
        }

        void pass2_setup(pass_setup_context& ctx)
        {
            // read img_b (handle 1) and write img_out
            ctx.read_image(1, image_usage::SAMPLED);
            const auto img_out = ctx.create_image(image_info{
                .name          = "img_out",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(img_out, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(img_out);
        }

        void build_acyclic_system(render_graph_system& system)
        {
            system.add_pass(pass0_setup, noop_execute);
            system.add_pass(pass1_setup, noop_execute);
            system.add_pass(pass2_setup, noop_execute);
        }

        [[maybe_unused]] void inject_2node_cycle(render_graph_system& system)
        {
            // Force a simple cycle: 0 -> 1 and 1 -> 0
            system.active_pass_flags.assign(2, true);

            system.dag.adjacency_begins = {0, 1, 2};
            system.dag.adjacency_list   = {1, 0};
            system.dag.in_degrees       = {1, 1};
            system.dag.out_degrees      = {1, 1};
        }
    } // namespace

    void dag_cycle_compile_test()
    {
        if constexpr (k_case == 0)
        {
            render_graph_system system;
            build_acyclic_system(system);

            // Should not assert in Step G cycle check.
            system.compile();

            // Also validate via shared helper (should not assert).
            render_graph_system::assert_no_cycles(system.dag, system.active_pass_flags);
        }
        else if constexpr (k_case == 1)
        {
            render_graph_system system;
            build_acyclic_system(system);
            system.compile();

            // Overwrite the DAG with a known cycle and expect the assert to fire.
            inject_2node_cycle(system);
            render_graph_system::assert_no_cycles(system.dag, system.active_pass_flags);

            // If you hit this line, asserts are disabled.
            assert(false && "Expected cycle assert did not trigger (are you in Release/NDEBUG?)");
        }
    }
} // namespace render_graph::unit_test
