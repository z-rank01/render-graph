cmake_minimum_required(VERSION 3.20)
project(render_graph LANGUAGES CXX)

# Build switches
option(RENDER_GRAPH_BUILD_UNIT_TESTS "Build render-graph unit-test object library" OFF)
option(RENDER_GRAPH_BUILD_SAMPLES "Build render-graph sample executables" OFF)

# Backend switches (optional dependencies)
option(RENDER_GRAPH_ENABLE_VULKAN "Enable Vulkan backend/sample support" ON)
option(RENDER_GRAPH_ENABLE_DX12 "Enable DX12 backend/sample support (Windows)" ON)

# Library type
option(RENDER_GRAPH_BUILD_SHARED "Build render_graph as a shared library (DLL)" OFF)

# Optional install/export (kept simple)
option(RENDER_GRAPH_BUILD_INSTALL "Enable install/export rules" OFF)

add_subdirectory(src)
