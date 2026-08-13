// Graph dependency analysis: turns per-resource access events into a pass
// dependency DAG, then produces a topological execution schedule for passes.

#include "graph.h"

#include <algorithm>
#include <queue>

namespace render_graph::core
{
    // --- Local helpers ---

    namespace
    {
        // Only a read/read pair is conflict-free; any write involved creates an edge.
        bool conflicts(access_type left, access_type right) noexcept
        {
            return left != access_type::read || right != access_type::read;
        }

        // O(E²) pairwise edge construction over one access-event table
        // (image and buffer tables are processed independently — rows never
        // conflict across kinds). P2 replaces this with a counting-bucket
        // versioned builder; the edge semantics must stay identical.
        template <typename AccessRows>
        void add_conflict_edges(const AccessRows& events, dependency_graph& graph)
        {
            const auto event_count = events.passes.size();
            for (std::size_t current = 0; current < event_count; ++current)
            {
                const auto& after_pass = events.passes[current];
                const auto& after_logical = events.logicals[current];
                for (std::size_t previous = 0; previous < current; ++previous)
                {
                    const auto& before_pass = events.passes[previous];
                    if (before_pass == after_pass || events.logicals[previous] != after_logical ||
                        !conflicts(events.accesses[previous], events.accesses[current]))
                        continue;
                    auto& edges = graph.outgoing[before_pass];
                    if (std::ranges::find(edges, after_pass) == edges.end())
                    {
                        edges.push_back(after_pass);
                        ++graph.in_degrees[after_pass];
                    }
                }
            }
        }
    }

    // =============================================================================
    // Dependency DAG construction
    // =============================================================================

    void build_dependency_dag(const image_access_rows& image_events,
                              const buffer_access_rows& buffer_events,
                              uint32_t pass_count,
                              dependency_graph& graph)
    {
        graph.outgoing.assign(pass_count, {});
        graph.in_degrees.assign(pass_count, 0);
        add_conflict_edges(image_events, graph);
        add_conflict_edges(buffer_events, graph);
    }

    // =============================================================================
    // Pass scheduling (topological sort)
    // =============================================================================

    // Kahn's algorithm with a min-heap over the active sub-graph. Ties between
    // ready passes are broken by lowest pass handle. Returns false if the
    // active sub-graph contains a cycle.
    bool schedule_passes(const dependency_graph& graph,
                         std::span<const uint8_t> active_passes,
                         std::vector<pass_handle>& schedule)
    {
        auto in_degrees = graph.in_degrees;
        std::priority_queue<pass_handle, std::vector<pass_handle>, std::greater<>> ready;

        uint32_t active_count = 0;
        for (pass_handle pass = 0; pass < in_degrees.size(); ++pass)
        {
            if (active_passes[pass] == 0U) continue;
            ++active_count;
            if (in_degrees[pass] == 0) ready.push(pass);
        }

        schedule.clear();
        while (!ready.empty())
        {
            const auto pass = ready.top();
            ready.pop();
            schedule.push_back(pass);
            for (const auto destination : graph.outgoing[pass])
                if (active_passes[destination] != 0U && --in_degrees[destination] == 0) ready.push(destination);
        }
        return schedule.size() == active_count;
    }
} // namespace render_graph::core
