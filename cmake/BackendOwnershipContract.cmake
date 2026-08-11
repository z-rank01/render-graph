if(NOT DEFINED RENDER_GRAPH_SOURCE_DIR)
    message(FATAL_ERROR "RENDER_GRAPH_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE _rg_sources LIST_DIRECTORIES false
    "${RENDER_GRAPH_SOURCE_DIR}/src/*.h"
    "${RENDER_GRAPH_SOURCE_DIR}/src/*.hpp"
    "${RENDER_GRAPH_SOURCE_DIR}/src/*.cpp"
)
foreach(_source IN LISTS _rg_sources)
    file(READ "${_source}" _contents)
    if(_contents MATCHES "#[ \t]*include[^\n]*(engine/|SDL|tiny_gltf|glm/|utility/logger)" OR
       _contents MATCHES "CGLAB_SOURCE_DIR")
        file(RELATIVE_PATH _relative "${RENDER_GRAPH_SOURCE_DIR}" "${_source}")
        message(FATAL_ERROR "Render Graph depends on an application-layer type: ${_relative}")
    endif()
endforeach()

file(GLOB_RECURSE _core_sources LIST_DIRECTORIES false
    "${RENDER_GRAPH_SOURCE_DIR}/src/core/*.h"
    "${RENDER_GRAPH_SOURCE_DIR}/src/core/*.hpp"
    "${RENDER_GRAPH_SOURCE_DIR}/src/core/*.cpp"
)
foreach(_source IN LISTS _core_sources)
    file(READ "${_source}" _contents)
    if(_contents MATCHES "#[ \t]*include[^\n]*(vulkan|vk_mem_alloc|d3d12|dxgi|Metal/)")
        file(RELATIVE_PATH _relative "${RENDER_GRAPH_SOURCE_DIR}" "${_source}")
        message(FATAL_ERROR "Render Graph Core exposes a native graphics API: ${_relative}")
    endif()
endforeach()
