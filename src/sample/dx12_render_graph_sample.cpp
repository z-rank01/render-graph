#include <cstdint>
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

    render_graph::dx12_backend backend;
    backend.set_context(device.Get());

    // Imported resources (act like swapchain/external inputs)
    ComPtr<ID3D12Resource> imported_tex;
    ComPtr<ID3D12Resource> imported_buf;
    if (device != nullptr)
    {
        (void)create_imported_texture(device.Get(), imported_tex);
        (void)create_imported_buffer(device.Get(), 2048, imported_buf);
    }

    render_graph::render_graph_system system;
    system.set_backend(&backend);

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

    auto noop_execute = [](render_graph::pass_execute_context&) {};

    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            state.g0 = ctx.create_image(render_graph::image_info{
                .name          = "g0",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT | render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.g0, render_graph::image_usage::COLOR_ATTACHMENT);

            state.g1 = ctx.create_image(render_graph::image_info{
                .name          = "g1",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT | render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.g1, render_graph::image_usage::COLOR_ATTACHMENT);

            state.b0 = ctx.create_buffer(render_graph::buffer_info{
                .name     = "b0",
                .size     = 4096,
                .usage    = render_graph::buffer_usage::STORAGE_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);
        },
        noop_execute);

    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g1, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            ctx.write_image(state.g1, render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.write_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            state.t0 = ctx.create_image(render_graph::image_info{
                .name          = "t0",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.t0, render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            state.external = ctx.create_image(render_graph::image_info{
                .name          = "external",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });
            if (imported_tex)
            {
                backend.bind_imported_image(state.external,
                                            static_cast<render_graph::backend::native_handle>(reinterpret_cast<uintptr_t>(imported_tex.Get())));
            }
            ctx.read_image(state.external, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);

            state.external_buf = ctx.create_buffer(render_graph::buffer_info{
                .name     = "external_buf",
                .size     = 2048,
                .usage    = render_graph::buffer_usage::STORAGE_BUFFER,
                .imported = true,
            });
            if (imported_buf)
            {
                backend.bind_imported_buffer(state.external_buf,
                                             static_cast<render_graph::backend::native_handle>(reinterpret_cast<uintptr_t>(imported_buf.Get())));
            }
            ctx.read_buffer(state.external_buf, render_graph::buffer_usage::STORAGE_BUFFER);

            state.l0 = ctx.create_image(render_graph::image_info{
                .name          = "l0",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT | render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.l0, render_graph::image_usage::COLOR_ATTACHMENT);

            state.b1 = ctx.create_buffer(render_graph::buffer_info{
                .name     = "b1",
                .size     = 1024,
                .usage    = render_graph::buffer_usage::UNIFORM_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(state.b1, render_graph::buffer_usage::UNIFORM_BUFFER);
        },
        noop_execute);

    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            ctx.read_image(state.l0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b1, render_graph::buffer_usage::UNIFORM_BUFFER);

            state.final_img = ctx.create_image(render_graph::image_info{
                .name          = "final",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.final_img, render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.final_img);
        },
        noop_execute);

    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            const auto trash = ctx.create_image(render_graph::image_info{
                .name          = "trash",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 128, .height = 128, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(trash, render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    system.compile();

    std::cout << "dx12_render_graph_sample: compile OK\n";
    std::cout << "  logical images: " << system.meta_table.image_metas.names.size() << "\n";
    std::cout << "  logical buffers: " << system.meta_table.buffer_metas.names.size() << "\n";
    std::cout << "  physical images: " << system.physical_resource_metas.physical_image_meta.size() << "\n";
    std::cout << "  physical buffers: " << system.physical_resource_metas.physical_buffer_meta.size() << "\n";

    std::cout << "  aliasing (logical->physical):\n";
    std::cout << "    g0          -> img#" << backend.get_physical_image_id(state.g0) << "\n";
    std::cout << "    g1          -> img#" << backend.get_physical_image_id(state.g1) << "\n";
    std::cout << "    t0          -> img#" << backend.get_physical_image_id(state.t0) << "\n";
    std::cout << "    l0          -> img#" << backend.get_physical_image_id(state.l0) << "\n";
    std::cout << "    external    -> img#" << backend.get_physical_image_id(state.external) << "\n";
    std::cout << "    final       -> img#" << backend.get_physical_image_id(state.final_img) << "\n";
    std::cout << "    b0          -> buf#" << backend.get_physical_buffer_id(state.b0) << "\n";
    std::cout << "    b1          -> buf#" << backend.get_physical_buffer_id(state.b1) << "\n";
    std::cout << "    externalBuf -> buf#" << backend.get_physical_buffer_id(state.external_buf) << "\n";

    size_t created_images = 0;
    for (const auto& img : backend.images)
    {
        if (img != nullptr) { created_images++; }
    }
    size_t created_buffers = 0;
    for (const auto& buf : backend.buffers)
    {
        if (buf != nullptr) { created_buffers++; }
    }
    std::cout << "  backend native handles (non-null): images=" << created_images
              << ", buffers=" << created_buffers << "\n";

    // Expected results (no asserts; verify by reading the printed mapping above):
    // - The pass named "trash" is culled (it does not contribute to declared outputs).
    // - Imported resources ("external", "external_buf") always map to their own physical ids.
    // - The short-lived image "t0" is eligible to alias with later transient images of the
    //   same shape/format/usage if lifetimes do not overlap (greedy first-fit).
    // - Buffers b0 (passes 0-1) and b1 (passes 2-3) have disjoint lifetimes and may alias.

    return 0;
#endif
}
