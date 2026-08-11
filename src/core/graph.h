#pragma once

#include <span>
#include <variant>
#include <vector>

#include "render_graph/compiler.h"

namespace render_graph::core
{
    struct access_event
    {
        pass_handle pass = invalid_pass;
        resource_kind kind = resource_kind::image;
        resource_handle logical = invalid_resource;
        access_type access = access_type::read;
        std::variant<image_access_desc, buffer_access_desc> state;
    };

    struct dependency_graph
    {
        std::vector<std::vector<pass_handle>> outgoing;
        std::vector<uint32_t> in_degrees;
    };

    void build_dependency_dag(std::span<const access_event> events,
                              uint32_t pass_count,
                              dependency_graph& graph);
    bool schedule_passes(const dependency_graph& graph, std::vector<pass_handle>& schedule);
} // namespace render_graph::core
