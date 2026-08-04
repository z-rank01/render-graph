#include "render_graph/unit_test/lifetime_aliasing_test.h"

#include <cstdint>
#include <vector>

#include "render_graph/system.h" // IWYU pragma: keep
#include "render_graph/unit_test/system_test_access.h" // IWYU pragma: keep
#include "render_graph/unit_test/test_backend.h" // IWYU pragma: keep
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        using system_t = render_graph_system<test_backend>;
        using pass_setup_context = system_t::pass_setup_context;
        using pass_execute_context = system_t::pass_execute_context;

        test_image_desc make_image_desc(uint32_t w, uint32_t h)
        {
            return test_image_desc{
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = w, .height = h, .depth = 1},
                .usage         = image_usage::SAMPLED,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
            };
        }

        struct test_state_t
        {
            resource_handle r1 = 0;
            resource_handle r2 = 0;
            resource_handle r3 = 0;
            resource_handle r4 = 0;
            resource_handle out = 0;

            void reset() { *this = test_state_t{}; }
        };

        test_state_t& test_state()
        {
            static test_state_t state{};
            return state;
        }

        void noop_execute(pass_execute_context&) {}

        void pass_1_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();
            state.r1 = ctx.create_image("R1", make_image_desc(100, 100), false);
            ctx.write_image(state.r1, image_usage::TRANSFER_DST);
        }

        void pass_2_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();
            ctx.read_image(state.r1, image_usage::TRANSFER_SRC);
            
            state.r2 = ctx.create_image("R2", make_image_desc(100, 100), false);
            ctx.write_image(state.r2, image_usage::TRANSFER_DST);
        }

        void pass_3_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();
            ctx.read_image(state.r2, image_usage::TRANSFER_SRC);

            // R3: Compatible with R1
            state.r3 = ctx.create_image("R3", make_image_desc(100, 100), false);
            ctx.write_image(state.r3, image_usage::TRANSFER_DST);
        }

        void pass_4_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();
            ctx.read_image(state.r3, image_usage::TRANSFER_SRC);
            
            // Output
            state.out = ctx.create_image("Out", make_image_desc(100, 100), false);
            ctx.write_image(state.out, image_usage::TRANSFER_DST);
        }

        void pass_5_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();
            // Keep the chain alive
            ctx.read_image(state.out, image_usage::TRANSFER_SRC);

            // R4: non-overlapping with R1/R3 but NOT compatible (different extent)
            state.r4 = ctx.create_image("R4", make_image_desc(200, 100), false);
            ctx.write_image(state.r4, image_usage::TRANSFER_DST);

            // Root output
            ctx.declare_image_output(state.r4);
        }
    }

    void lifetime_aliasing_test()
    {
        auto& state = test_state();
        state.reset();

        system_t rg;
        
        auto p1 = rg.add_pass(pass_1_setup, noop_execute);
        auto p2 = rg.add_pass(pass_2_setup, noop_execute);
        auto p3 = rg.add_pass(pass_3_setup, noop_execute);
        auto p4 = rg.add_pass(pass_4_setup, noop_execute);
        auto p5 = rg.add_pass(pass_5_setup, noop_execute);

        const auto compile_result = rg.compile();
        RG_CHECK(compile_result.succeeded());

        // 1. Check Sorted Order (Should be P1->P2->P3->P4->P5)
        // Note: Since it's a simple chain, topological sort should respect this.
        // However, indices might differ if implementation changes, but relative order matters.
        
        // Get execution indices
        std::vector<uint32_t> pass_indices(5);
        const auto& sorted = rg.get_sorted_passes();
        for(uint32_t i=0; i<sorted.size(); ++i) {
            pass_indices[sorted[i]] = i;
        }

        uint32_t idx1 = pass_indices[p1];
        uint32_t idx2 = pass_indices[p2];
        uint32_t idx3 = pass_indices[p3];
        uint32_t idx4 = pass_indices[p4];
        uint32_t idx5 = pass_indices[p5];

        RG_CHECK(idx1 < idx2);
        RG_CHECK(idx2 < idx3);
        RG_CHECK(idx3 < idx4);
        RG_CHECK(idx4 < idx5);

        // 2. Check Lifetimes
        // R1: Used in P1(Write), P2(Read). Lifetime: [idx1, idx2]
        const auto& lifetimes = system_test_access::resource_lifetimes(rg);
        RG_CHECK(lifetimes.image_first_used_pass[state.r1] == idx1);
        RG_CHECK(lifetimes.image_last_used_pass[state.r1] == idx2);

        // R2: Used in P2(Write), P3(Read). Lifetime: [idx2, idx3]
        RG_CHECK(lifetimes.image_first_used_pass[state.r2] == idx2);
        RG_CHECK(lifetimes.image_last_used_pass[state.r2] == idx3);

        // R3: Used in P3(Write), P4(Read). Lifetime: [idx3, idx4]
        RG_CHECK(lifetimes.image_first_used_pass[state.r3] == idx3);
        RG_CHECK(lifetimes.image_last_used_pass[state.r3] == idx4);

        // R4: Used in P5(Write). Lifetime: [idx5, idx5]
        RG_CHECK(lifetimes.image_first_used_pass[state.r4] == idx5);
        RG_CHECK(lifetimes.image_last_used_pass[state.r4] == idx5);

        // 3. Check Aliasing
        // R1 [idx1, idx2] and R2 [idx2, idx3] overlap at idx2. Should NOT alias.
        const auto unique_r1 = rg.get_physical_image_id(state.r1);
        const auto unique_r2 = rg.get_physical_image_id(state.r2);
        RG_CHECK(unique_r1 != unique_r2 && "R1 and R2 should not alias (overlap at P2)");

        // R1 [idx1, idx2] and R3 [idx3, idx4]. No overlap (idx2 < idx3). Should alias.
        const auto unique_r3 = rg.get_physical_image_id(state.r3);
        RG_CHECK(unique_r1 == unique_r3 && "R1 and R3 should alias (no overlap)");

        // R4 does not overlap, but meta is different -> must NOT alias.
        const auto unique_r4 = rg.get_physical_image_id(state.r4);
        RG_CHECK(unique_r1 != unique_r4 && "R4 meta differs; should not alias with R1");
    }
}
