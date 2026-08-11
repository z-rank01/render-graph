add_subdirectory(core)

if(RENDER_GRAPH_ENABLE_VULKAN)
    add_subdirectory(backend/vulkan)
endif()

if(WIN32 AND RENDER_GRAPH_ENABLE_DX12)
    add_subdirectory(backend/dx12)
endif()

add_subdirectory(backend/metal)

if(RENDER_GRAPH_BUILD_UNIT_TESTS)
    add_subdirectory(unit_test)
endif()

if(RENDER_GRAPH_BUILD_SAMPLES)
    add_subdirectory(sample)
endif()
