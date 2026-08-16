// Lowering of backend-agnostic resource descriptors (image_desc / buffer_desc
// / sampler_desc) into the Metal-specific forms used by the Metal backend.
#pragma once

#include "render_graph/render_device.h"
#include "render_graph/resource_types.h"

namespace render_graph
{
    // =============================================================================
    // Metal resource types
    // =============================================================================

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

    enum class metal_filter : uint8_t { nearest, linear };
    enum class metal_address_mode : uint8_t { repeat, mirrored_repeat, clamp_to_edge };
    // R2 契约同步：与 sampler_compare_op 一一对应（never = 无比较）
    enum class metal_compare_function : uint8_t
    {
        never,
        less,
        equal,
        less_equal,
        greater,
        not_equal,
        greater_equal,
        always,
    };

    // =============================================================================
    // Lowered resource descriptors
    // =============================================================================

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

    struct metal_sampler_lowering
    {
        metal_filter min_filter = metal_filter::linear;
        metal_filter mag_filter = metal_filter::linear;
        metal_address_mode address_u = metal_address_mode::repeat;
        metal_address_mode address_v = metal_address_mode::repeat;
        metal_compare_function compare_function = metal_compare_function::never;
        float max_lod = 0.0F;
    };

    // =============================================================================
    // Lowering functions
    // =============================================================================

    // --- Attribute mapping ---

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

    [[nodiscard]] inline metal_compare_function lower_metal_compare_function(sampler_compare_op value) noexcept
    {
        switch (value)
        {
        case sampler_compare_op::never: return metal_compare_function::never;
        case sampler_compare_op::less: return metal_compare_function::less;
        case sampler_compare_op::equal: return metal_compare_function::equal;
        case sampler_compare_op::less_or_equal: return metal_compare_function::less_equal;
        case sampler_compare_op::greater: return metal_compare_function::greater;
        case sampler_compare_op::not_equal: return metal_compare_function::not_equal;
        case sampler_compare_op::greater_or_equal: return metal_compare_function::greater_equal;
        case sampler_compare_op::always: return metal_compare_function::always;
        }
        return metal_compare_function::never;
    }

    [[nodiscard]] inline metal_sampler_lowering lower_metal_sampler_desc(const sampler_desc& desc) noexcept
    {
        return {
            .min_filter = desc.min_filter == sampler_filter::nearest ? metal_filter::nearest : metal_filter::linear,
            .mag_filter = desc.mag_filter == sampler_filter::nearest ? metal_filter::nearest : metal_filter::linear,
            .address_u = desc.address_u == sampler_address_mode::clamp_to_edge ? metal_address_mode::clamp_to_edge
                       : desc.address_u == sampler_address_mode::mirrored_repeat ? metal_address_mode::mirrored_repeat
                                                                                : metal_address_mode::repeat,
            .address_v = desc.address_v == sampler_address_mode::clamp_to_edge ? metal_address_mode::clamp_to_edge
                       : desc.address_v == sampler_address_mode::mirrored_repeat ? metal_address_mode::mirrored_repeat
                                                                                : metal_address_mode::repeat,
            .compare_function = lower_metal_compare_function(desc.compare_op),
            .max_lod = desc.max_lod,
        };
    }

    // --- Descriptor lowering ---

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
