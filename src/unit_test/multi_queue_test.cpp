#include "render_graph/unit_test/multi_queue_test.h"

#include <algorithm>
#include <array>
#include <memory>

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

        test_buffer_desc buffer_desc(buffer_usage usage = buffer_usage::STORAGE_BUFFER)
        {
            return test_buffer_desc{.size = 256, .usage = usage};
        }

        void graphics_compute_graphics_test()
        {
            system_t rg;
            rg.set_queue_availability({.compute = true, .copy = true});
            buffer_handle first{};
            buffer_handle second{};
            buffer_handle output{};
            rg.add_pass("graphics producer", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::graphics);
                first = ctx.create_buffer("First", buffer_desc());
                ctx.write_buffer(first, buffer_usage::STORAGE_BUFFER);
            }, [](execute_context& ctx) { ctx.commands().record_user_command(); });
            rg.add_pass("compute", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::compute);
                ctx.read_buffer(first, buffer_usage::STORAGE_BUFFER);
                second = ctx.create_buffer("Second", buffer_desc());
                ctx.write_buffer(second, buffer_usage::STORAGE_BUFFER);
            }, [](execute_context& ctx) { ctx.commands().record_user_command(); });
            rg.add_pass("graphics consumer", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::graphics);
                ctx.read_buffer(second, buffer_usage::STORAGE_BUFFER);
                output = ctx.create_buffer("Output", buffer_desc());
                ctx.write_buffer(output, buffer_usage::STORAGE_BUFFER);
                ctx.declare_buffer_output(output);
            }, [](execute_context& ctx) { ctx.commands().record_user_command(); });

            RG_CHECK(rg.compile().succeeded());
            const auto& plan = rg.get_submission_plan();
            RG_CHECK(plan.batches.size() == 3);
            RG_CHECK(plan.batches[0].queue == queue_class::graphics);
            RG_CHECK(plan.batches[1].queue == queue_class::compute);
            RG_CHECK(plan.batches[2].queue == queue_class::graphics);
            RG_CHECK(plan.batches[0].signal_value == 1 && plan.batches[1].signal_value == 2 && plan.batches[2].signal_value == 3);
            RG_CHECK(plan.batches[1].waits.size() == 1 && plan.batches[1].waits[0].value == 1);
            RG_CHECK(plan.batches[2].waits.size() == 1 && plan.batches[2].waits[0].value == 2);
            RG_CHECK(!plan.batches[0].release_barriers.empty());
            RG_CHECK(!plan.batches[1].acquire_barriers.empty());
            RG_CHECK(plan.batches[0].release_barriers.front().phase == synchronization_phase::release);
            RG_CHECK(plan.batches[1].acquire_barriers.front().phase == synchronization_phase::acquire);
            for (const auto& batch : plan.batches)
            {
                for (const auto& wait : batch.waits)
                {
                    RG_CHECK(wait.source_batch < batch.handle);
                    RG_CHECK(wait.value == plan.batches[wait.source_batch].signal_value);
                }
            }

            std::array<test_command_context, 3> commands;
            RG_CHECK(rg.execute_batches(commands).succeeded());
            for (const auto& command : commands)
            {
                RG_CHECK(std::ranges::any_of(command.records, [](const auto& record)
                {
                    return record.kind == test_command_kind::user_command;
                }));
            }
        }

        void copy_to_graphics_and_fallback_test()
        {
            auto build = [](system_t& rg, bool independent_queues)
            {
                rg.set_queue_availability({.compute = independent_queues, .copy = independent_queues});
                auto shared = std::make_shared<buffer_handle>();
                auto output = std::make_shared<buffer_handle>();
                rg.add_copy_pass("copy", [shared](setup_context& ctx)
                {
                    ctx.set_queue(queue_class::copy);
                    *shared = ctx.create_buffer("Copied", buffer_desc(buffer_usage::TRANSFER_DST | buffer_usage::VERTEX_BUFFER));
                    ctx.write_buffer(*shared, buffer_usage::TRANSFER_DST);
                }, [](execute_context&) {});
                rg.add_pass("draw", [shared, output](setup_context& ctx)
                {
                    ctx.set_queue(queue_class::graphics);
                    ctx.read_buffer(*shared, buffer_usage::VERTEX_BUFFER);
                    *output = ctx.create_buffer("DrawOutput", buffer_desc());
                    ctx.write_buffer(*output, buffer_usage::STORAGE_BUFFER);
                    ctx.declare_buffer_output(*output);
                }, [](execute_context&) {});
            };

            system_t multi_queue;
            build(multi_queue, true);
            RG_CHECK(multi_queue.compile().succeeded());
            const auto& multi_plan = multi_queue.get_submission_plan();
            RG_CHECK(multi_plan.batches[0].queue == queue_class::copy);
            RG_CHECK(multi_plan.batches[1].queue == queue_class::graphics);
            RG_CHECK(multi_plan.batches[1].waits.size() == 1);

            system_t fallback;
            build(fallback, false);
            RG_CHECK(fallback.compile().succeeded());
            const auto& fallback_plan = fallback.get_submission_plan();
            RG_CHECK(std::ranges::all_of(fallback_plan.batches, [](const auto& batch) { return batch.queue == queue_class::graphics; }));
            RG_CHECK(std::ranges::all_of(fallback_plan.batches, [](const auto& batch) { return batch.waits.empty(); }));
            RG_CHECK(fallback_plan.cross_queue_dependencies.empty());
        }

        void read_only_and_multiple_consumer_test()
        {
            system_t rg;
            rg.set_queue_availability({.compute = true, .copy = true});
            buffer_handle imported{};
            buffer_handle producer{};
            buffer_handle compute_output{};
            buffer_handle copy_output{};
            rg.add_pass("read imported compute", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::compute);
                imported = ctx.import_buffer("Imported", buffer_desc(), resource_lifetime_class::persistent);
                ctx.set_initial_state(imported,
                                      buffer_access_desc{.usage = buffer_usage::STORAGE_BUFFER, .domain = pipeline_domain::graphics},
                                      access_type::read,
                                      contents_policy::preserve);
                ctx.read_buffer(imported, buffer_usage::STORAGE_BUFFER);
                producer = ctx.create_buffer("Producer", buffer_desc());
                ctx.write_buffer(producer, buffer_usage::STORAGE_BUFFER);
            }, [](execute_context&) {});
            rg.add_pass("graphics read imported", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::graphics);
                ctx.read_buffer(imported, buffer_usage::STORAGE_BUFFER);
                compute_output = ctx.create_buffer("GraphicsOutput", buffer_desc());
                ctx.write_buffer(compute_output, buffer_usage::STORAGE_BUFFER);
                ctx.declare_buffer_output(compute_output);
            }, [](execute_context&) {});
            rg.add_pass("compute consumer", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::compute);
                ctx.read_buffer(producer, buffer_usage::STORAGE_BUFFER);
                compute_output = ctx.create_buffer("ComputeOutput", buffer_desc());
                ctx.write_buffer(compute_output, buffer_usage::STORAGE_BUFFER);
                ctx.declare_buffer_output(compute_output);
            }, [](execute_context&) {});
            rg.add_copy_pass("copy consumer", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::copy);
                ctx.read_buffer(producer, buffer_usage::STORAGE_BUFFER);
                copy_output = ctx.create_buffer("CopyOutput", buffer_desc());
                ctx.write_buffer(copy_output, buffer_usage::STORAGE_BUFFER);
                ctx.declare_buffer_output(copy_output);
            }, [](execute_context&) {});

            RG_CHECK(rg.compile().succeeded());
            const auto& plan = rg.get_submission_plan();
            RG_CHECK(std::ranges::any_of(plan.cross_queue_dependencies, [](const auto& dependency)
            {
                return dependency.ownership_transfer && dependency.source_queue == queue_class::compute &&
                       dependency.destination_queue == queue_class::graphics;
            }));
            RG_CHECK(std::ranges::count_if(plan.cross_queue_dependencies, [](const auto& dependency)
            {
                return dependency.source_pass == pass_handle{0};
            }) >= 2);
        }

        void epilogue_queue_transfer_test()
        {
            system_t rg;
            rg.set_queue_availability({.compute = true, .copy = true});
            buffer_handle output{};
            rg.add_pass("compute final", [&](setup_context& ctx)
            {
                ctx.set_queue(queue_class::compute);
                output = ctx.create_buffer("Output", buffer_desc());
                ctx.write_buffer(output, buffer_usage::STORAGE_BUFFER);
                ctx.set_final_state(output,
                                    buffer_access_desc{
                                        .usage = buffer_usage::UNIFORM_BUFFER,
                                        .domain = pipeline_domain::graphics,
                                        .queue = queue_class::graphics,
                                    });
                ctx.declare_buffer_output(output);
            }, [](execute_context& ctx) { ctx.commands().record_user_command(); });
            RG_CHECK(rg.compile().succeeded());
            const auto& plan = rg.get_submission_plan();
            RG_CHECK(plan.batches.size() == 2);
            RG_CHECK(plan.batches[0].queue == queue_class::compute);
            RG_CHECK(plan.batches[1].queue == queue_class::graphics);
            RG_CHECK(plan.batches[1].passes.empty());
            RG_CHECK(plan.batches[1].waits.size() == 1);
            RG_CHECK(plan.batches[0].release_barriers.size() == 1);
            RG_CHECK(plan.batches[1].acquire_barriers.size() == 1);
            std::array<test_command_context, 2> commands;
            RG_CHECK(rg.execute_batches(commands).succeeded());
        }
    }

    void multi_queue_test()
    {
        graphics_compute_graphics_test();
        copy_to_graphics_and_fallback_test();
        read_only_and_multiple_consumer_test();
        epilogue_queue_transfer_test();
    }
}
