#include <array>
#include <exception>
#include <iostream>
#include <string_view>

#include "render_graph/unit_test/barrier_plan_test.h"
#include "render_graph/unit_test/culling_compile_test.h"
#include "render_graph/unit_test/dag_compile_test.h"
#include "render_graph/unit_test/dag_cycle_compile_test.h"
#include "render_graph/unit_test/deferred_rendering_compile_test.h"
#include "render_graph/unit_test/execute_context_test.h"
#include "render_graph/unit_test/frame_lifecycle_test.h"
#include "render_graph/unit_test/hardening_test.h"
#include "render_graph/unit_test/lifetime_aliasing_test.h"
#include "render_graph/unit_test/multi_queue_test.h"
#include "render_graph/unit_test/ordered_subresource_compile_test.h"
#include "render_graph/unit_test/raster_pass_test.h"
#include "render_graph/unit_test/resource_generation_compile_test.h"
#include "render_graph/unit_test/resource_producer_map_compile_test.h"
#include "render_graph/unit_test/repeat_compile_test.h"
#include "render_graph/unit_test/render_device_contract_test.h"
#if RENDER_GRAPH_HAS_LOWERING_CONTRACTS
#include "render_graph/unit_test/resource_description_lowering_test.h"
#endif
#include "render_graph/unit_test/synchronization_plan_test.h"
#include "render_graph/unit_test/validation_compile_test.h"
#if RENDER_GRAPH_HAS_VULKAN
#include "render_graph/unit_test/vulkan_barrier_lowering_test.h"
#include "render_graph/unit_test/vulkan_resource_allocator_test.h"
#include "render_graph/unit_test/vulkan_sample_graph_test.h"
#endif

namespace
{
    struct test_case
    {
        std::string_view name;
        void (*run)();
    };

    constexpr std::array tests{
        test_case{"barrier_plan", &render_graph::unit_test::barrier_plan_test},
        test_case{"culling_compile", &render_graph::unit_test::culling_compile_test},
        test_case{"dag_compile", &render_graph::unit_test::dag_compile_test},
        test_case{"dag_cycle_compile", &render_graph::unit_test::dag_cycle_compile_test},
        test_case{"deferred_rendering_compile", &render_graph::unit_test::deferred_rendering_compile_test},
        test_case{"execute_context", &render_graph::unit_test::execute_context_test},
        test_case{"frame_lifecycle", &render_graph::unit_test::frame_lifecycle_test},
        test_case{"hardening", &render_graph::unit_test::hardening_test},
        test_case{"lifetime_aliasing", &render_graph::unit_test::lifetime_aliasing_test},
        test_case{"multi_queue", &render_graph::unit_test::multi_queue_test},
        test_case{"ordered_subresource_compile", &render_graph::unit_test::ordered_subresource_compile_test},
        test_case{"raster_pass", &render_graph::unit_test::raster_pass_test},
        test_case{"resource_generation_compile", &render_graph::unit_test::resource_generation_compile_test},
        test_case{"resource_producer_map_compile", &render_graph::unit_test::resource_producer_map_compile_test},
        test_case{"repeat_compile", &render_graph::unit_test::repeat_compile_test},
        test_case{"render_device_contract", &render_graph::unit_test::render_device_contract_test},
#if RENDER_GRAPH_HAS_LOWERING_CONTRACTS
        test_case{"resource_description_lowering", &render_graph::unit_test::resource_description_lowering_test},
#endif
        test_case{"synchronization_plan", &render_graph::unit_test::synchronization_plan_test},
        test_case{"validation_compile", &render_graph::unit_test::validation_compile_test},
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
            test.run();
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
