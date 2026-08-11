add_library(render_graph_dx12_backend INTERFACE)
add_library(render_graph::dx12 ALIAS render_graph_dx12_backend)
target_include_directories(render_graph_dx12_backend INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(render_graph_dx12_backend INTERFACE render_graph::core d3d12 dxgi)
