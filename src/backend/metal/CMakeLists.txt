add_library(render_graph_metal_contract INTERFACE)
add_library(render_graph::metal ALIAS render_graph_metal_contract)
target_include_directories(render_graph_metal_contract INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(render_graph_metal_contract INTERFACE render_graph::core)
