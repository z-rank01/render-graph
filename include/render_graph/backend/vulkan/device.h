#pragma once

// Vulkan backend of render_device: device creation configuration, creation
// result, and the factory entry point.

#include <string>

#include "render_graph/render_device.h"
#include "render_graph/diagnostic.h"
#include "render_graph/visibility.h"
#include "render_graph/backend/vulkan/surface_provider.h"

namespace render_graph::vulkan
{
    // --- Device creation ---

    struct device_config
    {
        std::string application_name = "RenderGraph";
        uint32_t frames_in_flight = 3;
        bool validation = false;
        surface_provider surface;
        diagnostic_sink diagnostics;
    };

    // Success iff no error was reported and the device handle is valid.
    struct device_create_result
    {
        render_device device;
        std::string error;
        [[nodiscard]] explicit operator bool() const noexcept { return error.empty() && static_cast<bool>(device); }
    };

    // Factory entry point; failures are reported through device_create_result::error.
    [[nodiscard]] RENDER_GRAPH_VULKAN_API device_create_result create_device(const device_config&);
} // namespace render_graph::vulkan
