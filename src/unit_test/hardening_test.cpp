#include "render_graph/unit_test/hardening_test.h"

#include <random>
#include <string>
#include <vector>

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

        void noop(execute_context&) { }

        test_buffer_desc buffer_desc()
        {
            return test_buffer_desc{.size = 4096, .usage = buffer_usage::STORAGE_BUFFER | buffer_usage::TRANSFER_DST};
        }

        bool has_code(const compile_result& result, compile_error_code code)
        {
            for (const auto& diagnostic : result.diagnostics)
            {
                if (diagnostic.code == code)
                {
                    return true;
                }
            }
            return false;
        }

        struct failing_backend : test_backend
        {
            template <typename MetaTableT>
            void on_compile_resource_allocation(const MetaTableT&, const physical_resource_meta&)
            {
                last_error = "allocator reported out of memory";
            }

            void clear_error() { last_error.clear(); }
            const std::string& get_last_error() const { return last_error; }
            std::string last_error;
        };

        struct unsupported_backend : failing_backend
        {
            compile_error_code get_compile_error_code() const
            {
                return compile_error_code::unsupported_feature;
            }
        };

        template <typename SystemT>
        void add_single_output(SystemT& system)
        {
            system.add_pass("output", [](auto& ctx)
            {
                const auto output = ctx.create_buffer("output", buffer_desc());
                ctx.write_buffer(output, buffer_usage::STORAGE_BUFFER);
                ctx.declare_buffer_output(output);
            }, [](auto&) { });
        }

        void check_limits()
        {
            {
                system_t system;
                system.set_limits(render_graph_limits{.max_passes = 0});
                add_single_output(system);
                RG_CHECK(has_code(system.compile(), compile_error_code::pass_limit_exceeded));
            }
            {
                system_t system;
                system.set_limits(render_graph_limits{.max_images = 0});
                system.add_pass("image", [](setup_context& ctx)
                {
                    const auto image = ctx.create_image("image", test_image_desc{.fmt = format::R8G8B8A8_UNORM,
                        .usage = image_usage::STORAGE});
                    ctx.write_image(image, image_usage::STORAGE);
                    ctx.declare_image_output(image);
                }, noop);
                RG_CHECK(has_code(system.compile(), compile_error_code::image_limit_exceeded));
            }
            {
                system_t system;
                system.set_limits(render_graph_limits{.max_buffers = 0});
                add_single_output(system);
                RG_CHECK(has_code(system.compile(), compile_error_code::buffer_limit_exceeded));
            }
            {
                system_t system;
                system.set_limits(render_graph_limits{.max_access_events = 0});
                add_single_output(system);
                RG_CHECK(has_code(system.compile(), compile_error_code::access_limit_exceeded));
            }
        }

        void check_backend_failure()
        {
            render_graph_system<failing_backend> system;
            add_single_output(system);
            const auto result = system.compile();
            RG_CHECK(has_code(result, compile_error_code::backend_failure));
            RG_CHECK(!result.diagnostics.front().message.empty());

            render_graph_system<unsupported_backend> unsupported;
            add_single_output(unsupported);
            RG_CHECK(has_code(unsupported.compile(), compile_error_code::unsupported_feature));
        }

        void check_dump_and_stress()
        {
            constexpr uint32_t pass_count = 96;
            system_t system;
            std::vector<buffer_handle> chain(pass_count);
            image_handle imported = invalid_image;
            std::mt19937 random(0xC0FFEEu);

            for (uint32_t index = 0; index < pass_count; ++index)
            {
                const uint32_t extra_dependency = index == 0 ? 0 : random() % index;
                const uint32_t mip = random() % 4;
                const uint32_t layer = random() % 3;
                system.add_pass("stress_" + std::to_string(index),
                    [&, index, extra_dependency, mip, layer](setup_context& ctx)
                    {
                        if (index == 0)
                        {
                            imported = ctx.import_image("history", test_image_desc{
                                .fmt = format::R8G8B8A8_UNORM,
                                .usage = image_usage::SAMPLED,
                                .mip_levels = 4,
                                .array_layers = 3,
                            }, resource_lifetime_class::history);
                        }
                        ctx.read_image(imported, image_access_desc{
                            .usage = image_usage::SAMPLED,
                            .domain = pipeline_domain::compute,
                            .subresource = image_subresource_range{
                                .aspects = image_aspect::color,
                                .base_mip_level = mip,
                                .mip_level_count = 1,
                                .base_array_layer = layer,
                                .array_layer_count = 1,
                            },
                        });
                        if (index != 0)
                        {
                            ctx.read_buffer(chain[index - 1], buffer_usage::STORAGE_BUFFER);
                            if (extra_dependency != index - 1)
                            {
                                ctx.read_buffer(chain[extra_dependency], buffer_usage::STORAGE_BUFFER);
                            }
                        }
                        chain[index] = ctx.create_buffer("chain_" + std::to_string(index), buffer_desc());
                        ctx.write_buffer(chain[index], buffer_usage::STORAGE_BUFFER);
                        if (index + 1 == pass_count)
                        {
                            ctx.declare_buffer_output(chain[index]);
                        }
                    }, noop);
            }

            RG_CHECK(system.compile().succeeded());
            const auto first_dump = system.debug_dump();
            const auto& statistics = system.get_statistics();
            RG_CHECK(statistics.pass_count == pass_count);
            RG_CHECK(statistics.active_pass_count == pass_count);
            RG_CHECK(statistics.buffer_count == pass_count);
            RG_CHECK(statistics.image_count == 1);
            RG_CHECK(statistics.access_event_count >= pass_count * 3 - 1);
            RG_CHECK(first_dump.find("schedule") != std::string::npos);
            RG_CHECK(first_dump.find("stress_95") != std::string::npos);
            RG_CHECK(first_dump.find("history") != std::string::npos);
            RG_CHECK(first_dump.find("synchronization") != std::string::npos);
            RG_CHECK(first_dump.find("submission") != std::string::npos);

            RG_CHECK(system.compile().succeeded());
            RG_CHECK(system.debug_dump() == first_dump);
        }
    }

    void hardening_test()
    {
        check_limits();
        check_backend_failure();
        check_dump_and_stress();
    }
}
