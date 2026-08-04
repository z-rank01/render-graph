#pragma once

#include "barrier.h"
#include "resource.h"

#if !defined(_WIN32)
    #error "dx12_backend requires Windows (_WIN32)"
#endif

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
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

        using error_callback_t = std::function<void(const char*)>;
        
        void set_error_callback(error_callback_t cb) { error_callback = std::move(cb); }

        [[nodiscard]] const std::string& get_last_error() const { return last_error; }

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

        static uint32_t image_mip_levels(const image_desc& desc) noexcept { return desc.MipLevels; }
        static uint32_t image_array_layers(const image_desc& desc) noexcept { return desc.DepthOrArraySize; }
        static uint64_t buffer_size(const buffer_desc& desc) noexcept { return desc.Width; }

        void bind_imported_image(image_handle logical_image, native_image_handle native_image)
        {
            if (native_image == nullptr)
            {
                report_error("bind_imported_image: native_image is null (logical=" +
                             std::to_string(static_cast<unsigned>(logical_image)) + ")");
            }
            pending_imported_images[logical_image] = native_image;
        }

        void bind_imported_buffer(buffer_handle logical_buffer, native_buffer_handle native_buffer)
        {
            if (native_buffer == nullptr)
            {
                report_error("bind_imported_buffer: native_buffer is null (logical=" +
                             std::to_string(static_cast<unsigned>(logical_buffer)) + ")");
            }
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
                report_error("on_compile_resource_allocation: missing D3D12 device (call set_context first)");
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
                    else
                    {
                        report_error("imported image is not bound (logical=" +
                                     std::to_string(static_cast<unsigned>(rep)) +
                                     ", physical=" +
                                     std::to_string(static_cast<unsigned>(physical_id)) +
                                     ")");
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
                else
                {
                    const auto hr_str = hresult_to_string(hr);
                    report_error("CreateCommittedResource(image) failed (logical=" +
                                 std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" +
                                 std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", hr=" + hr_str + ")");
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
                    else
                    {
                        report_error("imported buffer is not bound (logical=" +
                                     std::to_string(static_cast<unsigned>(rep)) +
                                     ", physical=" +
                                     std::to_string(static_cast<unsigned>(physical_id)) +
                                     ")");
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
                else
                {
                    const auto hr_str = hresult_to_string(hr);
                    report_error("CreateCommittedResource(buffer) failed (logical=" +
                                 std::to_string(static_cast<unsigned>(rep)) +
                                 ", physical=" +
                                 std::to_string(static_cast<unsigned>(physical_id)) +
                                 ", hr=" + hr_str + ")");
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

        [[nodiscard]] native_image_handle get_image(image_handle logical) const
        {
            const auto physical = get_physical_image_id(logical);
            if (physical == std::numeric_limits<uint32_t>::max() || physical >= images.size())
            {
                return nullptr;
            }
            return images[physical].Get();
        }

        [[nodiscard]] native_buffer_handle get_buffer(buffer_handle logical) const
        {
            const auto physical = get_physical_buffer_id(logical);
            if (physical == std::numeric_limits<uint32_t>::max() || physical >= buffers.size())
            {
                return nullptr;
            }
            return buffers[physical].Get();
        }

    private:
        static std::string hresult_to_string(HRESULT hr)
        {
            char sys_msg[512];
            sys_msg[0] = '\0';

            const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
            const DWORD len = FormatMessageA(flags,
                                            nullptr,
                                            static_cast<DWORD>(hr),
                                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                            sys_msg,
                                            static_cast<DWORD>(sizeof(sys_msg)),
                                            nullptr);

            char hex[32];
            (void)std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));

            std::string out = hex;
            if (len > 0)
            {
                std::string msg = sys_msg;
                while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' '))
                {
                    msg.pop_back();
                }
                if (!msg.empty())
                {
                    out += ": ";
                    out += msg;
                }
            }
            return out;
        }

        void report_error(const char* msg)
        {
            if (msg == nullptr)
            {
                return;
            }

            last_error = msg;
            if (error_callback)
            {
                error_callback(msg);
                return;
            }

            OutputDebugStringA("[render_graph][dx12_backend] ");
            OutputDebugStringA(msg);
            OutputDebugStringA("\n");
        }

        void report_error(const std::string& msg) { report_error(msg.c_str()); }

        // Optional: user-provided error callback. If unset, defaults to OutputDebugString.
        error_callback_t error_callback;

        // Stores the last reported error message (best-effort).
        std::string last_error;

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
    };
} // namespace render_graph
