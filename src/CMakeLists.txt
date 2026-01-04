add_subdirectory(core)

if (RENDER_GRAPH_BUILD_UNIT_TESTS)
    add_subdirectory(unit_test)
endif()

if (RENDER_GRAPH_BUILD_SAMPLES)
    add_subdirectory(sample)
endif()

if(RENDER_GRAPH_BUILD_INSTALL)
    install(TARGETS render_graph EXPORT render_graphTargets)
    install(EXPORT render_graphTargets
        FILE render_graphTargets.cmake
        NAMESPACE render_graph::
        DESTINATION lib/cmake/render_graph
    )
    install(DIRECTORY ../render_graph
        DESTINATION include
        FILES_MATCHING PATTERN "*.h"
    )
endif()