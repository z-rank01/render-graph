if(RENDER_GRAPH_BUILD_SHARED)
    add_library(render_graph_core SHARED)
    target_compile_definitions(render_graph_core
        PUBLIC RENDER_GRAPH_SHARED
        PRIVATE RENDER_GRAPH_CORE_BUILD
    )
else()
    add_library(render_graph_core STATIC)
endif()
add_library(render_graph::core ALIAS render_graph_core)

target_sources(render_graph_core PRIVATE
    graph.cpp
    system.cpp
)

target_compile_features(render_graph_core PUBLIC cxx_std_20)
target_include_directories(render_graph_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(render_graph_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(render_graph_core PROPERTIES EXPORT_NAME core)

if(MSVC)
    target_compile_options(render_graph_core PUBLIC /utf-8)
endif()
