#pragma once

#include <cstdint>

namespace render_graph
{
    enum class format : uint32_t
    {
        UNDEFINED = 0,
        R8G8B8A8_UNORM,
        R8G8B8A8_SRGB,
        B8G8R8A8_UNORM,
        B8G8R8A8_SRGB,
        D32_SFLOAT,
        // ... add others as needed, mapping to VkFormat/DXGI_FORMAT
    };

    enum class image_usage : uint32_t
    {
        NONE                     = 0,
        TRANSFER_SRC             = 1 << 0,
        TRANSFER_DST             = 1 << 1,
        SAMPLED                  = 1 << 2,
        STORAGE                  = 1 << 3,
        COLOR_ATTACHMENT         = 1 << 4,
        DEPTH_STENCIL_ATTACHMENT = 1 << 5,
        // ...
    };

    inline image_usage operator|(image_usage a, image_usage b)
    {
        return static_cast<image_usage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline image_usage operator&(image_usage a, image_usage b)
    {
        return static_cast<image_usage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    enum class buffer_usage : uint32_t
    {
        NONE            = 0,
        TRANSFER_SRC    = 1 << 0,
        TRANSFER_DST    = 1 << 1,
        UNIFORM_BUFFER  = 1 << 2,
        STORAGE_BUFFER  = 1 << 3,
        INDEX_BUFFER    = 1 << 4,
        VERTEX_BUFFER   = 1 << 5,
        INDIRECT_BUFFER = 1 << 6,
        // ...
    };

    inline buffer_usage operator|(buffer_usage a, buffer_usage b)
    {
        return static_cast<buffer_usage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    struct extent_3d
    {
        uint32_t width;
        uint32_t height;
        uint32_t depth;
    };

    enum class image_type : uint32_t
    {
        TYPE_1D = 0,
        TYPE_2D,
        TYPE_3D
    };

    enum class image_flags : uint32_t
    {
        NONE = 0,
        CUBE_COMPATIBLE = 1 << 0,
        MUTABLE_FORMAT = 1 << 1
    };

    inline image_flags operator|(image_flags a, image_flags b)
    {
        return static_cast<image_flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
} // namespace render_graph
