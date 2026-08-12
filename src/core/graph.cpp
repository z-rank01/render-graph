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
    }

    // =============================================================================
    // Dependency DAG construction
    // =============================================================================

    void build_dependency_dag(std::span<const access_event> events,
                              uint32_t pass_count,
                              dependency_graph& graph)
    {
        graph.outgoing.assign(pass_count, {});
        graph.in_degrees.assign(pass_count, 0);

        // For every earlier event that conflicts with this one on the same
        // resource, add an edge earlier pass -> later pass (deduplicated).
        for (uint32_t current = 0; current < events.size(); ++current)
        {
            const auto& after = events[current];
            for (uint32_t previous = 0; previous < current; ++previous)
            {
                const auto& before = events[previous];
                if (before.pass == after.pass || before.kind != after.kind ||
                    before.logical != after.logical || !conflicts(before.access, after.access))
                    continue;
                auto& edges = graph.outgoing[before.pass];
                if (std::ranges::find(edges, after.pass) == edges.end())
                {
                    edges.push_back(after.pass);
                    ++graph.in_degrees[after.pass];
                }
            }
        }
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
