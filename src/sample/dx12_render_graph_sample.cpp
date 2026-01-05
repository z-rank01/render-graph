#include <iostream>

#include "render_graph/dx12_backend.h"
#include "render_graph/system.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace
{
    using Microsoft::WRL::ComPtr;

    bool create_device(ComPtr<IDXGIFactory6>& factory, ComPtr<ID3D12Device>& device)
    {
        UINT flags = 0;
        if (CreateDXGIFactory2(flags, IID_PPV_ARGS(factory.GetAddressOf())) != S_OK)
        {
            return false;
        }

        for (UINT adapter_index = 0;; adapter_index++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(adapter_index, adapter.GetAddressOf()) != S_OK)
            {
                break;
            }

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            if (software)
            {
                continue;
            }

            if (D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())) == S_OK)
            {
                return true;
            }
        }

        // Fallback: WARP
        ComPtr<IDXGIAdapter> warp;
        if (factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf())) != S_OK)
        {
            return false;
        }
        return D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())) == S_OK;
    }

    bool create_imported_texture(ID3D12Device* device, ComPtr<ID3D12Resource>& out)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = 64;
        desc.Height = 64;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        return device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(out.GetAddressOf())) == S_OK;
    }

    bool create_imported_buffer(ID3D12Device* device, UINT64 size, ComPtr<ID3D12Resource>& out)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        return device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(out.GetAddressOf())) == S_OK;
    }
}
#endif

int main()
{
#if !defined(_WIN32)
    std::cout << "dx12_render_graph_sample: not on Windows\n";
    return 0;
#else
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    if (!create_device(factory, device))
    {
        std::cout << "dx12_render_graph_sample: D3D12 device init failed; will still build/compile graph without creating native resources.\n";
    }

    using system_t = render_graph::render_graph_system<render_graph::dx12_backend>;

    // Imported resources (act like swapchain/external inputs)
    ComPtr<ID3D12Resource> imported_tex;
    ComPtr<ID3D12Resource> imported_buf;
    if (device != nullptr)
    {
        (void)create_imported_texture(device.Get(), imported_tex);
        (void)create_imported_buffer(device.Get(), 2048, imported_buf);
    }

    system_t system;
    system.set_backend_context(device.Get());

    struct state_t
    {
        render_graph::resource_handle g0 = 0;
        render_graph::resource_handle g1 = 0;
        render_graph::resource_handle t0 = 0;
        render_graph::resource_handle l0 = 0;
        render_graph::resource_handle external = 0;
        render_graph::resource_handle final_img = 0;

        render_graph::resource_handle b0 = 0;
        render_graph::resource_handle b1 = 0;
        render_graph::resource_handle external_buf = 0;
    } state;

    auto noop_execute = [](system_t::pass_execute_context&) {};

    const auto make_tex = [](DXGI_FORMAT fmt, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags) -> D3D12_RESOURCE_DESC
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;
        return desc;
    };

    const auto make_buf = [](UINT64 size, D3D12_RESOURCE_FLAGS flags) -> D3D12_RESOURCE_DESC
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return desc;
    };

    system.add_pass(
        [&](system_t::pass_setup_context& ctx)
        {
            state.g0 = ctx.create_and_write_image(
                "g0",
                make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, 320, 180, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                render_graph::image_usage::COLOR_ATTACHMENT);

            state.g1 = ctx.create_and_write_image(
                "g1",
                make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, 320, 180, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                render_graph::image_usage::COLOR_ATTACHMENT);

            state.b0 = ctx.create_and_write_buffer(
                "b0",
                make_buf(4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                render_graph::buffer_usage::STORAGE_BUFFER);
        },
        noop_execute);

    system.add_pass(
        [&](system_t::pass_setup_context& ctx)
        {
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g1, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            ctx.write_image(state.g1, render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.write_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            state.t0 = ctx.create_and_write_image(
                "t0",
                make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, 320, 180, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    system.add_pass(
        [&](system_t::pass_setup_context& ctx)
        {
            state.external = ctx.create_image(
                "external",
                make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, D3D12_RESOURCE_FLAG_NONE),
                true);
            if (imported_tex)
            {
                system.bind_imported_image(state.external, imported_tex.Get());
            }
            ctx.read_image(state.external, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);

            state.external_buf = ctx.create_buffer(
                "external_buf",
                make_buf(2048, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                true);
            if (imported_buf)
            {
                system.bind_imported_buffer(state.external_buf, imported_buf.Get());
            }
            ctx.read_buffer(state.external_buf, render_graph::buffer_usage::STORAGE_BUFFER);

            state.l0 = ctx.create_and_write_image(
                "l0",
                make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, 320, 180, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                render_graph::image_usage::COLOR_ATTACHMENT);

            state.b1 = ctx.create_and_write_buffer(
                "b1",
                make_buf(1024, D3D12_RESOURCE_FLAG_NONE),
                render_graph::buffer_usage::UNIFORM_BUFFER);
        },
        noop_execute);

    system.add_pass(
        [&](system_t::pass_setup_context& ctx)
        {
            ctx.read_image(state.l0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b1, render_graph::buffer_usage::UNIFORM_BUFFER);

            state.final_img = ctx.create_and_write_image(
                "final",
                make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, 320, 180, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.final_img);
        },
        noop_execute);

    system.add_pass(
        [&](system_t::pass_setup_context& ctx)
        {
            const auto trash = ctx.create_image(
                "trash",
                make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
            ctx.write_image(trash, render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    system.compile();

    std::cout << "dx12_render_graph_sample: compile OK\n";

    // Execution is intentionally a no-op in this sample.
    // - Buffers b0 (passes 0-1) and b1 (passes 2-3) have disjoint lifetimes and may alias.

    return 0;
#endif
}
