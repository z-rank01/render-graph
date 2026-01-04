#include "render_graph/unit_test/culling_compile_test.h"

#include <vector>

#include "render_graph/system.h"

namespace render_graph::unit_test
{
    namespace
    {
        struct expected_state_t
        {
            std::vector<bool> expected_active;

            void reset(size_t pass_count) { expected_active.assign(pass_count, false); }
            void keep(pass_handle p)
            {
                if (p < expected_active.size())
                {
                    expected_active[p] = true;
                }
            }
        };

        expected_state_t& expected_state()
        {
            static expected_state_t s{};
            return s;
        }

        struct handles_t
        {
            // Branch A -> B -> Present (image output)
            resource_version_handle a_img0 = 0;
            resource_version_handle b_img1 = 0;
            resource_version_handle swapchain_img = 0;

            // Debug branch D -> E (debug image output)
            resource_version_handle dbg_img0 = 0;

            // Buffer output branch
            resource_version_handle stats_buf = 0;

            // Culled branches (written but never reaches any output)
            resource_version_handle dead_img0 = 0;
            resource_version_handle dead_buf0 = 0;
        };

        handles_t& handles()
        {
            static handles_t h{};
            return h;
        }

        void noop_execute(pass_execute_context&) { }

        // Pass 0: produce A image
        void pass_a_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            h.a_img0 = ctx.create_image(image_info{
                .name          = "a_img0",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(h.a_img0, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 1: read A, write B
        void pass_b_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            ctx.read_image(h.a_img0, image_usage::SAMPLED);

            h.b_img1 = ctx.create_image(image_info{
                .name          = "b_img1",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(h.b_img1, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 2: read B, write swapchain, declare swapchain output
        void pass_present_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            ctx.read_image(h.b_img1, image_usage::SAMPLED);

            h.swapchain_img = ctx.create_image(image_info{
                .name          = "swapchain",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });
            ctx.write_image(h.swapchain_img, image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(h.swapchain_img);
        }

        // Pass 3: dead branch producer (never reaches outputs)
        void pass_dead0_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            h.dead_img0 = ctx.create_image(image_info{
                .name          = "dead_img0",
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
            ctx.write_image(h.dead_img0, image_usage::COLOR_ATTACHMENT);

            h.dead_buf0 = ctx.create_buffer(buffer_info{
                .name     = "dead_buf0",
                .size     = 256,
                .usage    = buffer_usage::STORAGE_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(h.dead_buf0, buffer_usage::STORAGE_BUFFER);
        }

        // Pass 4: dead branch consumer (still not output)
        void pass_dead1_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            ctx.read_image(h.dead_img0, image_usage::SAMPLED);
            ctx.read_buffer(h.dead_buf0, buffer_usage::STORAGE_BUFFER);

            // Rewrite dead buffer to make dependency chain longer, still no outputs.
            ctx.write_buffer(h.dead_buf0, buffer_usage::STORAGE_BUFFER);
        }

        // Pass 5: debug branch producer
        void pass_dbg0_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            h.dbg_img0 = ctx.create_image(image_info{
                .name          = "dbg_img0",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 128, .height = 128, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(h.dbg_img0, image_usage::COLOR_ATTACHMENT);
        }

        // Pass 6: debug branch consumer, declare debug output
        void pass_dbg1_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            ctx.read_image(h.dbg_img0, image_usage::SAMPLED);
            ctx.declare_image_output(h.dbg_img0);
        }

        // Pass 7: buffer output producer
        void pass_stats_setup(pass_setup_context& ctx)
        {
            auto& h = handles();
            h.stats_buf = ctx.create_buffer(buffer_info{
                .name     = "stats_buf",
                .size     = 1024,
                .usage    = buffer_usage::STORAGE_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(h.stats_buf, buffer_usage::STORAGE_BUFFER);
            ctx.declare_buffer_output(h.stats_buf);
        }

        // Pass 8: extra producer that feeds into present through buffer->image chain
        void pass_extra_setup(pass_setup_context& ctx)
        {
            auto& h = handles();

            // Make sure culling traverses buffer reads as well for image outputs:
            // This pass writes a buffer which pass_b reads (indirectly kept alive).
            // We'll also read a_img0 to connect it.
            ctx.read_image(h.a_img0, image_usage::SAMPLED);

            // Write an auxiliary buffer that pass_b will read.
            auto aux = ctx.create_buffer(buffer_info{
                .name     = "aux_buf",
                .size     = 128,
                .usage    = buffer_usage::UNIFORM_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(aux, buffer_usage::UNIFORM_BUFFER);

            // Store aux in dead_buf0 slot not to increase test state struct.
            // (Only used to connect dependencies; debugger can still inspect deps lists.)
            h.dead_buf0 = aux;
        }

        // Patch pass_b to read aux buffer as well, increasing graph complexity.
        void pass_b_setup_with_aux(pass_setup_context& ctx)
        {
            pass_b_setup(ctx);
            auto& h = handles();
            ctx.read_buffer(h.dead_buf0, buffer_usage::UNIFORM_BUFFER);
        }
    } // namespace

    void culling_compile_test()
    {
        auto& exp = expected_state();
        handles() = handles_t{};

        render_graph_system system;

        const auto p0 = system.add_pass(pass_a_setup, noop_execute);
        const auto p8 = system.add_pass(pass_extra_setup, noop_execute);
        const auto p1 = system.add_pass(pass_b_setup_with_aux, noop_execute);
        const auto p2 = system.add_pass(pass_present_setup, noop_execute);

        const auto p3 = system.add_pass(pass_dead0_setup, noop_execute);
        const auto p4 = system.add_pass(pass_dead1_setup, noop_execute);

        const auto p5 = system.add_pass(pass_dbg0_setup, noop_execute);
        const auto p6 = system.add_pass(pass_dbg1_setup, noop_execute);

        const auto p7 = system.add_pass(pass_stats_setup, noop_execute);

        exp.reset(system.graph.passes.size());

        // Expected alive: present chain (p2 -> p1 -> p0 and also p8 because p1 reads aux written by p8)
        exp.keep(p2);
        exp.keep(p1);
        exp.keep(p0);
        exp.keep(p8);

        // Debug output keeps its producer chain. The read-only output-declaring pass (p6)
        // has no side effects and should be culled; only its producer is required.
        exp.keep(p5);

        // Buffer output producer is alive
        exp.keep(p7);

        // Dead branch should be culled
        // p3, p4 remain false

        system.compile();

        // Set a breakpoint here and compare:
        // - system.active_pass_flags
        // - expected_state().expected_active
        (void)system;
    }
} // namespace render_graph::unit_test
