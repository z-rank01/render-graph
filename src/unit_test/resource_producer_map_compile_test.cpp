#include "render_graph/unit_test/resource_producer_map_compile_test.h"

#include <limits>
#include <vector>

#include "render_graph/system.h"

namespace render_graph::unit_test
{
    namespace
    {
        struct test_state_t
        {
            // Images
            resource_handle img_a1            = 0;
            resource_handle img_a2            = 0;
            resource_handle img_b2            = 0;
            resource_handle img_swapchain     = 0;
            resource_handle img_external_only = 0; // created/imported, only read

            // Buffers
            resource_handle buf_b1 = 0;
            resource_handle buf_b3 = 0;

            // Expected version->producer map (by handle, then by version index)
            std::vector<std::vector<pass_handle>> expected_img_versions;
            std::vector<std::vector<pass_handle>> expected_buf_versions;

            // Flattened expected tables (match version_producer_map layout)
            std::vector<uint32_t> expected_img_version_offsets;
            std::vector<pass_handle> expected_img_version_producers;
            std::vector<resource_version_handle> expected_img_latest;

            std::vector<uint32_t> expected_buf_version_offsets;
            std::vector<pass_handle> expected_buf_version_producers;
            std::vector<resource_version_handle> expected_buf_latest;

            static pass_handle invalid_pass()  { return std::numeric_limits<pass_handle>::max(); }

            void reset()
            {
                *this = test_state_t{};
                expected_img_versions.clear();
                expected_buf_versions.clear();
                expected_img_version_offsets.clear();
                expected_img_version_producers.clear();
                expected_img_latest.clear();
                expected_buf_version_offsets.clear();
                expected_buf_version_producers.clear();
                expected_buf_latest.clear();
            }

            void record_img_write(resource_handle image, pass_handle producer)
            {
                if (expected_img_versions.size() <= image)
                {
                    expected_img_versions.resize(static_cast<size_t>(image) + 1);
                }
                expected_img_versions[image].push_back(producer);
            }

            void record_buf_write(resource_handle buffer, pass_handle producer)
            {
                if (expected_buf_versions.size() <= buffer)
                {
                    expected_buf_versions.resize(static_cast<size_t>(buffer) + 1);
                }
                expected_buf_versions[buffer].push_back(producer);
            }

            void build_expected_flat(resource_handle image_count, resource_handle buffer_count)
            {
                // Ensure storage exists for every handle in system meta table.
                if (expected_img_versions.size() < image_count)
                {
                    expected_img_versions.resize(image_count);
                }
                if (expected_buf_versions.size() < buffer_count)
                {
                    expected_buf_versions.resize(buffer_count);
                }

                // Images
                expected_img_version_offsets.assign(static_cast<size_t>(image_count) + 1, 0);
                expected_img_latest.assign(image_count, invalid_resource_version);
                {
                    uint32_t running = 0;
                    for (resource_handle image_handle = 0; image_handle < image_count; image_handle++)
                    {
                        expected_img_version_offsets[image_handle] = running;
                        const auto count = static_cast<uint32_t>(expected_img_versions[image_handle].size());
                        if (count > 0)
                        {
                            expected_img_latest[image_handle] = pack(image_handle, static_cast<version_handle>(count - 1));
                        }
                        running += count;
                    }
                    expected_img_version_offsets[image_count] = running;
                    expected_img_version_producers.assign(running, invalid_pass());
                    for (resource_handle image_handle = 0; image_handle < image_count; image_handle++)
                    {
                        const auto base = expected_img_version_offsets[image_handle];
                        const auto& per_version = expected_img_versions[image_handle];
                        for (size_t ver = 0; ver < per_version.size(); ver++)
                        {
                            expected_img_version_producers[static_cast<size_t>(base) + ver] = per_version[ver];
                        }
                    }
                }

                // Buffers
                expected_buf_version_offsets.assign(static_cast<size_t>(buffer_count) + 1, 0);
                expected_buf_latest.assign(buffer_count, invalid_resource_version);
                {
                    uint32_t running = 0;
                    for (resource_handle buffer_handle = 0; buffer_handle < buffer_count; buffer_handle++)
                    {
                        expected_buf_version_offsets[buffer_handle] = running;
                        const auto count = static_cast<uint32_t>(expected_buf_versions[buffer_handle].size());
                        if (count > 0)
                        {
                            expected_buf_latest[buffer_handle] = pack(buffer_handle, static_cast<version_handle>(count - 1));
                        }
                        running += count;
                    }
                    expected_buf_version_offsets[buffer_count] = running;
                    expected_buf_version_producers.assign(running, invalid_pass());
                    for (resource_handle buffer_handle = 0; buffer_handle < buffer_count; buffer_handle++)
                    {
                        const auto base = expected_buf_version_offsets[buffer_handle];
                        const auto& per_version = expected_buf_versions[buffer_handle];
                        for (size_t ver = 0; ver < per_version.size(); ver++)
                        {
                            expected_buf_version_producers[static_cast<size_t>(base) + ver] = per_version[ver];
                        }
                    }
                }
            }
        };

