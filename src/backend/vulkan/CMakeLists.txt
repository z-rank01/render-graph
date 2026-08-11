set(_rg_lib_type STATIC)
if(RENDER_GRAPH_BUILD_SHARED)
    set(_rg_lib_type SHARED)
endif()

add_library(render_graph_vulkan_backend ${_rg_lib_type}
    vk_backend.h
    vk_bindless.cpp
    vk_barrier_lowering.h
    vk_resource_allocator.h
    vk_resource_lowering.h
    vk_pipeline_store.cpp
    vk_scene_recorder.cpp
    vk_vma.cpp
    vk_resource_store.cpp
    vk_runtime.cpp
    vk_runtime.h
    vulkan_device.cpp
)
add_library(render_graph::vulkan ALIAS render_graph_vulkan_backend)

find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

target_compile_features(render_graph_vulkan_backend PUBLIC cxx_std_20)
target_include_directories(render_graph_vulkan_backend PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(render_graph_vulkan_backend PUBLIC
    render_graph::core
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
)
set_target_properties(render_graph_vulkan_backend PROPERTIES EXPORT_NAME vulkan)

if(MSVC)
    target_compile_options(render_graph_vulkan_backend PUBLIC /utf-8)
endif()
