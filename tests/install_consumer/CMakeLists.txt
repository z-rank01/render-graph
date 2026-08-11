cmake_minimum_required(VERSION 3.20)
project(render_graph_install_consumer LANGUAGES CXX)

find_package(render_graph CONFIG REQUIRED)

add_executable(render_graph_install_consumer main.cpp)
target_link_libraries(render_graph_install_consumer PRIVATE render_graph::core)

if(TARGET render_graph::vulkan)
    target_compile_definitions(render_graph_install_consumer PRIVATE RENDER_GRAPH_HAS_VULKAN=1)
    target_link_libraries(render_graph_install_consumer PRIVATE render_graph::vulkan)
endif()
