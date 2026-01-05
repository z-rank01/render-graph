#include "render_graph/unit_test/dag_compile_test.h"

#include <cassert>

#include "render_graph/system.h" // IWYU pragma: keep
#include "render_graph/unit_test/test_backend.h" // IWYU pragma: keep

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

        test_buffer_desc make_buffer_desc(uint64_t size, buffer_usage usage)
        {
            return test_buffer_desc{.size = size, .usage = usage};
        }

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

            state.img_a = ctx.create_image(
                "img_a",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 64, .height = 64, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            ctx.write_image(state.img_a, image_usage::COLOR_ATTACHMENT);

            state.buf_a = ctx.create_buffer("buf_a", make_buffer_desc(256, buffer_usage::STORAGE_BUFFER), false);
            ctx.write_buffer(state.buf_a, buffer_usage::STORAGE_BUFFER);
        }

        // Pass 1: read img_a + buf_a, write img_b
        void pass_b_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.img_a, image_usage::SAMPLED);
            ctx.read_buffer(state.buf_a, buffer_usage::STORAGE_BUFFER);

            state.img_b = ctx.create_image(
                "img_b",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 64, .height = 64, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            ctx.write_image(state.img_b, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 2: read img_b, write img_out, declare output
        void pass_c_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.img_b, image_usage::SAMPLED);

            state.img_out = ctx.create_image(
                "img_out",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 64, .height = 64, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            ctx.write_image(state.img_out, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.img_out);
        }
    } // namespace

    void dag_compile_test()
    {
        auto& state = test_state();
        state.reset();

        system_t system;

        system.add_pass(pass_a_setup, noop_execute);
        system.add_pass(pass_b_setup, noop_execute);
        system.add_pass(pass_c_setup, noop_execute);

        system.compile();

        // Expected active passes: all three are reachable from output.
        const auto& active = system.get_active_pass_flags();
        assert(active.size() == 3);
        assert(active[0]);
        assert(active[1]);
        assert(active[2]);

        // Expected edges: 0 -> 1, 1 -> 2
        const auto& dag = system.get_dag();
        const auto& begins = dag.adjacency_begins;
        const auto& adj    = dag.adjacency_list;
        const auto& indeg  = dag.in_degrees;
        const auto& outdeg = dag.out_degrees;

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
