#include "render_graph/unit_test/synchronization_plan_test.h"

#include <algorithm>
#include <cstdint>
#include <vector>

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

        void noop_execute(pass_execute_context&) {}

        test_image_desc image_desc(format image_format, image_usage usage)
        {
            return test_image_desc{
                .fmt = image_format,
                .extent = {.width = 64, .height = 64, .depth = 1},
                .usage = usage,
            };
        }

        test_buffer_desc buffer_desc(buffer_usage usage)
        {
            return test_buffer_desc{.size = 256, .usage = usage};
        }

        std::vector<const synchronization_op*> pass_ops(const synchronization_plan& plan,
                                                        pass_handle pass,
                                                        synchronization_scope scope)
        {
            const auto& begins = scope == synchronization_scope::pass_prologue ? plan.prologue_begins : plan.internal_begins;
            const auto& lengths = scope == synchronization_scope::pass_prologue ? plan.prologue_lengths : plan.internal_lengths;
            std::vector<const synchronization_op*> result;
            const auto begin = begins[pass];
            const auto end = begin + lengths[pass];
            for (auto index = begin; index < end; index++)
            {
                result.push_back(&plan.ops[index]);
            }
            return result;
        }

        const synchronization_op* find_resource_op(const std::vector<const synchronization_op*>& ops,
                                                    resource_kind kind,
                                                    resource_handle logical,
                                                    uint32_t after_usage)
        {
            const auto found = std::ranges::find_if(
                ops,
                [&](const synchronization_op* op)
                {
                    return op->kind == kind && op->logical == logical && op->after.usage_bits == after_usage;
                });
            return found == ops.end() ? nullptr : *found;
        }

        bool equal_plans(const synchronization_plan& left, const synchronization_plan& right)
        {
            return left.prologue_begins == right.prologue_begins &&
                   left.prologue_lengths == right.prologue_lengths &&
                   left.internal_begins == right.internal_begins &&
                   left.internal_lengths == right.internal_lengths &&
                   left.epilogue_begin == right.epilogue_begin &&
                   left.epilogue_length == right.epilogue_length &&
                   left.ops == right.ops;
        }

        void swapchain_contract_test()
        {
            system_t rg;
            image_handle swapchain{};
            const auto draw = rg.add_pass(
                "draw",
                [&](pass_setup_context& ctx)
                {
                    swapchain = ctx.create_image("Swapchain",
                                                 image_desc(format::B8G8R8A8_UNORM,
                                                            image_usage::COLOR_ATTACHMENT | image_usage::PRESENT),
                                                 resource_lifetime_class::imported);
                    const image_access_desc present{
                        .usage = image_usage::PRESENT,
                        .domain = pipeline_domain::graphics,
                        .queue = queue_class::graphics,
                    };
                    ctx.set_initial_state(swapchain, present, access_type::read, contents_policy::preserve);
                    ctx.write_image(swapchain,
                                    image_access_desc{
                                        .usage = image_usage::COLOR_ATTACHMENT,
                                        .domain = pipeline_domain::graphics,
                                        .queue = queue_class::graphics,
                                    });
                    ctx.set_final_state(swapchain, present, access_type::read);
                    ctx.declare_image_output(swapchain);
                },
                noop_execute);

            RG_CHECK(rg.compile().succeeded());
            const auto& plan = rg.get_synchronization_plan();
            const auto prologue = pass_ops(plan, draw, synchronization_scope::pass_prologue);
            const auto* to_attachment = find_resource_op(
                prologue, resource_kind::image, swapchain, static_cast<uint32_t>(image_usage::COLOR_ATTACHMENT));
            RG_CHECK(to_attachment != nullptr);
            RG_CHECK(to_attachment->before.usage_bits == static_cast<uint32_t>(image_usage::PRESENT));
            RG_CHECK(has_intent(to_attachment->intents, synchronization_intent::layout_transition));
            RG_CHECK(to_attachment->after.domain == pipeline_domain::graphics);

            RG_CHECK(plan.epilogue_length == 1);
            const auto& to_present = plan.ops[plan.epilogue_begin];
            RG_CHECK(to_present.scope == synchronization_scope::graph_epilogue);
            RG_CHECK(to_present.logical == swapchain);
            RG_CHECK(to_present.after.usage_bits == static_cast<uint32_t>(image_usage::PRESENT));
            RG_CHECK(has_intent(to_present.intents, synchronization_intent::layout_transition));
        }

        void ordered_internal_storage_test()
        {
            system_t rg;
            image_handle storage{};
            const auto compute = rg.add_pass(
                "ordered storage",
                [&](pass_setup_context& ctx)
                {
                    storage = ctx.create_image("Storage", image_desc(format::R8G8B8A8_UNORM, image_usage::STORAGE));
                    const image_access_desc state{
                        .usage = image_usage::STORAGE,
                        .domain = pipeline_domain::compute,
                        .queue = queue_class::compute,
                    };
                    ctx.write_image(storage, state);
                    ctx.read_image(storage, state);
                    ctx.write_image(storage, state);
                    ctx.write_image(storage, state);
                    ctx.declare_image_output(storage);
                },
                noop_execute);

            RG_CHECK(rg.compile().succeeded());
            const auto first = rg.get_synchronization_plan();
            const auto internal = pass_ops(first, compute, synchronization_scope::pass_internal);
            RG_CHECK(internal.size() == 3);
            for (const auto* op : internal)
            {
                RG_CHECK(!has_intent(op->intents, synchronization_intent::layout_transition));
                RG_CHECK(has_intent(op->intents, synchronization_intent::execution_dependency));
                RG_CHECK(has_intent(op->intents, synchronization_intent::memory_dependency));
                RG_CHECK(op->after.domain == pipeline_domain::compute);
            }
            RG_CHECK(internal[0]->before.access == access_type::write && internal[0]->after.access == access_type::read);
            RG_CHECK(internal[2]->before.access == access_type::write && internal[2]->after.access == access_type::write);

            RG_CHECK(rg.compile().succeeded());
            RG_CHECK(equal_plans(first, rg.get_synchronization_plan()));
        }

        void state_matrix_test()
        {
            system_t rg;
            std::vector<buffer_handle> buffers;
            image_handle depth{};
            image_handle output{};
            const std::vector<buffer_usage> consumer_usages{
                buffer_usage::VERTEX_BUFFER,
                buffer_usage::INDEX_BUFFER,
                buffer_usage::INDIRECT_BUFFER,
                buffer_usage::UNIFORM_BUFFER,
            };

            rg.add_pass(
                "uploads",
                [&](pass_setup_context& ctx)
                {
                    for (const auto usage : consumer_usages)
                    {
                        const auto buffer = ctx.create_buffer("Upload", buffer_desc(buffer_usage::TRANSFER_DST | usage));
                        buffers.push_back(buffer);
                        ctx.write_buffer(buffer,
                                         buffer_access_desc{
                                             .usage = buffer_usage::TRANSFER_DST,
                                             .domain = pipeline_domain::copy,
                                             .queue = queue_class::copy,
                                             .bytes = {.offset = static_cast<uint64_t>(buffers.size() - 1) * 16, .size = 16},
                                         });
                    }
                    depth = ctx.create_image("Depth",
                                             image_desc(format::D32_SFLOAT,
                                                        image_usage::DEPTH_STENCIL_ATTACHMENT | image_usage::SAMPLED));
                    ctx.write_image(depth,
                                    image_access_desc{
                                        .usage = image_usage::DEPTH_STENCIL_ATTACHMENT,
                                        .domain = pipeline_domain::graphics,
                                        .subresource = {.aspects = image_aspect::depth},
                                    });
                },
                noop_execute);
            const auto consume = rg.add_pass(
                "consume",
                [&](pass_setup_context& ctx)
                {
                    for (size_t index = 0; index < buffers.size(); index++)
                    {
                        ctx.read_buffer(buffers[index],
                                        buffer_access_desc{
                                            .usage = consumer_usages[index],
                                            .domain = pipeline_domain::graphics,
                                            .queue = queue_class::graphics,
                                            .bytes = {.offset = static_cast<uint64_t>(index) * 16, .size = 16},
                                        });
                    }
                    ctx.read_image(depth,
                                   image_access_desc{
                                       .usage = image_usage::SAMPLED,
                                       .domain = pipeline_domain::graphics,
                                       .subresource = {.aspects = image_aspect::depth},
                                   });
                    output = ctx.create_image("Output", image_desc(format::R8G8B8A8_UNORM, image_usage::COLOR_ATTACHMENT));
                    ctx.write_image(output,
                                    image_access_desc{
                                        .usage = image_usage::COLOR_ATTACHMENT,
                                        .domain = pipeline_domain::graphics,
                                    });
                    ctx.declare_image_output(output);
                },
                noop_execute);

            RG_CHECK(rg.compile().succeeded());
            const auto ops = pass_ops(rg.get_synchronization_plan(), consume, synchronization_scope::pass_prologue);
            for (size_t index = 0; index < buffers.size(); index++)
            {
                const auto* op = find_resource_op(
                    ops, resource_kind::buffer, buffers[index], static_cast<uint32_t>(consumer_usages[index]));
                RG_CHECK(op != nullptr);
                RG_CHECK(op->before.domain == pipeline_domain::copy);
                RG_CHECK(op->after.domain == pipeline_domain::graphics);
                RG_CHECK(has_intent(op->intents, synchronization_intent::memory_dependency));
                RG_CHECK(has_intent(op->intents, synchronization_intent::queue_ownership));
                RG_CHECK(op->after.buffer_range ==
                         (buffer_byte_range{.offset = static_cast<uint64_t>(index) * 16, .size = 16}));
            }
            const auto* depth_to_sampled = find_resource_op(
                ops, resource_kind::image, depth, static_cast<uint32_t>(image_usage::SAMPLED));
            RG_CHECK(depth_to_sampled != nullptr);
            RG_CHECK(has_intent(depth_to_sampled->intents, synchronization_intent::layout_transition));
            RG_CHECK(depth_to_sampled->after.image_range.aspects == image_aspect::depth);
        }
    }

    void synchronization_plan_test()
    {
        swapchain_contract_test();
        ordered_internal_storage_test();
        state_matrix_test();
    }
}
