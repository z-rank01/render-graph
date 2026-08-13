// Dependency-graph core: records per-pass resource accesses, builds the DAG
// between passes, and topologically schedules pass execution order.

#pragma once

#include <span>
#include <variant>
#include <vector>

#include "render_graph/compiler.h"

namespace render_graph::core
{
    // =============================================================================
    // Types
    // =============================================================================

    // One recorded access: 
    // 'pass' touches 'logical' resource
    // with 'access' mode 
    // 'state' selects the image or buffer descriptor.
    struct access_event
    {
        pass_handle pass = invalid_pass;
        resource_kind kind = resource_kind::image;
        resource_handle logical = invalid_resource;
        access_type access = access_type::read;
        std::variant<image_access_desc, buffer_access_desc> state;
    };

    // Adjacency-list DAG over passes: `outgoing[i]` lists successors of pass i,
    // `in_degrees[i]` its remaining predecessor count.
    struct dependency_graph
    {
        std::vector<std::vector<pass_handle>> outgoing;
        std::vector<uint32_t> in_degrees;
    };

    // =============================================================================
    // DAG construction and scheduling
    // =============================================================================

    void build_dependency_dag(std::span<const access_event> events,
                              uint32_t pass_count,
                              dependency_graph& graph);

    // Kahn-style topological order over the active sub-graph; returns false if
    // the (active) sub-graph contains a cycle.
    bool schedule_passes(const dependency_graph& graph,
                         std::span<const uint8_t> active_passes,
                         std::vector<pass_handle>& schedule);
} // namespace render_graph::core
