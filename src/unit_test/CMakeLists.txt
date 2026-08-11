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
    hardening_test.cpp
    resource_producer_map_compile_test.cpp
    repeat_compile_test.cpp
    render_device_contract_test.cpp
    resource_generation_compile_test.cpp
    culling_compile_test.cpp
    validation_compile_test.cpp
    dag_compile_test.cpp
    dag_cycle_compile_test.cpp
    lifetime_aliasing_test.cpp
    multi_queue_test.cpp
    ordered_subresource_compile_test.cpp
    raster_pass_test.cpp
    synchronization_plan_test.cpp
)

target_link_libraries(render_graph_unit_tests
    PRIVATE
        render_graph::core
        $<$<TARGET_EXISTS:render_graph_vulkan_backend>:render_graph::vulkan>
        $<$<TARGET_EXISTS:render_graph_dx12_backend>:render_graph::dx12>
        render_graph::metal
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

target_link_libraries(render_graph_tests PRIVATE
    render_graph::core
    $<$<TARGET_EXISTS:render_graph_vulkan_backend>:render_graph::vulkan>
    $<$<TARGET_EXISTS:render_graph_dx12_backend>:render_graph::dx12>
    render_graph::metal
)

if(TARGET render_graph_vulkan_backend)
    target_compile_definitions(render_graph_tests PRIVATE RENDER_GRAPH_HAS_VULKAN=1)
endif()

if(TARGET render_graph_vulkan_backend AND TARGET render_graph_dx12_backend)
    target_sources(render_graph_unit_tests PRIVATE resource_description_lowering_test.cpp)
    target_compile_definitions(render_graph_tests PRIVATE RENDER_GRAPH_HAS_LOWERING_CONTRACTS=1)
endif()

if(TARGET render_graph_vulkan_backend)
    target_sources(render_graph_unit_tests PRIVATE
        vulkan_barrier_lowering_test.cpp
        vulkan_resource_allocator_test.cpp
        vulkan_sample_graph_test.cpp
    )
endif()

add_test(
    NAME render_graph.backend_ownership
    COMMAND ${CMAKE_COMMAND}
        -DRENDER_GRAPH_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/cmake/BackendOwnershipContract.cmake
)

set(_render_graph_test_cases
    barrier_plan
    culling_compile
    dag_compile
    dag_cycle_compile
    deferred_rendering_compile
    execute_context
    frame_lifecycle
    hardening
    lifetime_aliasing
    multi_queue
    ordered_subresource_compile
    raster_pass
    resource_generation_compile
    resource_producer_map_compile
    repeat_compile
    render_device_contract
    synchronization_plan
    validation_compile
)

if(TARGET render_graph_vulkan_backend AND TARGET render_graph_dx12_backend)
    list(APPEND _render_graph_test_cases resource_description_lowering)
endif()

if(TARGET render_graph_vulkan_backend)
    list(APPEND _render_graph_test_cases
        vulkan_barrier_lowering
        vulkan_resource_allocator
        vulkan_sample_graph
    )
endif()

foreach(_test_case IN LISTS _render_graph_test_cases)
    add_test(
        NAME render_graph.${_test_case}
        COMMAND render_graph_tests ${_test_case}
    )
endforeach()
