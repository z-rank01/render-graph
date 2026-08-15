// Compiler contract tests: exercise the compile pipeline against the public
// contract — dependency ordering, aliasing, multi-queue submission, table
// validation, stable cache hashing, and the injected upload pass.
#include "compiler_contract_test.h"

#include <array>
#include <vector>

#include "render_graph/compiler.h"
#include "test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        // =========================================================================
        // Test helpers
        // =========================================================================

        // Owns the row containers of one recipe and publishes them into a plan.
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

        // =========================================================================
        // Test cases
        // =========================================================================

        // Pass order follows dependency edges and synchronization ops are emitted.
        void dependency_and_synchronization()
        {
            auto storage = make_dependency_recipe();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(output.succeeded());
            RG_CHECK(output.plan.scheduled_passes.size() == 2);
            RG_CHECK(output.plan.scheduled_passes[0] == pass_handle{0});
            RG_CHECK(output.plan.scheduled_passes[1] == pass_handle{1});
            RG_CHECK(output.plan.statistics.synchronization_op_count >= 2);
            RG_CHECK(output.plan.submissions.batch_queues.size() == 1);
            RG_CHECK(output.plan.passes.color_counts[1] == 1);
        }

        // Non-overlapping transient images may share one physical memory block.
        // Both compute passes use side_effect so they survive culling without
        // changing the transient-image lifetimes (which would break aliasing).
        void aliasing_contract()
        {
            recipe_storage storage;
            storage.resources = {
                {.source = frame_resource_source::transient_image, .name = "first",
                 .image_description = color_desc()},
                {.source = frame_resource_source::transient_image, .name = "second",
                 .image_description = color_desc()},
                {.source = frame_resource_source::swapchain_image, .name = "swapchain"},
            };
            storage.images = {
                {.resource = {0}, .usage = image_usage::COLOR_ATTACHMENT, .access = access_type::write},
                {.resource = {1}, .usage = image_usage::COLOR_ATTACHMENT, .access = access_type::write},
            };
            storage.attachments = {{.resource = {2}, .kind = frame_attachment_kind::color}};
            storage.passes = {
                {.name = "first-use", .kind = pass_kind::compute, .image_accesses = {0, 1}, .side_effect = true},
                {.name = "second-use", .kind = pass_kind::compute, .image_accesses = {1, 1}, .side_effect = true},
                {.name = "present", .kind = pass_kind::raster, .attachments = {0, 1}},
            };
            storage.publish();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(output.succeeded());
            const auto first = output.plan.frame_images[0];
            const auto second = output.plan.frame_images[1];
            RG_CHECK(output.plan.physical_resources.handle_to_physical_img_id[first.index()] ==
                     output.plan.physical_resources.handle_to_physical_img_id[second.index()]);
            RG_CHECK(!output.plan.physical_resources.image_alias_handoffs.empty());
        }

        // Queues split into separate submission batches with release/acquire pairs.
        void multiqueue_contract()
        {
            auto storage = make_dependency_recipe(true);
            const auto output = compile_graph(request_for(storage.plan, true));
            RG_CHECK(output.succeeded());
            const auto& submissions = output.plan.submissions;
            RG_CHECK(submissions.batch_queues.size() == 2);
            RG_CHECK(submissions.batch_queues[0] == queue_class::compute);
            RG_CHECK(submissions.batch_queues[1] == queue_class::graphics);
            // One timeline wait: batch 1 waits on batch 0's signal value.
            RG_CHECK(submissions.batch_waits.size() == 1);
            RG_CHECK(submissions.batch_waits[0].source_batch == submission_batch_handle{0});
            RG_CHECK(submissions.batch_waits[0].value == submissions.batch_signal_values[0]);
            RG_CHECK(submissions.batch_wait_begins[0] == 0);
            RG_CHECK(submissions.batch_wait_begins[1] == 0);
            RG_CHECK(submissions.batch_wait_begins[2] == 1);
            RG_CHECK(submissions.buffer_cross_queue_dependencies.size() == 1);
            // The split barrier is referenced (not copied): release lives in
            // batch 0, acquire in batch 1, both pointing at the same buffer
            // op row.
            RG_CHECK(submissions.release_refs.size() == 1);
            RG_CHECK(submissions.release_refs[0].buffer_op != invalid_op_index);
            RG_CHECK(submissions.release_refs[0].image_op == invalid_op_index);
            RG_CHECK(submissions.release_refs[0].phase == synchronization_phase::release);
            RG_CHECK(submissions.batch_release_begins[0] == 0);
            RG_CHECK(submissions.batch_release_begins[1] == 1);
            RG_CHECK(submissions.batch_release_begins[2] == 1);
            RG_CHECK(submissions.acquire_refs.size() == 1);
            RG_CHECK(submissions.acquire_refs[0].buffer_op == submissions.release_refs[0].buffer_op);
            RG_CHECK(submissions.acquire_refs[0].phase == synchronization_phase::acquire);
            RG_CHECK(submissions.batch_acquire_begins[0] == 0);
            RG_CHECK(submissions.batch_acquire_begins[1] == 0);
            RG_CHECK(submissions.batch_acquire_begins[2] == 1);
        }

        // Out-of-range row indices are rejected with a diagnostic.
        void validation_contract()
        {
            auto storage = make_dependency_recipe();
            storage.passes[0].buffer_accesses = {99, 1};
            storage.publish();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(!output.succeeded());
            RG_CHECK(output.result.diagnostics.front().code == compile_error_code::access_limit_exceeded);
        }

        // The compile hash is stable across equivalent recipes and changes
        // when a buffer range differs.
        void stable_hash_contract()
        {
            auto storage = make_dependency_recipe();
            storage.draws.resize(1);
            storage.passes[1].indexed_indirect_draws = {0, 1};
            storage.publish(99);
            const auto first = compile_graph(request_for(storage.plan));
            RG_CHECK(first.succeeded());
            storage.draws.resize(8);
            storage.passes[1].indexed_indirect_draws = {0, 8};
            storage.publish(99);
            const auto second = compile_graph(request_for(storage.plan));
            RG_CHECK(second.succeeded());
            RG_CHECK(first.plan.cache_key == second.plan.cache_key);

            storage.buffers[1].range = {.offset = 128, .size = 256};
            storage.publish(99);
            const auto range_changed = compile_graph(request_for(storage.plan));
            RG_CHECK(range_changed.succeeded());
            RG_CHECK(first.plan.cache_key != range_changed.plan.cache_key);
        }

        // An injected upload pass appears in front of the draw, holding the
        // stable upload buffer.
        void upload_contract()
        {
            recipe_storage storage;
            storage.resources = {{.source = frame_resource_source::swapchain_image, .name = "swapchain"}};
            storage.attachments = {{.resource = {0}, .kind = frame_attachment_kind::color}};
            storage.passes = {{.name = "draw", .kind = pass_kind::raster, .attachments = {0, 1}}};
            storage.publish();
            auto request = request_for(storage.plan);
            request.inject_stable_upload_pass = true;
            request.upload_buffer_desc = {.size = 4096, .usage = buffer_usage::TRANSFER_SRC,
                                          .memory = memory_domain::upload,
                                          .mapping = mapping_policy::persistent};
            const auto output = compile_graph(request);
            RG_CHECK(output.succeeded());
            RG_CHECK(output.plan.passes.size() == 2);
            RG_CHECK(output.plan.passes.is_backend_upload(0));
            RG_CHECK(output.plan.upload_buffer != invalid_buffer);
        }

        // =========================================================================
        // Pass culling contract
        // =========================================================================

        // Scenario 1: a compute pass + transient buffer with no consumer — culled.
        void culling_basic()
        {
            recipe_storage storage;
            storage.resources = {
                {.source = frame_resource_source::transient_buffer, .name = "dead",
                 .buffer_description = {.size = 256, .usage = buffer_usage::STORAGE_BUFFER}},
                {.source = frame_resource_source::swapchain_image, .name = "swapchain"},
            };
            storage.buffers = {
                {.resource = {0}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::write},
            };
            storage.attachments = {{.resource = {1}, .kind = frame_attachment_kind::color}};
            storage.passes = {
                {.name = "orphan", .kind = pass_kind::compute, .buffer_accesses = {0, 1}},
                {.name = "present", .kind = pass_kind::raster, .attachments = {0, 1}},
            };
            storage.publish();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(output.succeeded());
            // The orphan pass must be culled; present is the only scheduled pass.
            // The pass table is physically compacted, so the surviving row
            // references its original frame index through source_passes.
            RG_CHECK(output.plan.scheduled_passes.size() == 1);
            RG_CHECK(output.plan.scheduled_passes[0] == pass_handle{0});
            RG_CHECK(output.plan.statistics.culled_pass_count == 1);
            RG_CHECK(output.plan.passes.size() == 1);
            RG_CHECK(output.plan.passes.source_passes[0] == 1);
            // The dead transient buffer gets no physical allocation
            const auto dead_buf = output.plan.frame_buffers[0];
            RG_CHECK(output.plan.physical_resources.handle_to_physical_buf_id[dead_buf.index()] == invalid_physical_buffer_id);
            RG_CHECK(output.plan.physical_resources.buffer_memory_blocks.empty());
        }

        // Scenario 2: a compute pass with side_effect=true survives even without
        // a data consumer.
        void culling_side_effect()
        {
            recipe_storage storage;
            storage.resources = {
                {.source = frame_resource_source::transient_buffer, .name = "work",
                 .buffer_description = {.size = 256, .usage = buffer_usage::STORAGE_BUFFER}},
                {.source = frame_resource_source::swapchain_image, .name = "swapchain"},
            };
            storage.buffers = {
                {.resource = {0}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::write},
            };
            storage.attachments = {{.resource = {1}, .kind = frame_attachment_kind::color}};
            storage.passes = {
                {.name = "side", .kind = pass_kind::compute, .buffer_accesses = {0, 1}, .side_effect = true},
                {.name = "present", .kind = pass_kind::raster, .attachments = {0, 1}},
            };
            storage.publish();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(output.succeeded());
            RG_CHECK(output.plan.scheduled_passes.size() == 2);
            RG_CHECK(output.plan.statistics.culled_pass_count == 0);
            RG_CHECK(output.plan.passes.size() == 2);
            RG_CHECK(output.plan.passes.source_passes[0] == 0);
            RG_CHECK(output.plan.passes.source_passes[1] == 1);
        }

        // Scenario 3: producer chain — A→B→root survive, unrelated D culled.
        void culling_chain()
        {
            recipe_storage storage;
            storage.resources = {
                {.source = frame_resource_source::transient_buffer, .name = "buf_a",
                 .buffer_description = {.size = 256, .usage = buffer_usage::STORAGE_BUFFER}},
                {.source = frame_resource_source::transient_buffer, .name = "buf_b",
                 .buffer_description = {.size = 256, .usage = buffer_usage::STORAGE_BUFFER}},
                {.source = frame_resource_source::transient_buffer, .name = "buf_c",
                 .buffer_description = {.size = 256, .usage = buffer_usage::STORAGE_BUFFER}},
                {.source = frame_resource_source::swapchain_image, .name = "swapchain"},
            };
            storage.buffers = {
                {.resource = {0}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::write}, // A → buf_a
                {.resource = {0}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::read},  // B reads buf_a
                {.resource = {1}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::write}, // B → buf_b
                {.resource = {1}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::read},  // root reads buf_b
                {.resource = {2}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::write}, // D → buf_c (orphan)
            };
            storage.attachments = {{.resource = {3}, .kind = frame_attachment_kind::color}};
            storage.passes = {
                {.name = "producer", .kind = pass_kind::compute, .buffer_accesses = {0, 1}},
                {.name = "middle", .kind = pass_kind::compute, .buffer_accesses = {1, 2}},
                {.name = "orphan_d", .kind = pass_kind::compute, .buffer_accesses = {4, 1}},
                {.name = "present", .kind = pass_kind::raster,
                 .buffer_accesses = {3, 1}, .attachments = {0, 1}},
            };
            storage.publish();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(output.succeeded());
            // producer (0), middle (1), present (3) survive; orphan_d (2) is culled.
            // Compaction keeps only the surviving rows, in declaration order.
            RG_CHECK(output.plan.scheduled_passes.size() == 3);
            RG_CHECK(output.plan.statistics.culled_pass_count == 1);
            RG_CHECK(output.plan.passes.size() == 3);
            RG_CHECK(output.plan.passes.source_passes[0] == 0);
            RG_CHECK(output.plan.passes.source_passes[1] == 1);
            RG_CHECK(output.plan.passes.source_passes[2] == 3);
            // buf_c (resource index 2) is orphan — no physical allocation
            const auto buf_c = output.plan.frame_buffers[2];
            RG_CHECK(output.plan.physical_resources.handle_to_physical_buf_id[buf_c.index()] == invalid_physical_buffer_id);
            RG_CHECK(output.plan.physical_resources.buffer_memory_blocks.size() == 2);
        }

        void culling_contract()
        {
            culling_basic();
            culling_side_effect();
            culling_chain();
        }

        // =========================================================================
        // Per-pass render area contract
        // =========================================================================

        // A pass-level area overrides the frame extent; a 0×0 area falls back
        // to the frame extent (backward compatible with existing recipes).
        void pass_area_contract()
        {
            recipe_storage storage;
            storage.resources = {{.source = frame_resource_source::swapchain_image, .name = "swapchain"}};
            storage.attachments = {{.resource = {0}, .kind = frame_attachment_kind::color}};
            storage.passes = {
                {.name = "half", .kind = pass_kind::raster,
                 .attachments = {0, 1},
                 .area = {.x = 16, .y = 32, .width = 2048, .height = 1024}},
                {.name = "full", .kind = pass_kind::raster,
                 .attachments = {0, 1}},
            };
            storage.publish();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(output.succeeded());
            RG_CHECK(output.plan.passes.areas.size() == 2);
            RG_CHECK(output.plan.passes.areas[0].width == 2048);
            RG_CHECK(output.plan.passes.areas[0].height == 1024);
            RG_CHECK(output.plan.passes.areas[0].x == 16);
            RG_CHECK(output.plan.passes.areas[0].y == 32);
            RG_CHECK(output.plan.passes.areas[1].width == 1280); // falls back to the frame extent
            RG_CHECK(output.plan.passes.areas[1].height == 720);
        }

        // =========================================================================
        // Multi-pass color CSR contract
        // =========================================================================

        // 三个连续 raster pass 各带一个颜色附件：每 pass 的 color span 必须只含
        // 自己的颜色（begins 按附件入列顺序修正，counts 逐 pass 结算）。回归：
        // pass 行统一创建导致 color_begins 全为 0 时，≥3 pass 的中间/尾部 pass
        // 会把前面 pass 的颜色一起包进 span（M3 第三 pass 暴露的潜在缺陷）。
        void multi_pass_color_csr_contract()
        {
            recipe_storage storage;
            storage.resources = {
                {.source = frame_resource_source::transient_image, .name = "c0",
                 .image_description = color_desc()},
                {.source = frame_resource_source::transient_image, .name = "c1",
                 .image_description = color_desc()},
                {.source = frame_resource_source::transient_image, .name = "c2",
                 .image_description = color_desc()},
            };
            storage.attachments = {
                {.resource = {0}, .kind = frame_attachment_kind::color},
                {.resource = {1}, .kind = frame_attachment_kind::color},
                {.resource = {2}, .kind = frame_attachment_kind::color},
            };
            storage.passes = {
                {.name = "p0", .kind = pass_kind::raster, .attachments = {0, 1}, .side_effect = true},
                {.name = "p1", .kind = pass_kind::raster, .attachments = {1, 1}, .side_effect = true},
                {.name = "p2", .kind = pass_kind::raster, .attachments = {2, 1}, .side_effect = true},
            };
            storage.publish();
            const auto output = compile_graph(request_for(storage.plan));
            RG_CHECK(output.succeeded());
            RG_CHECK(output.plan.passes.size() == 3);
            RG_CHECK(output.plan.passes.color_begins[0] == 0);
            RG_CHECK(output.plan.passes.color_begins[1] == 1);
            RG_CHECK(output.plan.passes.color_begins[2] == 2);
            RG_CHECK(output.plan.passes.color_counts[0] == 1);
            RG_CHECK(output.plan.passes.color_counts[1] == 1);
            RG_CHECK(output.plan.passes.color_counts[2] == 1);
            // 每 pass 的颜色就是自己的附件（CSR 引用的颜色行与附件一一对应）
            RG_CHECK(output.plan.passes.colors.size() == 3);
        }
    }

    // Routes CLI test names (shared with the core test runner) to the right case.
    void compiler_contract_test(std::string_view requested)
    {
        if (requested == "validation_compile" || requested == "hardening" ||
            requested == "dag_cycle_compile")
            validation_contract();
        else if (requested == "lifetime_aliasing") aliasing_contract();
        else if (requested == "multi_queue") multiqueue_contract();
        else if (requested == "repeat_compile" || requested == "frame_lifecycle") stable_hash_contract();
        else if (requested == "execute_context") upload_contract();
        else if (requested == "culling_compile") culling_contract();
        else if (requested == "raster_pass")
        {
            pass_area_contract();
            multi_pass_color_csr_contract();
            dependency_and_synchronization();
        }
        else dependency_and_synchronization();
    }
}
