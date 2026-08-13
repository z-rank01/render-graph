// Dependency-graph core: records per-pass resource accesses, builds the DAG
// between passes, and topologically schedules pass execution order.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "render_graph/resource.h"

namespace render_graph::core
{
    // =============================================================================
    // Types
    // =============================================================================

    // One access-event table per resource kind (SoA). Events are appended in
    // declaration order during build_resource_versions, then sorted by
    // (pass, logical) and merged per (pass, logical) pair — merged rows carry
    // OR-ed usage and a whole range when the merged ranges disagreed.
    //
    // `event_begins` is the per-pass CSR index (size = pass_count + 1):
    // events of pass p occupy rows [event_begins[p], event_begins[p + 1]).
    struct image_access_rows
    {
        std::vector<pass_handle> passes;
        std::vector<resource_handle> logicals;
        std::vector<access_type> accesses;
        std::vector<image_usage> usages;
        std::vector<pipeline_domain> domains;
        std::vector<queue_class> queues;
        std::vector<image_subresource_range> ranges;
        std::vector<uint32_t> event_begins;
    };

    struct buffer_access_rows
    {
        std::vector<pass_handle> passes;
        std::vector<resource_handle> logicals;
        std::vector<access_type> accesses;
        std::vector<buffer_usage> usages;
        std::vector<pipeline_domain> domains;
        std::vector<queue_class> queues;
        std::vector<buffer_byte_range> ranges;
        std::vector<uint32_t> event_begins;
    };

    // CSR DAG over passes. Successors of pass p are
    // adjacency_list[adjacency_begins[p], adjacency_begins[p + 1]) and
    // predecessors of pass p are rev_list[rev_begins[p], rev_begins[p + 1)).
    // in_degrees[p] counts predecessors (remaining work for Kahn's algorithm).
    struct dependency_graph
    {
        std::vector<pass_handle> adjacency_list;
        std::vector<uint32_t> adjacency_begins;
        std::vector<uint32_t> in_degrees;
        std::vector<pass_handle> rev_list;
        std::vector<uint32_t> rev_begins;
    };

    // =============================================================================
    // DAG construction and scheduling
    // =============================================================================

    void build_dependency_dag(const image_access_rows& image_events,
                              const buffer_access_rows& buffer_events,
                              uint32_t pass_count,
                              dependency_graph& graph);

    // Kahn-style topological order over the active sub-graph; returns false if
    // the (active) sub-graph contains a cycle.
    bool schedule_passes(const dependency_graph& graph,
                         std::span<const uint8_t> active_passes,
                         std::vector<pass_handle>& schedule);
} // namespace render_graph::core
