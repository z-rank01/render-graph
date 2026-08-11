if (NOT RENDER_GRAPH_BUILD_SAMPLES)
    return()
endif()

# Vulkan sample
if (RENDER_GRAPH_ENABLE_VULKAN)
    add_executable(vulkan_render_graph_sample
        compiler_sample.cpp
    )

    target_link_libraries(vulkan_render_graph_sample PRIVATE render_graph::core render_graph::vulkan)
    target_compile_definitions(vulkan_render_graph_sample PRIVATE
        RENDER_GRAPH_SAMPLE_LABEL="vulkan_render_graph_sample")

    if (WIN32)
        target_compile_definitions(vulkan_render_graph_sample PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
        target_link_libraries(vulkan_render_graph_sample PRIVATE user32)
    endif()
endif()

# DX12 sample
if (WIN32 AND RENDER_GRAPH_ENABLE_DX12)
    add_executable(dx12_render_graph_sample
        compiler_sample.cpp
    )

    target_link_libraries(dx12_render_graph_sample PRIVATE render_graph::core render_graph::dx12 dxguid user32)
    target_compile_definitions(dx12_render_graph_sample PRIVATE NOMINMAX
        RENDER_GRAPH_SAMPLE_LABEL="dx12_render_graph_sample")
endif()

