#pragma once

#if defined(_WIN32) && defined(RENDER_GRAPH_SHARED)
    #if defined(RENDER_GRAPH_CORE_BUILD)
        #define RENDER_GRAPH_CORE_API __declspec(dllexport)
    #else
        #define RENDER_GRAPH_CORE_API __declspec(dllimport)
    #endif
    #if defined(RENDER_GRAPH_VULKAN_BUILD)
        #define RENDER_GRAPH_VULKAN_API __declspec(dllexport)
    #else
        #define RENDER_GRAPH_VULKAN_API __declspec(dllimport)
    #endif
#else
    #define RENDER_GRAPH_CORE_API
    #define RENDER_GRAPH_VULKAN_API
#endif
