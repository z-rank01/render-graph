#pragma once

#include "backend.h"

#if !defined(_WIN32)
#error "dx12_backend requires Windows (_WIN32)"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace render_graph
{
    class dx12_backend : public backend
    {
    public:
        using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

        ID3D12Device* device = nullptr; // external

        // Mapping from logical handle -> physical id (filled at compile)
        std::vector<uint32_t> logical_to_physical_img_id;
        std::vector<uint32_t> logical_to_physical_buf_id;

        // Physical tables (one entry per physical id)
        std::vector<ComPtr> images;
        std::vector<ComPtr> buffers;

        // Pending imported bindings (logical -> native)
        std::unordered_map<resource_handle, ID3D12Resource*> pending_imported_images;
        std::unordered_map<resource_handle, ID3D12Resource*> pending_imported_buffers;

        void set_context(ID3D12Device* device_in)
        {
            device = device_in;
        }

        static DXGI_FORMAT to_dxgi_format(format fmt)
        {
            switch (fmt)
            {
            case format::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case format::R8G8B8A8_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case format::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case format::B8G8R8A8_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            case format::D32_SFLOAT:     return DXGI_FORMAT_D32_FLOAT;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        void bind_imported_image(resource_handle logical_image, native_handle native_image, native_handle /*native_view*/ = 0) override
        {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto* res = reinterpret_cast<ID3D12Resource*>(native_image);
            pending_imported_images[logical_image] = res;
        }

        void bind_imported_buffer(resource_handle logical_buffer, native_handle native_buffer) override
        {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto* res = reinterpret_cast<ID3D12Resource*>(native_buffer);
            pending_imported_buffers[logical_buffer] = res;
        }

        void on_compile_resource_allocation(const resource_meta_table& meta, const physical_resource_meta& physical_meta) override
        {
            logical_to_physical_img_id = physical_meta.handle_to_physical_img_id;
            logical_to_physical_buf_id = physical_meta.handle_to_physical_buf_id;

            images.clear();
            buffers.clear();
            images.resize(physical_meta.physical_image_meta.size());
            buffers.resize(physical_meta.physical_buffer_meta.size());

            if (!device)
            {
                return;
            }

            // Images
            for (size_t physical_id = 0; physical_id < physical_meta.physical_image_meta.size(); physical_id++)
            {
                const auto rep = physical_meta.physical_image_meta[physical_id];
                if (rep >= meta.image_metas.names.size())
                {
                    continue;
                }

                if (meta.image_metas.is_imported[rep])
                {
                    auto it = pending_imported_images.find(rep);
                    if (it != pending_imported_images.end() && it->second)
                    {
                        images[physical_id] = it->second; // AddRef
                    }
                    continue;
                }

                const auto extent = meta.image_metas.extents[rep];
                const auto fmt = to_dxgi_format(meta.image_metas.formats[rep]);

                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
                const auto usage = meta.image_metas.usages[rep];
                if ((static_cast<uint32_t>(usage) & static_cast<uint32_t>(image_usage::COLOR_ATTACHMENT)) != 0)
                {
                    flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                }
                if ((static_cast<uint32_t>(usage) & static_cast<uint32_t>(image_usage::DEPTH_STENCIL_ATTACHMENT)) != 0)
                {
                    flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                }
                if ((static_cast<uint32_t>(usage) & static_cast<uint32_t>(image_usage::STORAGE)) != 0)
                {
                    flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                }

                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Alignment = 0;
                desc.Width = extent.width;
                desc.Height = extent.height;
                desc.DepthOrArraySize = static_cast<UINT16>(meta.image_metas.array_layers[rep]);
                desc.MipLevels = static_cast<UINT16>(meta.image_metas.mip_levels[rep]);
                desc.Format = fmt;
                desc.SampleDesc.Count = meta.image_metas.sample_counts[rep];
                desc.SampleDesc.Quality = 0;
                desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                desc.Flags = flags;

                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;

                ComPtr resource;
                const HRESULT hr = device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_COMMON,
                    nullptr,
                    IID_PPV_ARGS(resource.GetAddressOf()));

                if (SUCCEEDED(hr))
                {
                    images[physical_id] = resource;
                }
            }

            // Buffers
            for (size_t physical_id = 0; physical_id < physical_meta.physical_buffer_meta.size(); physical_id++)
            {
                const auto rep = physical_meta.physical_buffer_meta[physical_id];
                if (rep >= meta.buffer_metas.names.size())
                {
                    continue;
                }

                if (meta.buffer_metas.is_imported[rep])
                {
                    auto it = pending_imported_buffers.find(rep);
                    if (it != pending_imported_buffers.end() && it->second)
                    {
                        buffers[physical_id] = it->second;
                    }
                    continue;
                }

                const auto size = meta.buffer_metas.sizes[rep];
                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
                const auto usage = meta.buffer_metas.usages[rep];
                if ((static_cast<uint32_t>(usage) & static_cast<uint32_t>(buffer_usage::STORAGE_BUFFER)) != 0)
                {
                    flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                }

                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Alignment = 0;
                desc.Width = static_cast<UINT64>(size);
                desc.Height = 1;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = DXGI_FORMAT_UNKNOWN;
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                desc.Flags = flags;

                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;

                ComPtr resource;
                const HRESULT hr = device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_COMMON,
                    nullptr,
                    IID_PPV_ARGS(resource.GetAddressOf()));

                if (SUCCEEDED(hr))
                {
                    buffers[physical_id] = resource;
                }
            }
        }

        void apply_barriers(pass_handle /*pass*/, const per_pass_barrier& /*plan*/) override {}

        [[nodiscard]] uint32_t get_physical_image_id(resource_handle logical) const
        {
            if (logical >= logical_to_physical_img_id.size())
            {
                return std::numeric_limits<uint32_t>::max();
            }
            return logical_to_physical_img_id[logical];
        }

        [[nodiscard]] uint32_t get_physical_buffer_id(resource_handle logical) const
        {
            if (logical >= logical_to_physical_buf_id.size())
            {
                return std::numeric_limits<uint32_t>::max();
            }
            return logical_to_physical_buf_id[logical];
        }
    };
}
