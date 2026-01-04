#include "render_graph/unit_test/dag_compile_test.h"

#include <cassert>

#include "render_graph/system.h"

namespace render_graph::unit_test
{
    namespace
    {
        struct test_state_t
        {
            resource_handle img_a   = 0;
            resource_handle img_b   = 0;
            resource_handle img_out = 0;
            resource_handle buf_a   = 0;

            void reset() { *this = test_state_t{}; }
        };

        test_state_t& test_state()
        {
            static test_state_t state{};
            return state;
        }

        void noop_execute(pass_execute_context&) { }

        // Pass 0: write img_a + buf_a
        void pass_a_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.img_a = ctx.create_image(image_info{
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
            ctx.write_image(state.img_a, image_usage::COLOR_ATTACHMENT);

            state.buf_a = ctx.create_buffer(buffer_info{
                .name     = "buf_a",
                .size     = 256,
                .usage    = buffer_usage::NONE,
                .imported = false,
            });
            ctx.write_buffer(state.buf_a, buffer_usage::STORAGE_BUFFER);
        }

        // Pass 1: read img_a + buf_a, write img_b
        void pass_b_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.img_a, image_usage::SAMPLED);
            ctx.read_buffer(state.buf_a, buffer_usage::STORAGE_BUFFER);

            state.img_b = ctx.create_image(image_info{
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
            ctx.write_image(state.img_b, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 2: read img_b, write img_out, declare output
        void pass_c_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.img_b, image_usage::SAMPLED);

            state.img_out = ctx.create_image(image_info{
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
            ctx.write_image(state.img_out, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.img_out);
        }
    } // namespace

    void dag_compile_test()
    {
        auto& state = test_state();
        state.reset();

        render_graph_system system;

        system.add_pass(pass_a_setup, noop_execute);
        system.add_pass(pass_b_setup, noop_execute);
        system.add_pass(pass_c_setup, noop_execute);

        system.compile();

        // Expected active passes: all three are reachable from output.
        assert(system.active_pass_flags.size() == 3);
        assert(system.active_pass_flags[0]);
        assert(system.active_pass_flags[1]);
        assert(system.active_pass_flags[2]);

        // Expected edges: 0 -> 1, 1 -> 2
        const auto& begins = system.dag.adjacency_begins;
        const auto& adj    = system.dag.adjacency_list;
        const auto& indeg  = system.dag.in_degrees;
        const auto& outdeg = system.dag.out_degrees;

        assert(begins.size() == 4);
        assert(indeg.size() == 3);
        assert(outdeg.size() == 3);

        // CSR begins
        assert(begins[0] == 0);
        assert(begins[1] == 1);
        assert(begins[2] == 2);
        assert(begins[3] == 2);

        // adjacency list
        assert(adj.size() == 2);
        assert(adj[0] == 1);
        assert(adj[1] == 2);

        // degrees
        assert(indeg[0] == 0);
        assert(indeg[1] == 1);
        assert(indeg[2] == 1);
        assert(outdeg[0] == 1);
        assert(outdeg[1] == 1);
        assert(outdeg[2] == 0);

        (void)system;
    }
} // namespace render_graph::unit_test
