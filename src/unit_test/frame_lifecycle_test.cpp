#include "render_graph/unit_test/frame_lifecycle_test.h"

#include "render_graph/system.h"
#include "render_graph/unit_test/system_test_access.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    void frame_lifecycle_test()
    {
        using system_t = render_graph_system<test_backend>;
        using setup_context = system_t::pass_setup_context;
        using execute_context = system_t::pass_execute_context;

        system_t rg;
        image_handle history_read{};
        image_handle history_write{};
        const test_image_desc history_desc{
            .fmt = format::R8G8B8A8_UNORM,
            .extent = {.width = 64, .height = 64, .depth = 1},
            .usage = image_usage::SAMPLED | image_usage::COLOR_ATTACHMENT,
        };
        rg.add_raster_pass("History", [&](setup_context& ctx)
        {
            history_read = ctx.import_image("HistoryRead", history_desc, resource_lifetime_class::history);
            history_write = ctx.import_image("HistoryWrite", history_desc, resource_lifetime_class::history);
            const image_access_desc sampled{.usage = image_usage::SAMPLED, .domain = pipeline_domain::graphics};
            ctx.set_initial_state(history_read, sampled, access_type::read, contents_policy::preserve);
            ctx.set_final_state(history_read, sampled, access_type::read);
            ctx.read_image(history_read, sampled);
            ctx.set_initial_state(history_write, sampled, access_type::read, contents_policy::preserve);
            ctx.set_final_state(history_write, sampled, access_type::read);
            ctx.add_color_attachment(history_write, attachment_load_op::dont_care, attachment_store_op::store);
            ctx.declare_image_output(history_write);
        }, [](execute_context& ctx) { ctx.commands().record_user_command(); });

        for (uint64_t frame = 0; frame < 3; frame++)
        {
            rg.begin_frame(frame, frame == 0 ? 0 : frame - 1, 77);
            if (frame == 0)
            {
                RG_CHECK(rg.needs_recompile());
                RG_CHECK(rg.compile().succeeded());
                RG_CHECK(system_test_access::image_is_imported(rg, history_read));
                RG_CHECK(system_test_access::image_lifetime_class(rg, history_read) == resource_lifetime_class::history);
            }
            else
            {
                RG_CHECK(!rg.needs_recompile());
            }

            const uintptr_t first = frame % 2 == 0 ? 101 : 102;
            const uintptr_t second = frame % 2 == 0 ? 102 : 101;
            rg.bind_imported_image(history_read, first);
            rg.bind_imported_image(history_write, second);
            test_command_context commands;
            RG_CHECK(rg.execute(commands).succeeded());
            RG_CHECK(rg.has_active_frame());
            RG_CHECK(rg.commit_frame());
            RG_CHECK(!rg.has_active_frame());
        }
        RG_CHECK(rg.get_backend_context().commit_count == 3);
        RG_CHECK(rg.get_backend_context().begun_frames.size() == 3);

        rg.begin_frame(3, 2, 78);
        RG_CHECK(rg.needs_recompile());
        rg.abort_frame();
        RG_CHECK(rg.get_backend_context().abort_count == 1);

        rg.begin_frame(4, 3, 77);
        RG_CHECK(!rg.needs_recompile());
        test_command_context failing_commands;
        failing_commands.fail_next_barrier = true;
        RG_CHECK(!rg.execute(failing_commands).succeeded());
        RG_CHECK(!rg.has_active_frame());
        RG_CHECK(rg.get_backend_context().abort_count == 2);

        rg.begin_frame(5, 4, 77);
        test_command_context recovered_commands;
        RG_CHECK(rg.execute(recovered_commands).succeeded());
        RG_CHECK(rg.commit_frame());

        rg.clear();
        RG_CHECK(system_test_access::image_count(rg) == 0);
        rg.begin_frame(6, 5, 77);
        test_command_context after_clear;
        RG_CHECK(!rg.execute(after_clear).succeeded());
        RG_CHECK(!rg.has_active_frame());
    }
}
