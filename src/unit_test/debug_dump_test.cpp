// debug_dump contract test: schedule-ordered passes, derived edges, barrier
// and resource sections, cross-queue tagging, aliasing rows, determinism,
// and the empty-plan (pre-compile) shape.
#include "debug_dump_test.h"

#include <string>
#include <vector>

#include "render_graph/compiler.h"
#include "render_graph/debug_dump.h"
#include "test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        // Owns the row containers of one recipe and publishes them into a plan
        // (same harness pattern as compiler_contract_test.cpp).
        struct recipe_storage
        {
            std::vector<frame_resource_row> resources;
            std::vector<frame_pass_row> passes;
            std::vector<frame_buffer_access_row> buffers;
            std::vector<frame_image_access_row> images;
            std::vector<frame_attachment_row> attachments;
            std::vector<draw_indexed_indirect_row> draws;
            frame_plan plan;

            void publish(uint64_t key = 17)
            {
                plan = {
                    .cache_key = key,
                    .resources = resources,
                    .passes = passes,
                    .buffer_accesses = buffers,
                    .image_accesses = images,
                    .attachments = attachments,
                    .indexed_indirect_draws = draws,
                };
            }
        };

        image_desc color_desc(uint32_t size = 64)
        {
            return {.fmt = format::R8G8B8A8_UNORM, .extent = {size, size, 1},
                    .usage = image_usage::COLOR_ATTACHMENT | image_usage::SAMPLED};
        }

        graph_compile_request request_for(const frame_plan& plan, bool multiqueue = false)
        {
            return {
                .frame = &plan,
                .environment = {
                    .extent = {1280, 720, 1},
                    .color_format = format::B8G8R8A8_UNORM,
                    .swapchain_initialized = true,
                    .queues = {.compute = multiqueue, .copy = multiqueue},
                },
            };
        }

        // Producer (write) + consumer (read) over one shared buffer, plus a
        // swapchain color attachment on the graphics pass.
        recipe_storage make_dependency_recipe(bool multiqueue = false)
        {
            recipe_storage storage;
            storage.resources = {
                {.source = frame_resource_source::transient_buffer, .name = "table",
                 .buffer_description = {.size = 1024, .usage = buffer_usage::STORAGE_BUFFER}},
                {.source = frame_resource_source::swapchain_image, .name = "swapchain"},
            };
            storage.buffers = {
                {.resource = {0}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::write},
                {.resource = {0}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::read},
            };
            storage.attachments = {{.resource = {1}, .kind = frame_attachment_kind::color}};
            storage.passes = {
                {.name = "produce", .kind = pass_kind::compute,
                 .queue = multiqueue ? queue_class::compute : queue_class::graphics,
                 .buffer_accesses = {0, 1}},
                {.name = "consume", .kind = pass_kind::raster, .queue = queue_class::graphics,
                 .buffer_accesses = {1, 1}, .attachments = {0, 1}},
            };
            storage.publish();
            return storage;
        }

        void check_contains(const std::string& dump, const std::string& needle)
        {
            RG_CHECK(dump.find(needle) != std::string::npos);
        }
    } // namespace

    void debug_dump_test()
    {
        // Dependency graph: schedule order, derived sync edge, barrier and
        // resource sections, statistics, byte-determinism.
        auto storage = make_dependency_recipe();
        const auto output = compile_graph(request_for(storage.plan));
        RG_CHECK(output.succeeded());
        const std::string dump = debug_dump(output.plan);
        RG_CHECK(dump == debug_dump(output.plan)); // pure function: byte-identical

        const auto produce_pos = dump.find("\"name\":\"produce\"");
        const auto consume_pos = dump.find("\"name\":\"consume\"");
        RG_CHECK(produce_pos != std::string::npos && consume_pos != std::string::npos);
        RG_CHECK(produce_pos < consume_pos); // schedule order follows the dependency
        check_contains(dump, "\"from\":0,\"to\":1,\"kind\":\"sync\"");
        check_contains(dump, "\"resource\":\"table\"");
        check_contains(dump, "\"scope\":\"prologue\"");
        check_contains(dump, "\"name\":\"swapchain\",\"kind\":\"image\",\"imported\":true");
        check_contains(dump, "\"pass_count\":2");

        // Multi-queue compile: the same dependency crosses queues, so the
        // derived edge is tagged cross_queue instead of sync.
        auto mq_storage = make_dependency_recipe(true);
        const auto mq_output = compile_graph(request_for(mq_storage.plan, true));
        RG_CHECK(mq_output.succeeded());
        const std::string mq_dump = debug_dump(mq_output.plan);
        check_contains(mq_dump, "\"from\":0,\"to\":1,\"kind\":\"cross_queue\"");

        // Aliasing: two non-overlapping transient images share memory; the
        // handoff shows up in the aliases section.
        recipe_storage alias_storage;
        alias_storage.resources = {
            {.source = frame_resource_source::transient_image, .name = "first", .image_description = color_desc()},
            {.source = frame_resource_source::transient_image, .name = "second", .image_description = color_desc()},
            {.source = frame_resource_source::swapchain_image, .name = "swapchain"},
        };
        alias_storage.images = {
            {.resource = {0}, .usage = image_usage::COLOR_ATTACHMENT, .access = access_type::write},
            {.resource = {1}, .usage = image_usage::COLOR_ATTACHMENT, .access = access_type::write},
        };
        alias_storage.attachments = {{.resource = {2}, .kind = frame_attachment_kind::color}};
        alias_storage.passes = {
            {.name = "first-use", .kind = pass_kind::compute, .image_accesses = {0, 1}, .side_effect = true},
            {.name = "second-use", .kind = pass_kind::compute, .image_accesses = {1, 1}, .side_effect = true},
            {.name = "present", .kind = pass_kind::raster, .attachments = {0, 1}},
        };
        alias_storage.publish();
        const auto alias_output = compile_graph(request_for(alias_storage.plan));
        RG_CHECK(alias_output.succeeded());
        const std::string alias_dump = debug_dump(alias_output.plan);
        check_contains(alias_dump, "\"aliases\":[{\"kind\":\"image\"");
        check_contains(alias_dump, "\"at_pass\":");

        // Empty plan (device before the first compile): all sections present
        // and empty, no crash on null mappings.
        const compiled_graph_plan empty;
        const std::string empty_dump = debug_dump(empty);
        check_contains(empty_dump, "\"passes\":[]");
        check_contains(empty_dump, "\"edges\":[]");
        check_contains(empty_dump, "\"barriers\":[]");
        check_contains(empty_dump, "\"aliases\":[]");
        check_contains(empty_dump, "\"resources\":[]");
        check_contains(empty_dump, "\"pass_count\":0");
    }
} // namespace render_graph::unit_test
