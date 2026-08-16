#pragma once

// Bidirectional translation between render_graph's backend-neutral resource
// descriptions (format, usage, image/buffer desc, memory_domain) and the
// concrete DXGI/D3D12 types consumed by the DX12 backend.

#if !defined(_WIN32)
    #error "dx12_resource_lowering requires Windows (_WIN32)"
#endif

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#include <d3d12.h>
#include <dxgi1_6.h>

#include "render_graph/render_device.h"
#include "render_graph/resource_types.h"

namespace render_graph
{
    // =============================================================================
    // Format translation
    // =============================================================================

    // --- lower: render_graph -> D3D12 ---
    [[nodiscard]] inline DXGI_FORMAT lower_dx12_format(format value) noexcept
    {
        switch (value)
        {
        case format::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case format::R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case format::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case format::B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case format::D32_SFLOAT: return DXGI_FORMAT_D32_FLOAT;
        case format::UNDEFINED: return DXGI_FORMAT_UNKNOWN;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    // --- normalize: D3D12 -> render_graph ---
    [[nodiscard]] inline format normalize_dx12_format(DXGI_FORMAT value) noexcept
    {
        switch (value)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return format::R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return format::R8G8B8A8_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return format::B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return format::B8G8R8A8_SRGB;
        case DXGI_FORMAT_D32_FLOAT: return format::D32_SFLOAT;
        default: return format::UNDEFINED;
        }
    }

    // =============================================================================
    // Resource descriptor translation
    // =============================================================================

    [[nodiscard]] inline D3D12_RESOURCE_FLAGS lower_dx12_image_flags(image_usage usage) noexcept
    {
        D3D12_RESOURCE_FLAGS result = D3D12_RESOURCE_FLAG_NONE;
        if ((usage & image_usage::COLOR_ATTACHMENT) != image_usage::NONE)
            result |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if ((usage & image_usage::DEPTH_STENCIL_ATTACHMENT) != image_usage::NONE)
            result |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if ((usage & image_usage::STORAGE) != image_usage::NONE)
            result |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return result;
    }

    // --- normalize: D3D12 -> render_graph ---
    [[nodiscard]] inline image_desc normalize_dx12_image_desc(const D3D12_RESOURCE_DESC& desc) noexcept
    {
        image_usage usage = image_usage::NONE;
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) usage = usage | image_usage::COLOR_ATTACHMENT;
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
            usage = usage | image_usage::DEPTH_STENCIL_ATTACHMENT;
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) usage = usage | image_usage::STORAGE;
        return image_desc{
            .fmt = normalize_dx12_format(desc.Format),
            .extent = {static_cast<uint32_t>(desc.Width), desc.Height,
                       desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1u},
            .usage = usage,
            .type = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? image_type::TYPE_1D
                  : desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? image_type::TYPE_3D
                                                                         : image_type::TYPE_2D,
            .mip_levels = desc.MipLevels,
            .array_layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize,
            .samples = static_cast<sample_count>(desc.SampleDesc.Count),
        };
    }

    [[nodiscard]] inline buffer_desc normalize_dx12_buffer_desc(const D3D12_RESOURCE_DESC& desc) noexcept
    {
        buffer_usage usage = buffer_usage::NONE;
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) usage = usage | buffer_usage::STORAGE_BUFFER;
        return buffer_desc{.size = desc.Width, .usage = usage};
    }

    // --- lower: render_graph -> D3D12 ---
    [[nodiscard]] inline D3D12_RESOURCE_DESC lower_dx12_image_desc(const image_desc& desc) noexcept
    {
        return D3D12_RESOURCE_DESC{
            .Dimension = desc.type == image_type::TYPE_1D ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                       : desc.type == image_type::TYPE_3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                          : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = desc.extent.width,
            .Height = desc.extent.height,
            .DepthOrArraySize = static_cast<UINT16>(desc.type == image_type::TYPE_3D ? desc.extent.depth : desc.array_layers),
            .MipLevels = static_cast<UINT16>(desc.mip_levels),
            .Format = lower_dx12_format(desc.fmt),
            .SampleDesc = {.Count = static_cast<UINT>(desc.samples), .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = lower_dx12_image_flags(desc.usage),
        };
    }

    [[nodiscard]] inline D3D12_RESOURCE_DESC lower_dx12_buffer_desc(const buffer_desc& desc) noexcept
    {
        return D3D12_RESOURCE_DESC{
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Alignment = 0,
            .Width = desc.size,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = (desc.usage & buffer_usage::STORAGE_BUFFER) != buffer_usage::NONE
                         ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                         : D3D12_RESOURCE_FLAG_NONE,
        };
    }

    // =============================================================================
    // Sampler descriptor translation (R2 contract sync)
    // =============================================================================

    [[nodiscard]] inline D3D12_COMPARISON_FUNC lower_dx12_compare_func(sampler_compare_op value) noexcept
    {
        switch (value)
        {
        case sampler_compare_op::never: return D3D12_COMPARISON_FUNC_NEVER;
        case sampler_compare_op::less: return D3D12_COMPARISON_FUNC_LESS;
        case sampler_compare_op::equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case sampler_compare_op::less_or_equal: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case sampler_compare_op::greater: return D3D12_COMPARISON_FUNC_GREATER;
        case sampler_compare_op::not_equal: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case sampler_compare_op::greater_or_equal: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case sampler_compare_op::always: return D3D12_COMPARISON_FUNC_ALWAYS;
        }
        return D3D12_COMPARISON_FUNC_NEVER;
    }

    [[nodiscard]] inline sampler_compare_op normalize_dx12_compare_func(D3D12_COMPARISON_FUNC value) noexcept
    {
        switch (value)
        {
        case D3D12_COMPARISON_FUNC_NEVER: return sampler_compare_op::never;
        case D3D12_COMPARISON_FUNC_LESS: return sampler_compare_op::less;
        case D3D12_COMPARISON_FUNC_EQUAL: return sampler_compare_op::equal;
        case D3D12_COMPARISON_FUNC_LESS_EQUAL: return sampler_compare_op::less_or_equal;
        case D3D12_COMPARISON_FUNC_GREATER: return sampler_compare_op::greater;
        case D3D12_COMPARISON_FUNC_NOT_EQUAL: return sampler_compare_op::not_equal;
        case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return sampler_compare_op::greater_or_equal;
        case D3D12_COMPARISON_FUNC_ALWAYS: return sampler_compare_op::always;
        }
        return sampler_compare_op::never;
    }

    // --- lower: render_graph -> D3D12 ---
    [[nodiscard]] inline D3D12_SAMPLER_DESC lower_dx12_sampler_desc(const sampler_desc& desc) noexcept
    {
        const bool nearest = desc.min_filter == sampler_filter::nearest && desc.mag_filter == sampler_filter::nearest;
        return D3D12_SAMPLER_DESC{
            .Filter = nearest ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = desc.address_u == sampler_address_mode::clamp_to_edge ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                      : desc.address_u == sampler_address_mode::mirrored_repeat ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR
                                                                                : D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = desc.address_v == sampler_address_mode::clamp_to_edge ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                      : desc.address_v == sampler_address_mode::mirrored_repeat ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR
                                                                                : D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .MipLODBias = 0,
            .MaxAnisotropy = 0,
            .ComparisonFunc = lower_dx12_compare_func(desc.compare_op),
            .BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
            .MinLOD = 0,
            .MaxLOD = desc.max_lod,
        };
    }

    // --- normalize: D3D12 -> render_graph (仅还原本契约表达的字段) ---
    [[nodiscard]] inline sampler_desc normalize_dx12_sampler_desc(const D3D12_SAMPLER_DESC& desc) noexcept
    {
        const bool nearest = desc.Filter == D3D12_FILTER_MIN_MAG_MIP_POINT;
        return sampler_desc{
            .min_filter = nearest ? sampler_filter::nearest : sampler_filter::linear,
            .mag_filter = nearest ? sampler_filter::nearest : sampler_filter::linear,
            .address_u = desc.AddressU == D3D12_TEXTURE_ADDRESS_MODE_CLAMP ? sampler_address_mode::clamp_to_edge
                       : desc.AddressU == D3D12_TEXTURE_ADDRESS_MODE_MIRROR ? sampler_address_mode::mirrored_repeat
                                                                            : sampler_address_mode::repeat,
            .address_v = desc.AddressV == D3D12_TEXTURE_ADDRESS_MODE_CLAMP ? sampler_address_mode::clamp_to_edge
                       : desc.AddressV == D3D12_TEXTURE_ADDRESS_MODE_MIRROR ? sampler_address_mode::mirrored_repeat
                                                                            : sampler_address_mode::repeat,
            .compare_op = normalize_dx12_compare_func(desc.ComparisonFunc),
            .max_lod = desc.MaxLOD,
        };
    }

    // =============================================================================
    // Heap type translation
    // =============================================================================

    [[nodiscard]] inline D3D12_HEAP_TYPE lower_dx12_heap_type(memory_domain domain) noexcept
    {
        switch (domain)
        {
        case memory_domain::upload: return D3D12_HEAP_TYPE_UPLOAD;
        case memory_domain::readback: return D3D12_HEAP_TYPE_READBACK;
        case memory_domain::device_local: return D3D12_HEAP_TYPE_DEFAULT;
        }
        return D3D12_HEAP_TYPE_DEFAULT;
    }
} // namespace render_graph
