#include "render_graph/unit_test/barrier_plan_test.h"

#include <cstdint>

#include "render_graph/system.h" // IWYU pragma: keep
#include "render_graph/unit_test/test_backend.h" // IWYU pragma: keep
#include "render_graph/unit_test/test_check.h"

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
            resource_handle buf_hist = 0;

            resource_handle tmp_ping = 0;
            resource_handle tmp_pong = 0;

            resource_handle g_albedo = 0;
            resource_handle g_normal = 0;
            resource_handle g_depth  = 0;

            resource_handle lighting_hdr = 0;
            resource_handle tonemap_ldr  = 0;
            resource_handle swapchain    = 0;

            void reset() { *this = test_state_t{}; }
        };

        test_state_t& test_state()
        {
            static test_state_t state{};
            return state;
        }

        void noop_execute(pass_execute_context&) {}

        // Pass 0: compute writes a storage buffer, and writes a transient color image (tmp_ping).
        void compute_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.buf_hist = ctx.create_buffer("histogram", make_buffer_desc(1024, buffer_usage::STORAGE_BUFFER), false);
            ctx.write_buffer(state.buf_hist, buffer_usage::STORAGE_BUFFER);

            state.tmp_ping = ctx.create_image(
                "tmp_ping",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 256, .height = 256, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            ctx.write_image(state.tmp_ping, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 1: GBuffer writes albedo/normal/depth, and reads the compute buffer.
        void gbuffer_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_buffer(state.buf_hist, buffer_usage::STORAGE_BUFFER);

            state.g_albedo = ctx.create_image(
                "gbuffer_albedo",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            state.g_normal = ctx.create_image(
                "gbuffer_normal",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            state.g_depth = ctx.create_image(
                "gbuffer_depth",
                make_image_desc(
                    format::D32_SFLOAT, {.width = 1280, .height = 720, .depth = 1}, image_usage::DEPTH_STENCIL_ATTACHMENT),
                false);

            ctx.write_image(state.g_albedo, image_usage::COLOR_ATTACHMENT);
            ctx.write_image(state.g_normal, image_usage::COLOR_ATTACHMENT);
            ctx.write_image(state.g_depth, image_usage::DEPTH_STENCIL_ATTACHMENT);
        }

        // Pass 2: lighting reads GBuffer as sampled, writes HDR, and creates another transient color image.
        // tmp_pong is descriptor-compatible with tmp_ping and its lifetime doesn't overlap -> should alias.
        void lighting_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.g_albedo, image_usage::SAMPLED);
            ctx.read_image(state.g_normal, image_usage::SAMPLED);
            ctx.read_image(state.g_depth, image_usage::SAMPLED);

            state.lighting_hdr = ctx.create_image(
                "lighting_hdr",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            ctx.write_image(state.lighting_hdr, image_usage::COLOR_ATTACHMENT);

            state.tmp_pong = ctx.create_image(
                "tmp_pong",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 256, .height = 256, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            ctx.write_image(state.tmp_pong, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 3: tonemap reads HDR, writes LDR.
        void tonemap_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.lighting_hdr, image_usage::SAMPLED);

            state.tonemap_ldr = ctx.create_image(
                "tonemap_ldr",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            ctx.write_image(state.tonemap_ldr, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 4: present reads LDR and writes imported swapchain, declares output.
        void present_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.tonemap_ldr, image_usage::SAMPLED);

            state.swapchain = ctx.create_image(
                "swapchain",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 1280, .height = 720, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                true);
            ctx.write_image(state.swapchain, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.swapchain);
        }

        struct pass_range
        {
            uint32_t begin = 0;
            uint32_t end   = 0;
        };

        pass_range range_for(const per_pass_barrier& plan, pass_handle pass)
        {
            RG_CHECK(pass + 1 < plan.pass_begins.size());
            const uint32_t base = plan.pass_begins[pass];
            const uint32_t len  = plan.pass_lengths[pass];
            return pass_range{.begin = base, .end = base + len};
        }

        bool has_barrier(const per_pass_barrier& plan,
                         pass_handle pass,
                         barrier_op_type type,
                         resource_kind kind,
                         resource_handle logical)
        {
            const auto pass_range = range_for(plan, pass);
            for (uint32_t i = pass_range.begin; i < pass_range.end; i++)
            {
                if (plan.types[i] == type && plan.kinds[i] == kind && plan.logicals[i] == logical)
                {
                    return true;
                }
            }
            return false;
        }

        uint32_t count_barriers(const per_pass_barrier& plan,
                                pass_handle pass,
                                barrier_op_type type,
                                resource_kind kind)
        {
            uint32_t count = 0;
            const auto pass_range = range_for(plan, pass);
            for (uint32_t i = pass_range.begin; i < pass_range.end; i++)
            {
                if (plan.types[i] == type && plan.kinds[i] == kind)
                {
                    count++;
                }
            }
            return count;
        }
    } // namespace

    void barrier_plan_test()
    {
        auto& state = test_state();
        state.reset();

        system_t system;
        system.add_pass(compute_setup, noop_execute);  // 0
        system.add_pass(gbuffer_setup, noop_execute);  // 1
        system.add_pass(lighting_setup, noop_execute); // 2
        system.add_pass(tonemap_setup, noop_execute);  // 3
        system.add_pass(present_setup, noop_execute);  // 4

        system.compile();

        // Sanity: pass order is a strict chain.
        const auto& sorted = system.get_sorted_passes();
        RG_CHECK(sorted.size() == 5);
        RG_CHECK(sorted[0] == 0);
        RG_CHECK(sorted[1] == 1);
        RG_CHECK(sorted[2] == 2);
        RG_CHECK(sorted[3] == 3);
        RG_CHECK(sorted[4] == 4);

        // Barrier plan shapes.
        const auto& plan = system.get_per_pass_barriers();
        RG_CHECK(plan.pass_begins.size() == 6);
        RG_CHECK(plan.pass_lengths.size() == 5);

        // 1) Compute buffer: write(STORAGE) -> read(STORAGE) should trigger a UAV-like barrier on consumer pass.
        RG_CHECK(has_barrier(plan, /*pass=*/1, barrier_op_type::uav, resource_kind::buffer, state.buf_hist));

        // 2) GBuffer images: write(COLOR/DEPTH) -> read(SAMPLED) should trigger transitions in lighting pass.
        RG_CHECK(has_barrier(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image, state.g_albedo));
        RG_CHECK(has_barrier(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image, state.g_normal));
        RG_CHECK(has_barrier(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image, state.g_depth));

        // 3) HDR -> tonemap: write(COLOR) -> read(SAMPLED) should trigger a transition in tonemap pass.
        RG_CHECK(has_barrier(plan, /*pass=*/3, barrier_op_type::transition, resource_kind::image, state.lighting_hdr));

        // 4) LDR -> present: write(COLOR) -> read(SAMPLED) should trigger a transition in present pass.
        RG_CHECK(has_barrier(plan, /*pass=*/4, barrier_op_type::transition, resource_kind::image, state.tonemap_ldr));

        // 5) Aliasing: tmp_ping and tmp_pong should share the same physical image id.
        const auto ping_phys = system.get_physical_image_id(state.tmp_ping);
        const auto pong_phys = system.get_physical_image_id(state.tmp_pong);
        RG_CHECK(ping_phys != invalid_resource);
        RG_CHECK(pong_phys != invalid_resource);
        RG_CHECK(ping_phys == pong_phys);

        // When a physical id is reused by a different logical resource, we expect an aliasing barrier at first use of the new logical.
        RG_CHECK(has_barrier(plan, /*pass=*/2, barrier_op_type::aliasing, resource_kind::image, state.tmp_pong));

        // Optional: lighting pass should have at least 3 image transitions (gbuffer set) and may have more.
        RG_CHECK(count_barriers(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image) >= 3);

        (void)system;
    }
} // namespace render_graph::unit_test
