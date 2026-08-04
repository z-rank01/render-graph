if (NOT RENDER_GRAPH_BUILD_UNIT_TESTS)
    return()
endif()

# Unit-test sources for render_graph.
# These tests are compiled into the main executable (via OBJECT library objects)
# so they can be embedded into a parent executable.

add_library(render_graph_unit_tests OBJECT
    barrier_plan_test.cpp
    deferred_rendering_compile_test.cpp
    execute_context_test.cpp
    frame_lifecycle_test.cpp
    resource_producer_map_compile_test.cpp
    repeat_compile_test.cpp
    resource_generation_compile_test.cpp
    culling_compile_test.cpp
    validation_compile_test.cpp
    vulkan_barrier_lowering_test.cpp
    vulkan_resource_allocator_test.cpp
    vulkan_sample_graph_test.cpp
    dag_compile_test.cpp
    dag_cycle_compile_test.cpp
    lifetime_aliasing_test.cpp
    ordered_subresource_compile_test.cpp
    raster_pass_test.cpp
    synchronization_plan_test.cpp
)

target_link_libraries(render_graph_unit_tests
    PRIVATE
        render_graph
)

# Ensure includes like "render_graph/system.h" resolve.
# render_graph already exports ${CMAKE_CURRENT_SOURCE_DIR}/.. (i.e., src/),
# but we keep this explicit for the unit-test target.
target_include_directories(render_graph_unit_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

add_executable(render_graph_tests
    unit_test_main.cpp
    $<TARGET_OBJECTS:render_graph_unit_tests>
)

target_link_libraries(render_graph_tests PRIVATE render_graph)

set(_render_graph_test_cases
    barrier_plan
    culling_compile
    dag_compile
    dag_cycle_compile
    deferred_rendering_compile
    execute_context
    frame_lifecycle
    lifetime_aliasing
    ordered_subresource_compile
    raster_pass
    resource_generation_compile
    resource_producer_map_compile
    repeat_compile
    synchronization_plan
    validation_compile
    vulkan_barrier_lowering
    vulkan_resource_allocator
    vulkan_sample_graph
)

foreach(_test_case IN LISTS _render_graph_test_cases)
    add_test(
        NAME render_graph.${_test_case}
        COMMAND render_graph_tests ${_test_case}
    )
endforeach()
