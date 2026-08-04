#include "render_graph/unit_test/resource_generation_compile_test.h"

#include <vector>

#include "render_graph/system.h"
#include "render_graph/unit_test/system_test_access.h"
#include "render_graph/unit_test/test_backend.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        using system_t = render_graph_system<test_backend>;
        using pass_setup_context = system_t::pass_setup_context;
        using pass_execute_context = system_t::pass_execute_context;

        test_image_desc make_image_desc(format fmt, extent_3d extent, image_usage usage)
        {
            return test_image_desc{
                .fmt           = fmt,
                .extent        = extent,
                .usage         = usage,
                .type          = image_type::TYPE_2D,
                .flags         = image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
            };
        }

        test_buffer_desc make_buffer_desc(uint64_t size, buffer_usage usage)
        {
            return test_buffer_desc{.size = size, .usage = usage};
        }

        struct expected_stream_t
        {
            // Mirrors the packed dependency lists.
            std::vector<resource_version_handle> image_read_handles;
            std::vector<resource_version_handle> image_read_gens;
            std::vector<resource_version_handle> image_write_handles;
            std::vector<resource_version_handle> image_write_gens;

            std::vector<resource_version_handle> buffer_read_handles;
            std::vector<resource_version_handle> buffer_read_gens;
            std::vector<resource_version_handle> buffer_write_handles;
            std::vector<resource_version_handle> buffer_write_gens;

            // Tracks next generation id to assign on write.
            std::vector<resource_version_handle> image_next_gen;
            std::vector<resource_version_handle> buffer_next_gen;

            void reset()
            {
                *this = expected_stream_t{};
            }

            void ensure_image(resource_version_handle image)
            {
                if (image_next_gen.size() <= image)
                {
                    image_next_gen.resize(static_cast<size_t>(image) + 1, 0);
                }
            }

            void ensure_buffer(resource_version_handle buffer)
            {
                if (buffer_next_gen.size() <= buffer)
                {
                    buffer_next_gen.resize(static_cast<size_t>(buffer) + 1, 0);
                }
            }

            void record_image_read(resource_version_handle image)
            {
                ensure_image(image);
                const auto next = image_next_gen[image];
                const auto gen  = (next == 0) ? 0 : (next - 1);
                image_read_handles.push_back(image);
                image_read_gens.push_back(gen);
            }

            void record_image_write(resource_version_handle image)
            {
                ensure_image(image);
                const auto gen = image_next_gen[image];
                image_write_handles.push_back(image);
                image_write_gens.push_back(gen);
                image_next_gen[image] = gen + 1;
            }

            void record_buffer_read(resource_version_handle buffer)
            {
                ensure_buffer(buffer);
                const auto next = buffer_next_gen[buffer];
                const auto gen  = (next == 0) ? 0 : (next - 1);
                buffer_read_handles.push_back(buffer);
                buffer_read_gens.push_back(gen);
            }

            void record_buffer_write(resource_version_handle buffer)
            {
                ensure_buffer(buffer);
                const auto gen = buffer_next_gen[buffer];
                buffer_write_handles.push_back(buffer);
                buffer_write_gens.push_back(gen);
                buffer_next_gen[buffer] = gen + 1;
            }
        };

        struct test_state_t
        {
            // Images
            resource_version_handle img_g0 = 0;
            resource_version_handle img_g1 = 0;
            resource_version_handle img_l0 = 0;
            resource_version_handle img_external = 0; // imported, only read initially

            // Buffers
            resource_version_handle buf_b0 = 0;
            resource_version_handle buf_b1 = 0;

            expected_stream_t expected;

            void reset() { *this = test_state_t{}; expected.reset(); }
        };

        test_state_t& test_state()
        {
            static test_state_t state{};
            return state;
        }

        void noop_execute(pass_execute_context&) { }

        // Pass0: create/write two images and one buffer, plus a double-write to buffer.
        void pass0_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.img_g0 = ctx.create_image(
                "g0",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 320, .height = 180, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            state.expected.record_image_write(state.img_g0);
            ctx.write_image(state.img_g0, image_usage::COLOR_ATTACHMENT);

            state.img_g1 = ctx.create_image(
                "g1",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 320, .height = 180, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            state.expected.record_image_write(state.img_g1);
            ctx.write_image(state.img_g1, image_usage::COLOR_ATTACHMENT);

            state.buf_b0 = ctx.create_buffer("b0", make_buffer_desc(4096, buffer_usage::STORAGE_BUFFER), false);
            state.expected.record_buffer_write(state.buf_b0);
            ctx.write_buffer(state.buf_b0, buffer_usage::STORAGE_BUFFER);

            ctx.declare_image_output(static_cast<resource_handle>(state.img_g0));

            // Double-write the same buffer in one pass (should produce gen 1 on second write).
            state.expected.record_buffer_write(state.buf_b0);
            ctx.write_buffer(state.buf_b0, buffer_usage::STORAGE_BUFFER);
        }

        // Pass1: read both images, read buffer, then overwrite one image and buffer.
        void pass1_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.expected.record_image_read(state.img_g0);
            ctx.read_image(state.img_g0, image_usage::SAMPLED);
            state.expected.record_image_read(state.img_g1);
            ctx.read_image(state.img_g1, image_usage::SAMPLED);

            state.expected.record_buffer_read(state.buf_b0);
            ctx.read_buffer(state.buf_b0, buffer_usage::STORAGE_BUFFER);

            // Overwrite img_g1
            state.expected.record_image_write(state.img_g1);
            ctx.write_image(state.img_g1, image_usage::COLOR_ATTACHMENT);

            // Overwrite buf_b0
            state.expected.record_buffer_write(state.buf_b0);
            ctx.write_buffer(state.buf_b0, buffer_usage::STORAGE_BUFFER);
        }

        // Pass2: create an imported external image and read it; create/write a lighting image; create/write a second buffer.
        void pass2_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.img_external = ctx.create_image(
                "external",
                make_image_desc(format::R8G8B8A8_UNORM, {.width = 64, .height = 64, .depth = 1}, image_usage::SAMPLED),
                true);
            state.expected.record_image_read(state.img_external);
            ctx.read_image(state.img_external, image_usage::SAMPLED);

            // Read the overwritten g1
            state.expected.record_image_read(state.img_g1);
            ctx.read_image(state.img_g1, image_usage::SAMPLED);

            state.img_l0 = ctx.create_image(
                "l0",
                make_image_desc(
                    format::R8G8B8A8_UNORM, {.width = 320, .height = 180, .depth = 1}, image_usage::COLOR_ATTACHMENT),
                false);
            state.expected.record_image_write(state.img_l0);
            ctx.write_image(state.img_l0, image_usage::COLOR_ATTACHMENT);

            state.buf_b1 = ctx.create_buffer("b1", make_buffer_desc(1024, buffer_usage::UNIFORM_BUFFER), false);
            state.expected.record_buffer_write(state.buf_b1);
            ctx.write_buffer(state.buf_b1, buffer_usage::UNIFORM_BUFFER);
        }

        // Pass3: read l0 and external; write g0 again; read/write both buffers.
        void pass3_setup(pass_setup_context& ctx)
        {
            auto& state = test_state();

            state.expected.record_image_read(state.img_l0);
            ctx.read_image(state.img_l0, image_usage::SAMPLED);
            state.expected.record_image_read(state.img_external);
            ctx.read_image(state.img_external, image_usage::SAMPLED);

            // Rewrite g0
            state.expected.record_image_write(state.img_g0);
            ctx.write_image(state.img_g0, image_usage::COLOR_ATTACHMENT);

            // Buffers
            state.expected.record_buffer_read(state.buf_b1);
            ctx.read_buffer(state.buf_b1, buffer_usage::UNIFORM_BUFFER);
            state.expected.record_buffer_read(state.buf_b0);
            ctx.read_buffer(state.buf_b0, buffer_usage::STORAGE_BUFFER);

            state.expected.record_buffer_write(state.buf_b1);
            ctx.write_buffer(state.buf_b1, buffer_usage::UNIFORM_BUFFER);
            state.expected.record_buffer_write(state.buf_b0);
            ctx.write_buffer(state.buf_b0, buffer_usage::STORAGE_BUFFER);
        }
    } // namespace

    void resource_generation_compile_test()
    {
        auto& state = test_state();
        state.reset();

        system_t system;
        system.add_pass(pass0_setup, noop_execute);
        system.add_pass(pass1_setup, noop_execute);
        system.add_pass(pass2_setup, noop_execute);
        system.add_pass(pass3_setup, noop_execute);

        const auto compile_result = system.compile();
        RG_CHECK(compile_result.succeeded());

        const auto check_stream = [](const auto& resources,
                                     const auto& versions,
                                     const auto& expected_resources,
                                     const auto& expected_generations)
        {
            RG_CHECK(resources.size() == expected_resources.size());
            RG_CHECK(versions.size() == expected_generations.size());
            for (size_t index = 0; index < resources.size(); index++)
            {
                RG_CHECK(resources[index] == static_cast<resource_handle>(expected_resources[index]));
                if (versions[index] == invalid_resource_version)
                {
                    RG_CHECK(expected_generations[index] == 0);
                    continue;
                }
                RG_CHECK(unpack_to_resource(versions[index]) == resources[index]);
                RG_CHECK(unpack_to_version(versions[index]) == expected_generations[index]);
            }
        };

        const auto& expected = state.expected;
        const auto& image_reads = system_test_access::image_read_dependencies(system);
        const auto& image_writes = system_test_access::image_write_dependencies(system);
        const auto& buffer_reads = system_test_access::buffer_read_dependencies(system);
        const auto& buffer_writes = system_test_access::buffer_write_dependencies(system);
        check_stream(image_reads.read_list,
                     system_test_access::image_read_versions(system),
                     expected.image_read_handles,
                     expected.image_read_gens);
        check_stream(image_writes.write_list,
                     system_test_access::image_write_versions(system),
                     expected.image_write_handles,
                     expected.image_write_gens);
        check_stream(buffer_reads.read_list,
                     system_test_access::buffer_read_versions(system),
                     expected.buffer_read_handles,
                     expected.buffer_read_gens);
        check_stream(buffer_writes.write_list,
                     system_test_access::buffer_write_versions(system),
                     expected.buffer_write_handles,
                     expected.buffer_write_gens);
    }
} // namespace render_graph::unit_test
