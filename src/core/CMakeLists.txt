add_library(render_graph_core INTERFACE)
add_library(render_graph::core ALIAS render_graph_core)

target_compile_features(render_graph_core INTERFACE cxx_std_20)
target_include_directories(render_graph_core INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(render_graph_core PROPERTIES EXPORT_NAME core)

if(MSVC)
    target_compile_options(render_graph_core INTERFACE /utf-8)
endif()
