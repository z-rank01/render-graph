#include "render_graph/unit_test/barrier_plan_test.h"

#include <cassert>
#include <cstdint>

#include "render_graph/system.h"

namespace render_graph::unit_test
{
    namespace
    {
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
            auto& s = test_state();

            s.buf_hist = ctx.create_buffer(buffer_info{
                .name     = "histogram",
                .size     = 1024,
                .usage    = buffer_usage::STORAGE_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(s.buf_hist, buffer_usage::STORAGE_BUFFER);

            s.tmp_ping = ctx.create_image(image_info{
                .name          = "tmp_ping",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 256, .height = 256, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(s.tmp_ping, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 1: GBuffer writes albedo/normal/depth, and reads the compute buffer.
        void gbuffer_setup(pass_setup_context& ctx)
        {
            auto& s = test_state();

            ctx.read_buffer(s.buf_hist, buffer_usage::STORAGE_BUFFER);

            s.g_albedo = ctx.create_image(image_info{
                .name          = "gbuffer_albedo",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            s.g_normal = ctx.create_image(image_info{
                .name          = "gbuffer_normal",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            s.g_depth = ctx.create_image(image_info{
                .name          = "gbuffer_depth",
                .fmt           = format::D32_SFLOAT,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::DEPTH_STENCIL_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });

            ctx.write_image(s.g_albedo, image_usage::COLOR_ATTACHMENT);
            ctx.write_image(s.g_normal, image_usage::COLOR_ATTACHMENT);
            ctx.write_image(s.g_depth, image_usage::DEPTH_STENCIL_ATTACHMENT);
        }

        // Pass 2: lighting reads GBuffer as sampled, writes HDR, and creates another transient color image.
        // tmp_pong is descriptor-compatible with tmp_ping and its lifetime doesn't overlap -> should alias.
        void lighting_setup(pass_setup_context& ctx)
        {
            auto& s = test_state();

            ctx.read_image(s.g_albedo, image_usage::SAMPLED);
            ctx.read_image(s.g_normal, image_usage::SAMPLED);
            ctx.read_image(s.g_depth, image_usage::SAMPLED);

            s.lighting_hdr = ctx.create_image(image_info{
                .name          = "lighting_hdr",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(s.lighting_hdr, image_usage::COLOR_ATTACHMENT);

            s.tmp_pong = ctx.create_image(image_info{
                .name          = "tmp_pong",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 256, .height = 256, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(s.tmp_pong, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 3: tonemap reads HDR, writes LDR.
        void tonemap_setup(pass_setup_context& ctx)
        {
            auto& s = test_state();

            ctx.read_image(s.lighting_hdr, image_usage::SAMPLED);

            s.tonemap_ldr = ctx.create_image(image_info{
                .name          = "tonemap_ldr",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(s.tonemap_ldr, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 4: present reads LDR and writes imported swapchain, declares output.
        void present_setup(pass_setup_context& ctx)
        {
            auto& s = test_state();

            ctx.read_image(s.tonemap_ldr, image_usage::SAMPLED);

            s.swapchain = ctx.create_image(image_info{
                .name          = "swapchain",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 1280, .height = 720, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });
            ctx.write_image(s.swapchain, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(s.swapchain);
        }

        struct pass_range
        {
            uint32_t begin = 0;
            uint32_t end   = 0;
        };

        pass_range range_for(const per_pass_barrier& plan, pass_handle pass)
        {
            assert(pass + 1 < plan.pass_begins.size());
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
            const auto r = range_for(plan, pass);
            for (uint32_t i = r.begin; i < r.end; i++)
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
            const auto r   = range_for(plan, pass);
            for (uint32_t i = r.begin; i < r.end; i++)
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
        auto& s = test_state();
        s.reset();

        render_graph_system system;
        system.add_pass(compute_setup, noop_execute);  // 0
        system.add_pass(gbuffer_setup, noop_execute);  // 1
        system.add_pass(lighting_setup, noop_execute); // 2
        system.add_pass(tonemap_setup, noop_execute);  // 3
        system.add_pass(present_setup, noop_execute);  // 4

        system.compile();

        // Sanity: pass order is a strict chain.
        assert(system.sorted_passes.size() == 5);
        assert(system.sorted_passes[0] == 0);
        assert(system.sorted_passes[1] == 1);
        assert(system.sorted_passes[2] == 2);
        assert(system.sorted_passes[3] == 3);
        assert(system.sorted_passes[4] == 4);

        // Barrier plan shapes.
        const auto& plan = system.per_pass_barriers;
        assert(plan.pass_begins.size() == 6);
        assert(plan.pass_lengths.size() == 5);

        // 1) Compute buffer: write(STORAGE) -> read(STORAGE) should trigger a UAV-like barrier on consumer pass.
        assert(has_barrier(plan, /*pass=*/1, barrier_op_type::uav, resource_kind::buffer, s.buf_hist));

        // 2) GBuffer images: write(COLOR/DEPTH) -> read(SAMPLED) should trigger transitions in lighting pass.
        assert(has_barrier(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image, s.g_albedo));
        assert(has_barrier(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image, s.g_normal));
        assert(has_barrier(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image, s.g_depth));

        // 3) HDR -> tonemap: write(COLOR) -> read(SAMPLED) should trigger a transition in tonemap pass.
        assert(has_barrier(plan, /*pass=*/3, barrier_op_type::transition, resource_kind::image, s.lighting_hdr));

        // 4) LDR -> present: write(COLOR) -> read(SAMPLED) should trigger a transition in present pass.
        assert(has_barrier(plan, /*pass=*/4, barrier_op_type::transition, resource_kind::image, s.tonemap_ldr));

        // 5) Aliasing: tmp_ping and tmp_pong should share the same physical image id.
        assert(s.tmp_ping < system.physical_resource_metas.handle_to_physical_img_id.size());
        assert(s.tmp_pong < system.physical_resource_metas.handle_to_physical_img_id.size());
        const auto ping_phys = static_cast<resource_handle>(system.physical_resource_metas.handle_to_physical_img_id[s.tmp_ping]);
        const auto pong_phys = static_cast<resource_handle>(system.physical_resource_metas.handle_to_physical_img_id[s.tmp_pong]);
        assert(ping_phys == pong_phys);

        // When a physical id is reused by a different logical resource, we expect an aliasing barrier at first use of the new logical.
        assert(has_barrier(plan, /*pass=*/2, barrier_op_type::aliasing, resource_kind::image, s.tmp_pong));

        // Optional: lighting pass should have at least 3 image transitions (gbuffer set) and may have more.
        assert(count_barriers(plan, /*pass=*/2, barrier_op_type::transition, resource_kind::image) >= 3);

        (void)system;
    }
} // namespace render_graph::unit_test
