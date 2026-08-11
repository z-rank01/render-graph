#pragma once

#include "resource_types.h"

namespace render_graph
{
    enum class metal_storage_mode : uint8_t
    {
        private_memory = 0,
        shared_memory,
        managed_memory,
    };

    enum class metal_pixel_format : uint8_t
    {
        invalid = 0,
        rgba8_unorm,
        rgba8_srgb,
        bgra8_unorm,
        bgra8_srgb,
        depth32_float,
    };

    struct metal_image_lowering
    {
        metal_pixel_format pixel_format = metal_pixel_format::invalid;
        extent_3d extent{};
        uint32_t mip_levels = 1;
        uint32_t array_layers = 1;
        metal_storage_mode storage = metal_storage_mode::private_memory;
        image_usage usage = image_usage::NONE;
    };

    struct metal_buffer_lowering
    {
        uint64_t size = 0;
        metal_storage_mode storage = metal_storage_mode::private_memory;
        buffer_usage usage = buffer_usage::NONE;
        bool persistently_mapped = false;
    };

    [[nodiscard]] inline metal_pixel_format lower_metal_format(format value) noexcept
    {
        switch (value)
        {
        case format::R8G8B8A8_UNORM: return metal_pixel_format::rgba8_unorm;
        case format::R8G8B8A8_SRGB: return metal_pixel_format::rgba8_srgb;
        case format::B8G8R8A8_UNORM: return metal_pixel_format::bgra8_unorm;
        case format::B8G8R8A8_SRGB: return metal_pixel_format::bgra8_srgb;
        case format::D32_SFLOAT: return metal_pixel_format::depth32_float;
        case format::UNDEFINED: return metal_pixel_format::invalid;
        }
        return metal_pixel_format::invalid;
    }

    [[nodiscard]] inline metal_storage_mode lower_metal_storage_mode(memory_domain domain) noexcept
    {
        switch (domain)
        {
        case memory_domain::device_local: return metal_storage_mode::private_memory;
        case memory_domain::upload: return metal_storage_mode::shared_memory;
        case memory_domain::readback: return metal_storage_mode::managed_memory;
        }
        return metal_storage_mode::private_memory;
    }

    [[nodiscard]] inline metal_image_lowering lower_metal_image_desc(const image_desc& desc) noexcept
    {
        return {
            .pixel_format = lower_metal_format(desc.fmt),
            .extent = desc.extent,
            .mip_levels = desc.mip_levels,
            .array_layers = desc.array_layers,
            .storage = lower_metal_storage_mode(desc.memory),
            .usage = desc.usage,
        };
    }

    [[nodiscard]] inline metal_buffer_lowering lower_metal_buffer_desc(const buffer_desc& desc) noexcept
    {
        return {
            .size = desc.size,
            .storage = lower_metal_storage_mode(desc.memory),
            .usage = desc.usage,
            .persistently_mapped = desc.mapping == mapping_policy::persistent,
        };
    }
} // namespace render_graph
