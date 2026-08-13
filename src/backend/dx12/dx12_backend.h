#pragma once

// Direct3D 12 backend for render_graph: lowers resource descs to D3D12,
// creates physical resources at compile time, and binds imported resources.
// Barrier and raster-pass emission is currently stubbed.

#include "render_graph/barrier.h"
#include "render_graph/resource.h"
#include "render_graph/raster.h"
#include "dx12_resource_lowering.h"

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
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// dx12_backend — Direct3D 12 backend
// =============================================================================
namespace render_graph
{
    class dx12_backend
    {
    public:
        // --- Type aliases ---
        using ComPtr               = Microsoft::WRL::ComPtr<ID3D12Resource>;
        using image_desc           = render_graph::image_desc;
        using buffer_desc          = render_graph::buffer_desc;
        using native_image_handle  = ID3D12Resource*;
        using native_buffer_handle = ID3D12Resource*;
        using command_context       = ID3D12GraphicsCommandList*;

        // --- Context and error reporting ---
        using error_callback_t = std::function<void(const char*)>;
        
        void set_error_callback(error_callback_t cb) { error_callback = std::move(cb); }

        [[nodiscard]] const std::string& get_last_error() const { return last_error; }
        void clear_error() { last_error.clear(); }

        void set_context(ID3D12Device* device_in) { device = device_in; }

        // --- Pass emission ---
        // Stubs for now: barriers and raster passes succeed without recording any commands.
        template <typename OpTable>
        bool emit_barriers(command_context& /*commands*/, const OpTable& /*ops*/, uint32_t /*begin*/, uint32_t /*length*/)
        {
            return true;
        }

        bool begin_raster_pass(command_context&, std::span<const raster_attachment>, const raster_attachment*,
                               render_area, uint32_t) { return true; }
        bool end_raster_pass(command_context&) { return true; }

        // --- Descriptor hashing and compatibility ---
        static uint64_t hash_combine(uint64_t seed, uint64_t v) noexcept
        {
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }

        static uint64_t hash_image_desc(const image_desc& d) noexcept
        {
            uint64_t hash = 0;
            hash = hash_combine(hash, static_cast<uint64_t>(d.fmt));
            hash = hash_combine(hash, (static_cast<uint64_t>(d.extent.width) << 32) | d.extent.height);
            hash = hash_combine(hash, d.extent.depth);
            hash = hash_combine(hash, (static_cast<uint64_t>(d.mip_levels) << 32) | d.array_layers);
            hash = hash_combine(hash, static_cast<uint64_t>(d.samples));
            hash = hash_combine(hash, static_cast<uint64_t>(d.usage));
            hash = hash_combine(hash, static_cast<uint64_t>(d.type));
            hash = hash_combine(hash, static_cast<uint64_t>(d.flags));
            hash = hash_combine(hash, static_cast<uint64_t>(d.memory));
            hash = hash_combine(hash, static_cast<uint64_t>(d.mapping));
            hash = hash_combine(hash, static_cast<uint64_t>(d.allocation));
            hash = hash_combine(hash, static_cast<uint64_t>(d.aliasing));
            hash = hash_combine(hash, static_cast<uint64_t>(d.lifetime));
            return hash;
        }

        static uint64_t hash_buffer_desc(const buffer_desc& d) noexcept
        {
            uint64_t hash = 0;
            hash = hash_combine(hash, d.size);
            hash = hash_combine(hash, static_cast<uint64_t>(d.usage));
            hash = hash_combine(hash, static_cast<uint64_t>(d.memory));
            hash = hash_combine(hash, static_cast<uint64_t>(d.mapping));
            hash = hash_combine(hash, static_cast<uint64_t>(d.allocation));
            hash = hash_combine(hash, static_cast<uint64_t>(d.aliasing));
            hash = hash_combine(hash, static_cast<uint64_t>(d.lifetime));
            return hash;
        }

        static bool is_compatible_image(const image_desc& a, const image_desc& b) noexcept
        {
            return a == b;
        }

        static bool is_compatible_buffer(const buffer_desc& a, const buffer_desc& b) noexcept { return a == b; }

        // --- Capabilities and validation ---
        [[nodiscard]] static backend_capabilities capabilities() noexcept { return {}; }

        [[nodiscard]] static resource_desc_diagnostic validate_image_desc(const image_desc& desc)
        {
            if (desc.fmt == format::UNDEFINED || lower_dx12_format(desc.fmt) == DXGI_FORMAT_UNKNOWN)
                return {false, "DX12 lowering does not support the requested image format"};
            if (desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0)
                return {false, "DX12 image extent components must be non-zero"};
            if (desc.mapping == mapping_policy::persistent && desc.memory == memory_domain::device_local)
                return {false, "persistent mapping requires upload or readback memory"};
            if (desc.memory != memory_domain::device_local &&
                (desc.usage & (image_usage::COLOR_ATTACHMENT | image_usage::DEPTH_STENCIL_ATTACHMENT)) != image_usage::NONE)
                return {false, "DX12 attachment textures require default heap memory"};
            return {};
        }

