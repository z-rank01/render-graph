set(_rg_lib_type STATIC)
if(RENDER_GRAPH_BUILD_SHARED)
    set(_rg_lib_type SHARED)
endif()

add_library(render_graph_core ${_rg_lib_type}
    backend.h
    barrier.h
    compile_result.h
    execute_result.h
    graph.cpp
    graph.h
    hardening.h
    raster.h
    render_device.h
    resource.h
    resource_types.h
    rg_function.h
    system.h
    submission.h
)
add_library(render_graph::core ALIAS render_graph_core)

target_compile_features(render_graph_core PUBLIC cxx_std_20)
target_include_directories(render_graph_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(render_graph_core PUBLIC /utf-8)
endif()