        test_state_t& test_state()
        {
            static test_state_t state{};
            return state;
        }

        void noop_execute(pass_execute_context&) { }

        // Pass 0: create/write a1, a2, b1
        void pass_a_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.img_a1 = ctx.create_image(image_info{
                .name          = "img_a1",
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
            ctx.write_image(state.img_a1, image_usage::COLOR_ATTACHMENT);
            state.record_img_write(state.img_a1, ctx.current_pass);

            state.img_a2 = ctx.create_image(image_info{
                .name          = "img_a2",
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
            ctx.write_image(state.img_a2, image_usage::COLOR_ATTACHMENT);
            state.record_img_write(state.img_a2, ctx.current_pass);

            state.buf_b1 = ctx.create_buffer(buffer_info{
                .name     = "buf_b1",
                .size     = 1024,
                .usage    = buffer_usage::NONE,
                .imported = false,
            });
            ctx.write_buffer(state.buf_b1, buffer_usage::STORAGE_BUFFER);
            state.record_buf_write(state.buf_b1, ctx.current_pass);
        }

        // Pass 1: read a1, write b2, rewrite b1 (overwrite producer)
        void pass_b_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.img_a1, image_usage::SAMPLED);

            state.img_b2 = ctx.create_image(image_info{
                .name          = "img_b2",
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
            ctx.write_image(state.img_b2, image_usage::COLOR_ATTACHMENT);
            state.record_img_write(state.img_b2, ctx.current_pass);

            // Overwrite producer for buf_b1
            ctx.read_buffer(state.buf_b1, buffer_usage::STORAGE_BUFFER);
            ctx.write_buffer(state.buf_b1, buffer_usage::STORAGE_BUFFER);
            state.record_buf_write(state.buf_b1, ctx.current_pass);
        }

        // Pass 2: read b2 & b1, rewrite a2 (overwrite producer), create/write b3
        void pass_c_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.img_b2, image_usage::SAMPLED);
            ctx.read_buffer(state.buf_b1, buffer_usage::STORAGE_BUFFER);

            // Rewrite producer for img_a2
            ctx.write_image(state.img_a2, image_usage::COLOR_ATTACHMENT);
            state.record_img_write(state.img_a2, ctx.current_pass);

            state.buf_b3 = ctx.create_buffer(buffer_info{
                .name     = "buf_b3",
                .size     = 2048,
                .usage    = buffer_usage::NONE,
                .imported = false,
            });
            ctx.write_buffer(state.buf_b3, buffer_usage::STORAGE_BUFFER);
            state.record_buf_write(state.buf_b3, ctx.current_pass);
        }

        // Pass 3: imported external image that is only read (no write) -> producer should remain invalid
        void pass_external_input_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.img_external_only = ctx.create_image(image_info{
                .name          = "img_external_only",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = image_usage::SAMPLED,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });

            ctx.read_image(state.img_external_only, image_usage::SAMPLED);
            // no write recorded -> expected producer table will remain invalid for this handle
        }

        // Pass 4: read a2 & external, write imported swapchain
        void pass_present_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            ctx.read_image(state.img_a2, image_usage::SAMPLED);
            ctx.read_image(state.img_external_only, image_usage::SAMPLED);

            state.img_swapchain = ctx.create_image(image_info{
                .name          = "swapchain_backbuffer_test",
                .fmt           = format::R8G8B8A8_UNORM,
                .extent        = {.width = 256, .height = 256, .depth = 1},
                .usage         = image_usage::COLOR_ATTACHMENT,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });

            ctx.write_image(state.img_swapchain, image_usage::COLOR_ATTACHMENT);
            state.record_img_write(state.img_swapchain, ctx.current_pass);
        }
    } // namespace

    void resource_producer_map_compile_test()
    {
        auto& state = test_state();
        state.reset();

        render_graph_system system;

        system.add_pass(pass_a_setup, noop_execute);
        system.add_pass(pass_b_setup, noop_execute);
        system.add_pass(pass_c_setup, noop_execute);
        system.add_pass(pass_external_input_setup, noop_execute);
        system.add_pass(pass_present_setup, noop_execute);

        system.compile();

        // Build the expected flat tables using the same shapes as the system.
        state.build_expected_flat(static_cast<resource_handle>(system.meta_table.image_metas.names.size()),
                     static_cast<resource_handle>(system.meta_table.buffer_metas.names.size()));

        // Set a breakpoint here and inspect:
        // - system.producer_lookup_table.img_version_offsets / img_version_producers / img_latest
        // - system.producer_lookup_table.buf_version_offsets / buf_version_producers / buf_latest
        // - test_state().expected_*_version_offsets / expected_*_version_producers / expected_*_latest
        // Also pay attention to rewritten resources (img_a2, buf_b1).
        (void)system;
    }
} // namespace render_graph::unit_test
