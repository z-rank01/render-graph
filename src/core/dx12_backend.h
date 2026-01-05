#pragma once

#include "barrier.h"
#include "resource.h"

#if !defined(_WIN32)
    #error "dx12_backend requires Windows (_WIN32)"
#endif

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>


namespace render_graph
{
    class dx12_backend
    {
    public:
        using ComPtr               = Microsoft::WRL::ComPtr<ID3D12Resource>;
        using image_desc           = D3D12_RESOURCE_DESC;
        using buffer_desc          = D3D12_RESOURCE_DESC;
        using native_image_handle  = ID3D12Resource*;
        using native_buffer_handle = ID3D12Resource*;

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

        void set_context(ID3D12Device* device_in) { device = device_in; }

        void apply_barriers(pass_handle /*pass*/, const per_pass_barrier& /*plan*/) { }

        static uint64_t hash_combine(uint64_t seed, uint64_t v) noexcept
        {
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }

        static uint64_t hash_image_desc(const image_desc& d) noexcept
        {
            uint64_t hash = 0;
            hash          = hash_combine(hash, static_cast<uint64_t>(d.Dimension));
            hash          = hash_combine(hash, static_cast<uint64_t>(d.Alignment));
            hash          = hash_combine(hash, static_cast<uint64_t>(d.Width));
            hash          = hash_combine(hash, static_cast<uint64_t>(d.Height));
            hash          = hash_combine(hash, static_cast<uint64_t>(d.DepthOrArraySize));
            hash          = hash_combine(hash, static_cast<uint64_t>(d.MipLevels));
            hash          = hash_combine(hash, static_cast<uint64_t>(d.Format));
            hash          = hash_combine(hash, (static_cast<uint64_t>(d.SampleDesc.Count) << 32) | d.SampleDesc.Quality);
            hash          = hash_combine(hash, static_cast<uint64_t>(d.Layout));
            hash          = hash_combine(hash, static_cast<uint64_t>(d.Flags));
            return hash;
        }

        static uint64_t hash_buffer_desc(const buffer_desc& d) noexcept { return hash_image_desc(d); }

        static bool is_compatible_image(const image_desc& a, const image_desc& b) noexcept
        {
            return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width && a.Height == b.Height &&
                   a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
                   a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout &&
                   a.Flags == b.Flags;
        }

        static bool is_compatible_buffer(const buffer_desc& a, const buffer_desc& b) noexcept { return is_compatible_image(a, b); }

        void bind_imported_image(resource_handle logical_image, native_image_handle native_image)
        {
            pending_imported_images[logical_image] = native_image;
        }

        void bind_imported_buffer(resource_handle logical_buffer, native_buffer_handle native_buffer)
        {
            pending_imported_buffers[logical_buffer] = native_buffer;
        }

        template <typename MetaTableT>
        void on_compile_resource_allocation(const MetaTableT& meta, const physical_resource_meta& physical_meta)
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

                const D3D12_RESOURCE_DESC desc = meta.image_metas.descs[rep];

                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;

                ComPtr resource;
                const HRESULT hr = device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(resource.GetAddressOf()));

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

                const D3D12_RESOURCE_DESC desc = meta.buffer_metas.descs[rep];

                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;

                ComPtr resource;
                const HRESULT hr = device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(resource.GetAddressOf()));

                if (SUCCEEDED(hr))
                {
                    buffers[physical_id] = resource;
                }
            }
        }

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

        [[nodiscard]] native_image_handle get_image(resource_handle logical) const
        {
            const auto physical = get_physical_image_id(logical);
            if (physical == std::numeric_limits<uint32_t>::max() || physical >= images.size())
            {
                return nullptr;
            }
            return images[physical].Get();
        }

        [[nodiscard]] native_buffer_handle get_buffer(resource_handle logical) const
        {
            const auto physical = get_physical_buffer_id(logical);
            if (physical == std::numeric_limits<uint32_t>::max() || physical >= buffers.size())
            {
                return nullptr;
            }
            return buffers[physical].Get();
        }
    };
} // namespace render_graph
