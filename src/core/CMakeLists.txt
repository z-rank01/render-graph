set(_rg_lib_type STATIC)
if (RENDER_GRAPH_BUILD_SHARED)
    set(_rg_lib_type SHARED)
endif()

add_library(render_graph ${_rg_lib_type}
    backend.h
    barrier.h
    compile_result.h
    execute_result.h
    graph.cpp
    graph.h
    resource.h
    resource_types.h
    system.h
    dx12_backend.h
    vk_backend.h
    vk_barrier_lowering.h
    vk_resource_allocator.h
)

target_compile_features(render_graph PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(render_graph PUBLIC /utf-8)
endif()

# Expose public headers as: #include "render_graph/*.h"
# The repo root is the include root.
target_include_directories(render_graph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

if (RENDER_GRAPH_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG QUIET)
    target_link_libraries(render_graph PUBLIC Vulkan::Vulkan)
    if (TARGET GPUOpen::VulkanMemoryAllocator)
        target_link_libraries(render_graph PUBLIC GPUOpen::VulkanMemoryAllocator)
    endif()
endif()

if (WIN32 AND RENDER_GRAPH_ENABLE_DX12)
    target_link_libraries(render_graph PUBLIC d3d12 dxgi)
endif()
