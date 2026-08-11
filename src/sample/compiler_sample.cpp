#include <array>
#include <iostream>

#include "render_graph/compiler.h"

#ifndef RENDER_GRAPH_SAMPLE_LABEL
#define RENDER_GRAPH_SAMPLE_LABEL "render_graph_sample"
#endif

int main()
{
    using namespace render_graph;
    const std::array resources{
        frame_resource_row{.source = frame_resource_source::transient_buffer, .name = "scene",
            .buffer_description = {.size = 4096, .usage = buffer_usage::STORAGE_BUFFER}},
        frame_resource_row{.source = frame_resource_source::swapchain_image, .name = "swapchain"},
    };
    const std::array buffers{frame_buffer_access_row{
        .resource = {0}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::read}};
    const std::array attachments{frame_attachment_row{.resource = {1}}};
    const std::array passes{frame_pass_row{
        .name = "DrawPass", .kind = pass_kind::raster,
        .buffer_accesses = {0, 1}, .attachments = {0, 1}}};
    const frame_plan frame{.resources = resources, .passes = passes,
                           .buffer_accesses = buffers, .attachments = attachments};
    const auto compiled = compile_graph({.frame = &frame, .environment = {
        .extent = {1280, 720, 1}, .color_format = format::B8G8R8A8_UNORM}});
    if (!compiled)
    {
        std::cerr << RENDER_GRAPH_SAMPLE_LABEL << ": compile failed\n";
        return 1;
    }
    std::cout << RENDER_GRAPH_SAMPLE_LABEL << ": compile OK\n";
    return 0;
}
