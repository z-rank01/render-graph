#include <array>
#include <exception>
#include <iostream>
#include <string_view>

#include "render_graph/unit_test/barrier_plan_test.h"
#include "render_graph/unit_test/culling_compile_test.h"
#include "render_graph/unit_test/dag_compile_test.h"
#include "render_graph/unit_test/dag_cycle_compile_test.h"
#include "render_graph/unit_test/deferred_rendering_compile_test.h"
#include "render_graph/unit_test/lifetime_aliasing_test.h"
#include "render_graph/unit_test/ordered_subresource_compile_test.h"
#include "render_graph/unit_test/resource_generation_compile_test.h"
#include "render_graph/unit_test/resource_producer_map_compile_test.h"
#include "render_graph/unit_test/repeat_compile_test.h"
#include "render_graph/unit_test/validation_compile_test.h"

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
        test_case{"lifetime_aliasing", &render_graph::unit_test::lifetime_aliasing_test},
        test_case{"ordered_subresource_compile", &render_graph::unit_test::ordered_subresource_compile_test},
        test_case{"resource_generation_compile", &render_graph::unit_test::resource_generation_compile_test},
        test_case{"resource_producer_map_compile", &render_graph::unit_test::resource_producer_map_compile_test},
        test_case{"repeat_compile", &render_graph::unit_test::repeat_compile_test},
        test_case{"validation_compile", &render_graph::unit_test::validation_compile_test},
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
