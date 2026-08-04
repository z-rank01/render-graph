#include "render_graph/unit_test/lifetime_aliasing_test.h"

#include <algorithm>
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

        test_image_desc make_image_desc(uint32_t w, uint32_t h, uint32_t memory_type_bits = 1)
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
                .memory_type_bits = memory_type_bits,
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
            state.out = ctx.create_image("Out", make_image_desc(150, 100), false);
            ctx.write_image(state.out, image_usage::TRANSFER_DST);
        }

        void pass_5_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();
            // Keep the chain alive
            ctx.read_image(state.out, image_usage::TRANSFER_SRC);

            // R4: non-overlapping with R1/R3 but NOT compatible (different extent)
            state.r4 = ctx.create_image("R4", make_image_desc(200, 100, 2), false);
            ctx.write_image(state.r4, image_usage::TRANSFER_DST);

            // Root output
            ctx.declare_image_output(state.r4);
        }

        void lifetime_classification_test()
        {
            system_t rg;
            image_handle persistent{};
            image_handle history{};
            image_handle imported{};
            image_handle bridge{};
            image_handle transient{};
            image_handle dedicated{};
            image_handle relay{};
            image_handle compatible_after_dedicated{};

            const auto p0 = rg.add_pass(
                "create retained resources",
                [&](pass_setup_context& ctx)
                {
                    persistent = ctx.create_image("Persistent", make_image_desc(64, 64), resource_lifetime_class::persistent);
                    history = ctx.create_image("History", make_image_desc(64, 64), resource_lifetime_class::history);
                    imported = ctx.create_image("Imported", make_image_desc(64, 64), resource_lifetime_class::imported);
                    ctx.write_image(persistent, image_usage::TRANSFER_DST);
                    ctx.write_image(history, image_usage::TRANSFER_DST);
                    ctx.write_image(imported, image_usage::TRANSFER_DST);
                },
                noop_execute);
            const auto p1 = rg.add_pass(
                "consume retained resources",
                [&](pass_setup_context& ctx)
                {
                    ctx.read_image(persistent, image_usage::TRANSFER_SRC);
                    ctx.read_image(history, image_usage::TRANSFER_SRC);
                    ctx.read_image(imported, image_usage::TRANSFER_SRC);
                    bridge = ctx.create_image("Bridge", make_image_desc(32, 32), resource_lifetime_class::transient);
                    ctx.write_image(bridge, image_usage::TRANSFER_DST);
                },
                noop_execute);
            const auto p2 = rg.add_pass(
                "create transient",
                [&](pass_setup_context& ctx)
                {
                    ctx.read_image(bridge, image_usage::TRANSFER_SRC);
                    transient = ctx.create_image("Transient", make_image_desc(64, 64), resource_lifetime_class::transient);
                    ctx.write_image(transient, image_usage::TRANSFER_DST);
                },
                noop_execute);
            const auto p3 = rg.add_pass(
                "create dedicated",
                [&](pass_setup_context& ctx)
                {
                    ctx.read_image(transient, image_usage::TRANSFER_SRC);
                    auto desc = make_image_desc(96, 96);
                    desc.requires_dedicated = true;
                    dedicated = ctx.create_image("Dedicated", desc, resource_lifetime_class::transient);
                    ctx.write_image(dedicated, image_usage::TRANSFER_DST);
                },
                noop_execute);
            const auto p4 = rg.add_pass(
                "consume dedicated",
                [&](pass_setup_context& ctx)
                {
                    ctx.read_image(dedicated, image_usage::TRANSFER_SRC);
                    relay = ctx.create_image("Relay", make_image_desc(48, 48), resource_lifetime_class::transient);
                    ctx.write_image(relay, image_usage::TRANSFER_DST);
                },
                noop_execute);
            const auto p5 = rg.add_pass(
                "create compatible non-dedicated",
                [&](pass_setup_context& ctx)
                {
                    ctx.read_image(relay, image_usage::TRANSFER_SRC);
                    compatible_after_dedicated =
                        ctx.create_image("CompatibleAfterDedicated", make_image_desc(96, 96), resource_lifetime_class::transient);
                    ctx.write_image(compatible_after_dedicated, image_usage::TRANSFER_DST);
                    ctx.declare_image_output(compatible_after_dedicated);
                },
                noop_execute);

            RG_CHECK(rg.compile().succeeded());
            RG_CHECK(rg.get_sorted_passes() == std::vector<pass_handle>({p0, p1, p2, p3, p4, p5}));

            const auto transient_object = rg.get_physical_image_id(transient);
            RG_CHECK(rg.get_physical_image_id(persistent) != transient_object);
            RG_CHECK(rg.get_physical_image_id(history) != transient_object);
            RG_CHECK(rg.get_physical_image_id(imported) != transient_object);

            RG_CHECK(rg.get_image_memory_block(imported) == invalid_resource);
            const auto persistent_block = rg.get_image_memory_block(persistent);
            const auto history_block = rg.get_image_memory_block(history);
            const auto dedicated_block = rg.get_image_memory_block(dedicated);
            RG_CHECK(persistent_block != invalid_resource);
            RG_CHECK(history_block != invalid_resource);
            RG_CHECK(dedicated_block != invalid_resource);
            RG_CHECK(persistent_block != history_block);
            RG_CHECK(persistent_block != dedicated_block);
            RG_CHECK(history_block != dedicated_block);
            RG_CHECK(rg.get_physical_resource_plan().image_memory_blocks[dedicated_block].requires_dedicated);
            RG_CHECK(rg.get_physical_image_id(dedicated) != rg.get_physical_image_id(compatible_after_dedicated));
            RG_CHECK(dedicated_block != rg.get_image_memory_block(compatible_after_dedicated));
        }

        void subresource_lifetime_union_test()
        {
            system_t rg;
            image_handle image{};
            image_handle output{};

            const image_subresource_range mip0{.base_mip_level = 0, .mip_level_count = 1};
            const image_subresource_range mip1{.base_mip_level = 1, .mip_level_count = 1};

            const auto p0 = rg.add_pass(
                "write mip zero",
                [&](pass_setup_context& ctx)
                {
                    auto desc = make_image_desc(64, 64);
                    desc.mip_levels = 2;
                    image = ctx.create_image("MipChain", desc, false);
                    ctx.write_image(image, image_access_desc{.usage = image_usage::TRANSFER_DST, .subresource = mip0});
                },
                noop_execute);
            const auto p1 = rg.add_pass(
                "write mip one",
                [&](pass_setup_context& ctx)
                {
                    ctx.read_image(image, image_access_desc{.usage = image_usage::TRANSFER_SRC, .subresource = mip0});
                    ctx.write_image(image, image_access_desc{.usage = image_usage::TRANSFER_DST, .subresource = mip1});
                },
                noop_execute);
            const auto p2 = rg.add_pass(
                "consume mip one",
                [&](pass_setup_context& ctx)
                {
                    ctx.read_image(image, image_access_desc{.usage = image_usage::TRANSFER_SRC, .subresource = mip1});
                    output = ctx.create_image("Output", make_image_desc(16, 16), false);
                    ctx.write_image(output, image_usage::TRANSFER_DST);
                    ctx.declare_image_output(output);
                },
                noop_execute);

            RG_CHECK(rg.compile().succeeded());
            RG_CHECK(rg.get_sorted_passes() == std::vector<pass_handle>({p0, p1, p2}));
            const auto& lifetimes = system_test_access::resource_lifetimes(rg);
            RG_CHECK(lifetimes.image_first_used_pass[image] == pass_handle{0});
            RG_CHECK(lifetimes.image_last_used_pass[image] == pass_handle{2});
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

        // Object reuse and memory aliasing are separate plans. R2 and Out have
        // different descs, but compatible, non-overlapping allocation requirements.
        RG_CHECK(rg.get_physical_image_id(state.r2) != rg.get_physical_image_id(state.out));
        RG_CHECK(rg.get_image_memory_block(state.r2) == rg.get_image_memory_block(state.out));

        // R4 requires a disjoint memory type and therefore cannot share either block.
        RG_CHECK(rg.get_image_memory_block(state.r4) != rg.get_image_memory_block(state.r1));
        RG_CHECK(rg.get_image_memory_block(state.r4) != rg.get_image_memory_block(state.r2));

        const auto& handoffs = rg.get_physical_resource_plan().alias_handoffs;
        const auto r1_to_r3 = std::ranges::find_if(
            handoffs,
            [&](const physical_resource_meta::alias_handoff& handoff)
            {
                return handoff.kind == resource_kind::image && handoff.previous == state.r1 && handoff.next == state.r3;
            });
        RG_CHECK(r1_to_r3 != handoffs.end());
        RG_CHECK(r1_to_r3->memory_block == rg.get_image_memory_block(state.r1));
        RG_CHECK(r1_to_r3->at_pass == p3);

        const auto r2_to_out = std::ranges::find_if(
            handoffs,
            [&](const physical_resource_meta::alias_handoff& handoff)
            {
                return handoff.kind == resource_kind::image && handoff.previous == state.r2 && handoff.next == state.out;
            });
        RG_CHECK(r2_to_out != handoffs.end());
        RG_CHECK(r2_to_out->at_pass == p4);

        lifetime_classification_test();
        subresource_lifetime_union_test();
    }
}
