#include "render_graph/unit_test/execute_context_test.h"

#include <algorithm>
#include <span>

#include "render_graph/system.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        using system_t = render_graph_system<test_backend>;
        using pass_setup_context = system_t::pass_setup_context;
        using pass_execute_context = system_t::pass_execute_context;

        test_image_desc image_desc(image_usage usage)
        {
            return test_image_desc{
                .fmt = format::R8G8B8A8_UNORM,
                .extent = {.width = 32, .height = 32, .depth = 1},
                .usage = usage,
            };
        }

        bool has_error(const execute_result& result, execute_error_code code)
        {
            return std::ranges::any_of(result.diagnostics, [&](const execute_diagnostic& diagnostic) { return diagnostic.code == code; });
        }

        template <typename ExecuteFn>
        void build_internal_graph(system_t& rg, image_handle& image, ExecuteFn&& execute)
        {
            rg.add_pass(
                "internal transition",
                [&](pass_setup_context& ctx)
                {
                    image = ctx.create_image("Tracked",
                                             image_desc(image_usage::COLOR_ATTACHMENT | image_usage::SAMPLED | image_usage::PRESENT),
                                             resource_lifetime_class::imported);
                    const image_access_desc present{.usage = image_usage::PRESENT, .domain = pipeline_domain::graphics};
                    ctx.set_initial_state(image, present, access_type::read, contents_policy::preserve);
                    ctx.write_image(image,
                                    image_access_desc{
                                        .usage = image_usage::COLOR_ATTACHMENT,
                                        .domain = pipeline_domain::graphics,
                                    });
                    ctx.read_image(image,
                                   image_access_desc{
                                       .usage = image_usage::SAMPLED,
                                       .domain = pipeline_domain::graphics,
                                   });
                    ctx.set_final_state(image, present);
                    ctx.declare_image_output(image);
                },
                std::forward<ExecuteFn>(execute));
            RG_CHECK(rg.compile().succeeded());
        }

        void execution_order_test()
        {
            system_t rg;
            image_handle image{};
            build_internal_graph(
                rg,
                image,
                [](pass_execute_context& ctx)
                {
                    ctx.commands().record_user_command();
                    const auto pending = ctx.pending_explicit_transitions();
                    RG_CHECK(pending.size() == 1);
                    ctx.explicit_barrier(pending);
                    ctx.commands().record_user_command();
                });

            test_command_context commands;
            const auto result = rg.execute(commands);
            RG_CHECK(result.succeeded());
            RG_CHECK(commands.records.size() == 5);
            RG_CHECK(commands.records[0].kind == test_command_kind::barrier_batch);
            RG_CHECK(commands.records[0].scope == synchronization_scope::pass_prologue);
            RG_CHECK(commands.records[1].kind == test_command_kind::user_command);
            RG_CHECK(commands.records[2].scope == synchronization_scope::pass_internal);
            RG_CHECK(commands.records[3].kind == test_command_kind::user_command);
            RG_CHECK(commands.records[4].scope == synchronization_scope::graph_epilogue);
        }

        void multi_resource_batch_test()
        {
            system_t rg;
            image_handle first{};
            image_handle second{};
            rg.add_pass(
                "multi resource internal batch",
                [&](pass_setup_context& ctx)
                {
                    first = ctx.create_image("First", image_desc(image_usage::STORAGE));
                    second = ctx.create_image("Second", image_desc(image_usage::STORAGE));
                    const image_access_desc storage{
                        .usage = image_usage::STORAGE,
                        .domain = pipeline_domain::compute,
                        .queue = queue_class::compute,
                    };
                    ctx.write_image(first, storage);
                    ctx.write_image(second, storage);
                    ctx.read_image(first, storage);
                    ctx.read_image(second, storage);
                    ctx.declare_image_output(first);
                    ctx.declare_image_output(second);
                },
                [](pass_execute_context& ctx)
                {
                    const auto pending = ctx.pending_explicit_transitions();
                    RG_CHECK(pending.size() == 2);
                    ctx.explicit_barrier(pending);
                });
            RG_CHECK(rg.compile().succeeded());

            test_command_context commands;
            RG_CHECK(rg.execute(commands).succeeded());
            const auto internal = std::ranges::find_if(
                commands.records,
                [](const test_command_record& record) { return record.scope == synchronization_scope::pass_internal; });
            RG_CHECK(internal != commands.records.end());
            RG_CHECK(internal->resources == std::vector<resource_handle>({first, second}));
        }

        void validation_errors_test()
        {
            {
                system_t rg;
                test_command_context commands;
                RG_CHECK(has_error(rg.execute(commands), execute_error_code::graph_not_compiled));
            }
            {
                system_t rg;
                image_handle image{};
                build_internal_graph(rg, image, [](pass_execute_context&) {});
                test_command_context commands;
                RG_CHECK(has_error(rg.execute(commands), execute_error_code::missing_explicit_barrier));
            }
            {
                system_t rg;
                image_handle image{};
                build_internal_graph(
                    rg,
                    image,
                    [](pass_execute_context& ctx)
                    {
                        auto wrong = ctx.pending_explicit_transitions().front();
                        wrong.after.usage_bits = static_cast<uint32_t>(image_usage::TRANSFER_SRC);
                        ctx.explicit_barrier(std::span<const explicit_transition>(&wrong, 1));
                    });
                test_command_context commands;
                RG_CHECK(has_error(rg.execute(commands), execute_error_code::explicit_barrier_out_of_order));
            }
            {
                system_t rg;
                image_handle image{};
                build_internal_graph(
                    rg,
                    image,
                    [](pass_execute_context& ctx)
                    {
                        auto wrong = ctx.pending_explicit_transitions().front();
                        wrong.logical += 1;
                        ctx.explicit_barrier(std::span<const explicit_transition>(&wrong, 1));
                    });
                test_command_context commands;
                RG_CHECK(has_error(rg.execute(commands), execute_error_code::explicit_barrier_out_of_order));
            }
            {
                system_t rg;
                image_handle image{};
                build_internal_graph(
                    rg,
                    image,
                    [](pass_execute_context& ctx)
                    {
                        const auto transition = ctx.pending_explicit_transitions().front();
                        ctx.explicit_barrier(std::span<const explicit_transition>(&transition, 1));
                        ctx.explicit_barrier(std::span<const explicit_transition>(&transition, 1));
                    });
                test_command_context commands;
                RG_CHECK(has_error(rg.execute(commands), execute_error_code::unexpected_explicit_barrier));
            }
            {
                system_t rg;
                image_handle first{};
                image_handle second{};
                rg.add_pass(
                    "out of order",
                    [&](pass_setup_context& ctx)
                    {
                        first = ctx.create_image("First", image_desc(image_usage::STORAGE));
                        second = ctx.create_image("Second", image_desc(image_usage::STORAGE));
                        ctx.write_image(first, image_usage::STORAGE);
                        ctx.write_image(second, image_usage::STORAGE);
                        ctx.read_image(first, image_usage::STORAGE);
                        ctx.read_image(second, image_usage::STORAGE);
                        ctx.declare_image_output(first);
                        ctx.declare_image_output(second);
                    },
                    [](pass_execute_context& ctx)
                    {
                        const auto pending = ctx.pending_explicit_transitions();
                        ctx.explicit_barrier(pending.subspan(1, 1));
                    });
                RG_CHECK(rg.compile().succeeded());
                test_command_context commands;
                RG_CHECK(has_error(rg.execute(commands), execute_error_code::explicit_barrier_out_of_order));
            }
        }

        void backend_failure_and_zero_internal_test()
        {
            system_t rg;
            image_handle output{};
            rg.add_pass(
                "ordinary pass",
                [&](pass_setup_context& ctx)
                {
                    output = ctx.create_image("Output", image_desc(image_usage::COLOR_ATTACHMENT));
                    ctx.write_image(output, image_usage::COLOR_ATTACHMENT);
                    ctx.declare_image_output(output);
                },
                [](pass_execute_context& ctx)
                {
                    RG_CHECK(ctx.pending_explicit_transitions().empty());
                    ctx.commands().record_user_command();
                });
            RG_CHECK(rg.compile().succeeded());

            test_command_context commands;
            RG_CHECK(rg.execute(commands).succeeded());
            RG_CHECK(std::ranges::none_of(
                commands.records,
                [](const test_command_record& record) { return record.scope == synchronization_scope::pass_internal; }));

            test_command_context failing_commands;
            failing_commands.fail_next_barrier = true;
            RG_CHECK(has_error(rg.execute(failing_commands), execute_error_code::backend_failure));
        }
    }

    void execute_context_test()
    {
        execution_order_test();
        multi_resource_batch_test();
        validation_errors_test();
        backend_failure_and_zero_internal_test();
    }
}
