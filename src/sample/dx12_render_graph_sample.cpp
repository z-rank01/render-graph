#include <iostream>

#include "render_graph/system.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

int main()
{
    render_graph::render_graph_system rg;

#if defined(_WIN32)
    std::cout << "dx12_render_graph_sample: compiled graph (DX12 headers available)\n";
#else
    std::cout << "dx12_render_graph_sample: compiled graph (not on Windows)\n";
#endif

    return 0;
}
