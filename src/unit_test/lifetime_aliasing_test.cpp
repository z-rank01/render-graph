#include "render_graph/unit_test/lifetime_aliasing_test.h"
#include "render_graph/system.h"
#include <cassert>
#include <iostream>

namespace render_graph::unit_test
{
    namespace
    {
        struct test_state_t
        {
            resource_handle r1 = 0;
            resource_handle r2 = 0;
            resource_handle r3 = 0;
            resource_handle r4 = 0;
            resource_handle out = 0;
        };

        test_state_t state;

        void noop_execute(pass_execute_context&) {}

        void pass_1_setup(pass_setup_context& ctx)
        {
            // R1: 1024 bytes
            state.r1 = ctx.create_image(image_info{
                .name = "R1", .fmt = format::R8G8B8A8_UNORM, .extent = {.width=100, .height=100, .depth=1}, .usage = image_usage::SAMPLED, .imported = false
            });
            ctx.write_image(state.r1, image_usage::TRANSFER_DST);
        }

        void pass_2_setup(pass_setup_context& ctx)
        {
            ctx.read_image(state.r1, image_usage::TRANSFER_SRC);
            
            // R2: 1024 bytes
            state.r2 = ctx.create_image(image_info{
                .name = "R2", .fmt = format::R8G8B8A8_UNORM, .extent = {.width=100, .height=100, .depth=1}, .usage = image_usage::SAMPLED, .imported = false
            });
            ctx.write_image(state.r2, image_usage::TRANSFER_DST);
        }

        void pass_3_setup(pass_setup_context& ctx)
        {
            ctx.read_image(state.r2, image_usage::TRANSFER_SRC);

            // R3: 1024 bytes (Compatible with R1)
            state.r3 = ctx.create_image(image_info{
                .name = "R3", .fmt = format::R8G8B8A8_UNORM, .extent = {.width=100, .height=100, .depth=1}, .usage = image_usage::SAMPLED, .imported = false
            });
            ctx.write_image(state.r3, image_usage::TRANSFER_DST);
        }

        void pass_4_setup(pass_setup_context& ctx)
        {
            ctx.read_image(state.r3, image_usage::TRANSFER_SRC);
            
            // Output
            state.out = ctx.create_image(image_info{
                .name = "Out", .fmt = format::R8G8B8A8_UNORM, .extent = {.width=100, .height=100, .depth=1}, .usage = image_usage::SAMPLED, .imported = false
            });
            ctx.write_image(state.out, image_usage::TRANSFER_DST);
        }

        void pass_5_setup(pass_setup_context& ctx)
        {
            // Keep the chain alive
            ctx.read_image(state.out, image_usage::TRANSFER_SRC);

            // R4: non-overlapping with R1/R3 but NOT compatible (different extent)
            state.r4 = ctx.create_image(image_info{
                .name = "R4", .fmt = format::R8G8B8A8_UNORM, .extent = {.width=200, .height=100, .depth=1}, .usage = image_usage::SAMPLED, .imported = false
            });
            ctx.write_image(state.r4, image_usage::TRANSFER_DST);

            // Root output
            ctx.declare_image_output(state.r4);
        }
    }

    void lifetime_aliasing_test()
    {
        render_graph_system rg;
        
        auto p1 = rg.add_pass(pass_1_setup, noop_execute);
        auto p2 = rg.add_pass(pass_2_setup, noop_execute);
        auto p3 = rg.add_pass(pass_3_setup, noop_execute);
        auto p4 = rg.add_pass(pass_4_setup, noop_execute);
        auto p5 = rg.add_pass(pass_5_setup, noop_execute);

        rg.compile();

        // 1. Check Sorted Order (Should be P1->P2->P3->P4->P5)
        // Note: Since it's a simple chain, topological sort should respect this.
        // However, indices might differ if implementation changes, but relative order matters.
        
        // Get execution indices
        std::vector<uint32_t> pass_indices(rg.graph.passes.size());
        for(uint32_t i=0; i<rg.sorted_passes.size(); ++i) {
            pass_indices[rg.sorted_passes[i]] = i;
        }

        uint32_t idx1 = pass_indices[p1];
        uint32_t idx2 = pass_indices[p2];
        uint32_t idx3 = pass_indices[p3];
        uint32_t idx4 = pass_indices[p4];
        uint32_t idx5 = pass_indices[p5];

        assert(idx1 < idx2);
        assert(idx2 < idx3);
        assert(idx3 < idx4);
        assert(idx4 < idx5);

        // 2. Check Lifetimes
        // R1: Used in P1(Write), P2(Read). Lifetime: [idx1, idx2]
        assert(rg.resource_lifetimes.image_first_used_pass[state.r1] == idx1);
        assert(rg.resource_lifetimes.image_last_used_pass[state.r1] == idx2);

        // R2: Used in P2(Write), P3(Read). Lifetime: [idx2, idx3]
        assert(rg.resource_lifetimes.image_first_used_pass[state.r2] == idx2);
        assert(rg.resource_lifetimes.image_last_used_pass[state.r2] == idx3);

        // R3: Used in P3(Write), P4(Read). Lifetime: [idx3, idx4]
        assert(rg.resource_lifetimes.image_first_used_pass[state.r3] == idx3);
        assert(rg.resource_lifetimes.image_last_used_pass[state.r3] == idx4);

        // R4: Used in P5(Write). Lifetime: [idx5, idx5]
        assert(rg.resource_lifetimes.image_first_used_pass[state.r4] == idx5);
        assert(rg.resource_lifetimes.image_last_used_pass[state.r4] == idx5);

        // 3. Check Aliasing
        // R1 [idx1, idx2] and R2 [idx2, idx3] overlap at idx2. Should NOT alias.
        auto unique_r1 = rg.physical_resource_metas.handle_to_physical_img_id[state.r1];
        auto unique_r2 = rg.physical_resource_metas.handle_to_physical_img_id[state.r2];
        assert(unique_r1 != unique_r2 && "R1 and R2 should not alias (overlap at P2)");

        // R1 [idx1, idx2] and R3 [idx3, idx4]. No overlap (idx2 < idx3). Should alias.
        auto unique_r3 = rg.physical_resource_metas.handle_to_physical_img_id[state.r3];
        assert(unique_r1 == unique_r3 && "R1 and R3 should alias (no overlap)");

        // R4 does not overlap, but meta is different -> must NOT alias.
        auto unique_r4 = rg.physical_resource_metas.handle_to_physical_img_id[state.r4];
        assert(unique_r1 != unique_r4 && "R4 meta differs; should not alias with R1");

        std::cout << "Lifetime & Aliasing Test Passed!" << std::endl;
    }
}