        [[nodiscard]] static resource_desc_diagnostic validate_buffer_desc(const buffer_desc& desc)
        {
            if (desc.size == 0) return {false, "DX12 buffer size must be non-zero"};
            if (desc.mapping == mapping_policy::persistent && desc.memory == memory_domain::device_local)
                return {false, "persistent mapping requires upload or readback memory"};
            return {};
        }

        // --- Descriptor accessors and allocation requirements ---
        static uint32_t image_mip_levels(const image_desc& desc) noexcept { return desc.mip_levels; }
        static uint32_t image_array_layers(const image_desc& desc) noexcept { return desc.array_layers; }
        static uint64_t buffer_size(const buffer_desc& desc) noexcept { return desc.size; }
        static extent_3d image_extent(const image_desc& desc) noexcept { return desc.extent; }
        static uint32_t image_sample_count(const image_desc& desc) noexcept { return static_cast<uint32_t>(desc.samples); }
        static format image_format(const image_desc& desc) noexcept { return desc.fmt; }
        static bool is_depth_format(format value) noexcept { return value == format::D32_SFLOAT; }

        static allocation_requirements get_image_allocation_requirements(const image_desc&) noexcept
        {
            return allocation_requirements{.supports_aliasing = false};
        }

        static allocation_requirements get_buffer_allocation_requirements(const buffer_desc&) noexcept
        {
            return allocation_requirements{.supports_aliasing = false};
        }

        // --- Imported resource binding ---
        // Imported native resources are staged in the pending_* maps and adopted
        // by on_compile_resource_allocation().
        void bind_imported_image(image_handle logical_image, native_image_handle native_image)
        {
            if (native_image == nullptr)
            {
                report_error("bind_imported_image: native_image is null (logical=" +
                             std::to_string(static_cast<unsigned>(logical_image)) + ")");
            }
            pending_imported_images[logical_image.index()] = native_image;
        }

        void bind_imported_buffer(buffer_handle logical_buffer, native_buffer_handle native_buffer)
        {
            if (native_buffer == nullptr)
            {
                report_error("bind_imported_buffer: native_buffer is null (logical=" +
                             std::to_string(static_cast<unsigned>(logical_buffer)) + ")");
            }
            pending_imported_buffers[logical_buffer.index()] = native_buffer;
        }

        // --- Compile-time resource allocation ---
        // Rebuilds the physical tables (images/buffers, indexed by physical id) and caches
        // the logical -> physical mapping. Requires set_context() to have been called first.
        template <typename MetaTableT>
        void on_compile_resource_allocation(const MetaTableT& meta, const physical_resource_meta& physical_meta)
        {
            // Snapshot the mapping and size the physical tables.
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

            // --- Images ---
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

                const D3D12_RESOURCE_DESC desc = lower_dx12_image_desc(meta.image_metas.descs[rep]);

                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = lower_dx12_heap_type(meta.image_metas.descs[rep].memory);

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

            // --- Buffers ---
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

                const D3D12_RESOURCE_DESC desc = lower_dx12_buffer_desc(meta.buffer_metas.descs[rep]);

                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = lower_dx12_heap_type(meta.buffer_metas.descs[rep].memory);

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

        // --- Resource lookup ---
        // Logical handles resolve through the compile-time mapping; unmapped or
        // out-of-range handles yield invalid_resource / nullptr.
        [[nodiscard]] resource_handle get_physical_image_id(image_handle logical) const
        {
            if (logical.index() >= logical_to_physical_img_id.size())
            {
                return invalid_resource;
            }
            return logical_to_physical_img_id[logical.index()];
        }

        [[nodiscard]] resource_handle get_physical_buffer_id(buffer_handle logical) const
        {
            if (logical.index() >= logical_to_physical_buf_id.size())
            {
                return invalid_resource;
            }
            return logical_to_physical_buf_id[logical.index()];
        }

        [[nodiscard]] native_image_handle get_image(image_handle logical) const
        {
            const auto physical = get_physical_image_id(logical);
            if (physical == invalid_resource || physical >= images.size())
            {
                return nullptr;
            }
            return images[physical].Get();
        }

        [[nodiscard]] native_buffer_handle get_buffer(buffer_handle logical) const
        {
            const auto physical = get_physical_buffer_id(logical);
            if (physical == invalid_resource || physical >= buffers.size())
            {
                return nullptr;
            }
            return buffers[physical].Get();
        }

    // =========================================================================
    // Internal implementation
    // =========================================================================
    private:
        // --- Error formatting and reporting ---
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

        // --- Backend state ---
        // Optional: user-provided error callback. If unset, defaults to OutputDebugString.
        error_callback_t error_callback;

        // Stores the last reported error message (best-effort).
        std::string last_error;

        ID3D12Device* device = nullptr; // external

        // Mapping from logical handle -> physical id (filled at compile)
        std::vector<resource_handle> logical_to_physical_img_id;
        std::vector<resource_handle> logical_to_physical_buf_id;

        // Physical tables (one entry per physical id)
        std::vector<ComPtr> images;
        std::vector<ComPtr> buffers;

        // Pending imported bindings (logical -> native)
        std::unordered_map<resource_handle, ID3D12Resource*> pending_imported_images;
        std::unordered_map<resource_handle, ID3D12Resource*> pending_imported_buffers;
    };
} // namespace render_graph
