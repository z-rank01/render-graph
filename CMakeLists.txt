cmake_minimum_required(VERSION 3.20)
project(render_graph VERSION 1.0.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)

include(CTest)

# Build switches
option(RENDER_GRAPH_BUILD_UNIT_TESTS "Build render-graph unit-test object library" OFF)
option(RENDER_GRAPH_BUILD_SAMPLES "Build render-graph sample executables" OFF)

# Backend switches (optional dependencies)
option(RENDER_GRAPH_ENABLE_VULKAN "Enable Vulkan backend/sample support" ON)
option(RENDER_GRAPH_ENABLE_DX12 "Enable DX12 backend/sample support (Windows)" ON)

# Library type
option(RENDER_GRAPH_BUILD_SHARED "Build render_graph as a shared library (DLL)" OFF)

option(RENDER_GRAPH_BUILD_INSTALL "Enable install/export rules" OFF)

add_subdirectory(src)

if(RENDER_GRAPH_BUILD_INSTALL)
    set(_render_graph_install_targets render_graph_core)
    if(TARGET render_graph_vulkan_backend)
        list(APPEND _render_graph_install_targets render_graph_vulkan_backend)
    endif()
    install(TARGETS ${_render_graph_install_targets}
        EXPORT render_graphTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )
    install(EXPORT render_graphTargets
        FILE render_graphTargets.cmake
        NAMESPACE render_graph::
        DESTINATION lib/cmake/render_graph
    )
    install(DIRECTORY include/render_graph DESTINATION include FILES_MATCHING PATTERN "*.h")
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/render_graphConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/render_graphConfig.cmake
        INSTALL_DESTINATION lib/cmake/render_graph
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/render_graphConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/render_graphConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/render_graphConfigVersion.cmake
        DESTINATION lib/cmake/render_graph
    )
endif()
