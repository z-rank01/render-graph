#pragma once

#include <compare>
#include <cstdint>
#include <string>

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

    enum class memory_domain : uint8_t
    {
        device_local = 0,
        upload,
        readback,
    };

    enum class mapping_policy : uint8_t
    {
        none = 0,
        persistent,
    };

    enum class allocation_policy : uint8_t
    {
        automatic = 0,
        dedicated,
    };

    enum class aliasing_policy : uint8_t
    {
        automatic = 0,
        forbidden,
    };

    enum class resource_lifetime_class : uint8_t
    {
        transient = 0,
        imported,
        persistent,
        history,
    };

    enum class sample_count : uint8_t
    {
        x1 = 1,
        x2 = 2,
        x4 = 4,
        x8 = 8,
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
        PRESENT                  = 1 << 6,
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

    inline buffer_usage operator&(buffer_usage a, buffer_usage b)
    {
        return static_cast<buffer_usage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    struct extent_3d
    {
        uint32_t width  = 1;
        uint32_t height = 1;
        uint32_t depth  = 1;

        [[nodiscard]] constexpr auto operator<=>(const extent_3d&) const noexcept = default;
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

    inline image_flags operator&(image_flags a, image_flags b)
    {
        return static_cast<image_flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    struct image_desc
    {
        format fmt                   = format::UNDEFINED;
        extent_3d extent{};
        image_usage usage            = image_usage::NONE;
        image_type type              = image_type::TYPE_2D;
        image_flags flags            = image_flags::NONE;
        uint32_t mip_levels          = 1;
        uint32_t array_layers        = 1;
        sample_count samples         = sample_count::x1;
        memory_domain memory         = memory_domain::device_local;
        mapping_policy mapping       = mapping_policy::none;
        allocation_policy allocation = allocation_policy::automatic;
        aliasing_policy aliasing     = aliasing_policy::automatic;
        resource_lifetime_class lifetime = resource_lifetime_class::transient;

        [[nodiscard]] constexpr auto operator<=>(const image_desc&) const noexcept = default;
    };

    struct buffer_desc
    {
        uint64_t size                = 0;
        buffer_usage usage           = buffer_usage::NONE;
        memory_domain memory         = memory_domain::device_local;
        mapping_policy mapping       = mapping_policy::none;
        allocation_policy allocation = allocation_policy::automatic;
        aliasing_policy aliasing     = aliasing_policy::automatic;
        resource_lifetime_class lifetime = resource_lifetime_class::transient;

        [[nodiscard]] constexpr auto operator<=>(const buffer_desc&) const noexcept = default;
    };

    struct backend_capabilities
    {
        bool supports_device_local = true;
        bool supports_upload = true;
        bool supports_readback = true;
        bool supports_persistent_mapping = true;
        uint32_t max_image_dimension = 16384;
        sample_count max_samples = sample_count::x8;
    };

    struct resource_desc_diagnostic
    {
        bool supported = true;
        std::string message;

        [[nodiscard]] explicit operator bool() const noexcept { return supported; }
    };
} // namespace render_graph
