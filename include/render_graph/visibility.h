// Symbol visibility macros for shared-library builds. RENDER_GRAPH_CORE_API
// and RENDER_GRAPH_VULKAN_API decorate the core and Vulkan-backend public
// APIs respectively when building/linking the render graph as a DLL.
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
    // Static or non-Windows builds: both macros expand to nothing.
    #define RENDER_GRAPH_CORE_API
    #define RENDER_GRAPH_VULKAN_API
#endif
