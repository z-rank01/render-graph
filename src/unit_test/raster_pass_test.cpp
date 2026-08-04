#include "render_graph/unit_test/raster_pass_test.h"

#include <algorithm>

#include "render_graph/system.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        using system_t = render_graph_system<test_backend>;
        using setup_context = system_t::pass_setup_context;
        using execute_context = system_t::pass_execute_context;

        test_image_desc image_desc(format value, uint32_t width = 64, uint32_t samples = 1)
        {
            return test_image_desc{
                .fmt = value,
                .extent = {.width = width, .height = 64, .depth = 1},
                .usage = value == format::D32_SFLOAT ? image_usage::DEPTH_STENCIL_ATTACHMENT : image_usage::COLOR_ATTACHMENT,
                .sample_counts = samples,
            };
        }

        bool has_compile_error(const compile_result& result, compile_error_code code)
        {
            return std::ranges::any_of(result.diagnostics, [&](const compile_diagnostic& diagnostic) { return diagnostic.code == code; });
        }

        void color_depth_execution_test()
        {
            system_t rg;
            image_handle color{};
            image_handle depth{};
            rg.add_raster_pass("color depth", [&](setup_context& ctx)
            {
                color = ctx.create_image("Color", image_desc(format::R8G8B8A8_UNORM));
                depth = ctx.create_image("Depth", image_desc(format::D32_SFLOAT));
                ctx.set_render_area({.width = 64, .height = 64});
                ctx.add_color_attachment(color, attachment_load_op::clear, attachment_store_op::store,
                                         clear_value{.color = {0.1F, 0.2F, 0.3F, 1.0F}});
                ctx.set_depth_stencil_attachment(depth, attachment_load_op::clear, attachment_store_op::store,
                                                 clear_value{.depth = 0.0F});
                ctx.declare_image_output(color);
            }, [](execute_context& ctx) { ctx.commands().record_user_command(); });
            RG_CHECK(rg.compile().succeeded());
            test_command_context commands;
            RG_CHECK(rg.execute(commands).succeeded());
            const auto begin = std::ranges::find_if(commands.records, [](const auto& record)
            {
                return record.kind == test_command_kind::begin_rendering;
            });
            const auto user = std::ranges::find_if(commands.records, [](const auto& record)
            {
                return record.kind == test_command_kind::user_command;
            });
            const auto end = std::ranges::find_if(commands.records, [](const auto& record)
            {
                return record.kind == test_command_kind::end_rendering;
            });
            RG_CHECK(begin < user && user < end);
        }

        void mrt_depth_only_and_resolve_test()
        {
            {
                system_t rg;
                image_handle a{};
                image_handle b{};
                rg.add_raster_pass("mrt", [&](setup_context& ctx)
                {
                    a = ctx.create_image("A", image_desc(format::R8G8B8A8_UNORM));
                    b = ctx.create_image("B", image_desc(format::R8G8B8A8_UNORM));
                    ctx.add_color_attachment(a, attachment_load_op::clear, attachment_store_op::store);
                    ctx.add_color_attachment(b, attachment_load_op::clear, attachment_store_op::store);
                    ctx.declare_image_output(a);
                    ctx.declare_image_output(b);
                }, [](execute_context&) {});
                RG_CHECK(rg.compile().succeeded());
            }
            {
                system_t rg;
                image_handle depth{};
                rg.add_raster_pass("depth only", [&](setup_context& ctx)
                {
                    depth = ctx.create_image("Depth", image_desc(format::D32_SFLOAT));
                    ctx.set_depth_stencil_attachment(depth, attachment_load_op::clear, attachment_store_op::store);
                    ctx.declare_image_output(depth);
                }, [](execute_context&) {});
                RG_CHECK(rg.compile().succeeded());
            }
            {
                system_t rg;
                image_handle multisampled{};
                image_handle resolved{};
                rg.add_raster_pass("resolve", [&](setup_context& ctx)
                {
                    multisampled = ctx.create_image("MSAA", image_desc(format::R8G8B8A8_UNORM, 64, 4));
                    resolved = ctx.create_image("Resolved", image_desc(format::R8G8B8A8_UNORM));
                    ctx.add_color_attachment(multisampled,
                                             attachment_load_op::clear,
                                             attachment_store_op::dont_care,
                                             {},
                                             {},
                                             resolved);
                    ctx.declare_image_output(resolved);
                }, [](execute_context&) {});
                RG_CHECK(rg.compile().succeeded());
            }
        }

        void validation_test()
        {
            {
                system_t rg;
                image_handle transient{};
                rg.add_raster_pass("invalid load", [&](setup_context& ctx)
                {
                    transient = ctx.create_image("Transient", image_desc(format::R8G8B8A8_UNORM));
                    ctx.add_color_attachment(transient, attachment_load_op::load, attachment_store_op::store);
                    ctx.declare_image_output(transient);
                }, [](execute_context&) {});
                RG_CHECK(has_compile_error(rg.compile(), compile_error_code::image_read_before_write));
            }
            {
                system_t rg;
                image_handle imported{};
                rg.add_raster_pass("valid load", [&](setup_context& ctx)
                {
                    imported = ctx.create_image("Imported", image_desc(format::R8G8B8A8_UNORM), true);
                    ctx.add_color_attachment(imported, attachment_load_op::load, attachment_store_op::store);
                    ctx.declare_image_output(imported);
                }, [](execute_context&) {});
                RG_CHECK(rg.compile().succeeded());
            }
            {
                system_t rg;
                image_handle a{};
                image_handle b{};
                rg.add_raster_pass("mismatch", [&](setup_context& ctx)
                {
                    a = ctx.create_image("A", image_desc(format::R8G8B8A8_UNORM, 64));
                    b = ctx.create_image("B", image_desc(format::R8G8B8A8_UNORM, 32));
                    ctx.add_color_attachment(a, attachment_load_op::clear, attachment_store_op::store);
                    ctx.add_color_attachment(b, attachment_load_op::clear, attachment_store_op::store);
                    ctx.declare_image_output(a);
                }, [](execute_context&) {});
                RG_CHECK(has_compile_error(rg.compile(), compile_error_code::raster_attachment_mismatch));
            }
        }

        void non_raster_scope_test()
        {
            system_t rg;
            image_handle output{};
            rg.add_copy_pass("copy", [&](setup_context& ctx)
            {
                output = ctx.create_image("Output", image_desc(format::R8G8B8A8_UNORM));
                ctx.write_image(output, image_usage::TRANSFER_DST);
                ctx.declare_image_output(output);
            }, [](execute_context& ctx) { ctx.commands().record_user_command(); });
            RG_CHECK(rg.compile().succeeded());
            test_command_context commands;
            RG_CHECK(rg.execute(commands).succeeded());
            RG_CHECK(std::ranges::none_of(commands.records, [](const auto& record)
            {
                return record.kind == test_command_kind::begin_rendering || record.kind == test_command_kind::end_rendering;
            }));
        }
    }

    void raster_pass_test()
    {
        color_depth_execution_test();
        mrt_depth_only_and_resolve_test();
        validation_test();
        non_raster_scope_test();
    }
}
