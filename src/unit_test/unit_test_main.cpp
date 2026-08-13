// Unit test runner: resolves the single requested test name against a static
// table. Core compiler tests are routed through compiler_contract_test();
// backend and device tests are registered here directly. Unknown names exit
// with code 2, check failures print [FAIL] and exit 1.
#include <array>
#include <exception>
#include <iostream>
#include <string_view>

#include "compile_benchmark.h"
#include "compiler_contract_test.h"
#include "render_device_contract_test.h"
#if RENDER_GRAPH_HAS_LOWERING_CONTRACTS
#include "resource_description_lowering_test.h"
#endif
#if RENDER_GRAPH_HAS_VULKAN
#include "vulkan_barrier_lowering_test.h"
#include "vulkan_resource_allocator_test.h"
#include "vulkan_sample_graph_test.h"
#endif

namespace
{
    struct test_case
    {
        std::string_view name;
        void (*run)();
    };

    // All known test names; nullptr runs route into compiler_contract_test().
    constexpr std::array tests{
        test_case{"compile_benchmark", &render_graph::unit_test::compile_benchmark_test},
        test_case{"barrier_plan", nullptr},
        test_case{"culling_compile", nullptr},
        test_case{"dag_compile", nullptr},
        test_case{"dag_cycle_compile", nullptr},
        test_case{"deferred_rendering_compile", nullptr},
        test_case{"execute_context", nullptr},
        test_case{"frame_lifecycle", nullptr},
        test_case{"hardening", nullptr},
        test_case{"lifetime_aliasing", nullptr},
        test_case{"multi_queue", nullptr},
        test_case{"ordered_subresource_compile", nullptr},
        test_case{"raster_pass", nullptr},
        test_case{"resource_generation_compile", nullptr},
        test_case{"resource_producer_map_compile", nullptr},
        test_case{"repeat_compile", nullptr},
        test_case{"render_device_contract", &render_graph::unit_test::render_device_contract_test},
#if RENDER_GRAPH_HAS_LOWERING_CONTRACTS
        test_case{"resource_description_lowering", &render_graph::unit_test::resource_description_lowering_test},
#endif
        test_case{"synchronization_plan", nullptr},
        test_case{"validation_compile", nullptr},
#if RENDER_GRAPH_HAS_VULKAN
        test_case{"vulkan_barrier_lowering", &render_graph::unit_test::vulkan_barrier_lowering_test},
        test_case{"vulkan_resource_allocator", &render_graph::unit_test::vulkan_resource_allocator_test},
        test_case{"vulkan_sample_graph", &render_graph::unit_test::vulkan_sample_graph_test},
#endif
    };
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: render_graph_tests <test-name>\n";
        return 2;
    }

    const std::string_view requested = argv[1];
    for (const auto& test : tests)
    {
        if (test.name != requested)
        {
            continue;
        }

        try
        {
            if (test.run) test.run();
            else render_graph::unit_test::compiler_contract_test(test.name);
            std::cout << "[PASS] " << test.name << '\n';
            return 0;
        }
        catch (const std::exception& error)
        {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        }
        catch (...)
        {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            return 1;
        }
    }

    std::cerr << "unknown test: " << requested << '\n';
    return 2;
}
