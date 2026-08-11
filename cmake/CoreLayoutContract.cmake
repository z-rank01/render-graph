if(NOT DEFINED RENDER_GRAPH_SOURCE_DIR)
    message(FATAL_ERROR "RENDER_GRAPH_SOURCE_DIR is required")
endif()

foreach(_required IN ITEMS
    src/core/system.h
    src/core/system.cpp
    src/core/graph.h
    src/core/graph.cpp)
    if(NOT EXISTS "${RENDER_GRAPH_SOURCE_DIR}/${_required}")
        message(FATAL_ERROR "Render Graph Core implementation file is missing: ${_required}")
    endif()
    file(SIZE "${RENDER_GRAPH_SOURCE_DIR}/${_required}" _size)
    if(_size EQUAL 0)
        message(FATAL_ERROR "Render Graph Core implementation file is empty: ${_required}")
    endif()
endforeach()

foreach(_removed IN ITEMS
    include/render_graph/system.h
    include/render_graph/backend.h
    include/render_graph/graph.h
    include/render_graph/rg_function.h
    include/render_graph/execute_result.h)
    if(EXISTS "${RENDER_GRAPH_SOURCE_DIR}/${_removed}")
        message(FATAL_ERROR "Legacy public Core implementation header still exists: ${_removed}")
    endif()
endforeach()

file(GLOB_RECURSE _public_headers LIST_DIRECTORIES false
    "${RENDER_GRAPH_SOURCE_DIR}/include/render_graph/*.h")
foreach(_header IN LISTS _public_headers)
    file(READ "${_header}" _contents)
    file(SIZE "${_header}" _size)
    if(_size GREATER 32768)
        file(RELATIVE_PATH _relative "${RENDER_GRAPH_SOURCE_DIR}" "${_header}")
        message(FATAL_ERROR "Public header is large enough to indicate leaked implementation: ${_relative}")
    endif()
    if(_contents MATCHES "render_graph_system[ \\t]*<" OR
       _contents MATCHES "struct[ \\t]+compiler_state" OR
       _contents MATCHES "struct[ \\t]+dependency_graph" OR
       _contents MATCHES "#[ \\t]*include[ \\t]*\"[^\"]*src/")
        file(RELATIVE_PATH _relative "${RENDER_GRAPH_SOURCE_DIR}" "${_header}")
        message(FATAL_ERROR "Public header exposes Core implementation: ${_relative}")
    endif()
endforeach()


file(GLOB_RECURSE _source_tree_forwarders LIST_DIRECTORIES false
    "${RENDER_GRAPH_SOURCE_DIR}/render_graph/*.h")
if(_source_tree_forwarders)
    message(FATAL_ERROR "Legacy source-tree forwarding headers still exist: ${_source_tree_forwarders}")
endif()

file(GLOB_RECURSE _core_sources LIST_DIRECTORIES false
    "${RENDER_GRAPH_SOURCE_DIR}/src/core/*.h"
    "${RENDER_GRAPH_SOURCE_DIR}/src/core/*.cpp")
foreach(_source IN LISTS _core_sources)
    file(READ "${_source}" _contents)
    if(_contents MATCHES "#[ \\t]*include[^\\n]*(vulkan|vk_mem_alloc|d3d12|dxgi|Metal/)")
        file(RELATIVE_PATH _relative "${RENDER_GRAPH_SOURCE_DIR}" "${_source}")
        message(FATAL_ERROR "Render Graph Core implementation depends on a native API: ${_relative}")
    endif()
endforeach()
