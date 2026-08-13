// Graph dependency analysis: turns per-resource access events into a pass
// dependency DAG, then produces a topological execution schedule for passes.

#include "graph.h"

#include <algorithm>
#include <numeric>
#include <queue>
#include <vector>

namespace render_graph::core
{
    // --- Local helpers ---

    namespace
    {
        struct edge
        {
            pass_handle from;
            pass_handle to;
            auto operator<=>(const edge&) const = default;
        };

        // Versioned edge construction over one access-event table (image and
        // buffer tables are processed independently — rows never conflict
        // across kinds). Events arrive sorted by (pass, logical); a stable
        // counting sort by logical re-orders the row indices into per-resource
        // buckets, keeping pass order inside each bucket.
        //
        // Each bucket is then scanned in one linear pass maintaining:
        //   last_writer — most recent pass writing the resource;
        //   read_window — every pass that read the resource since the last
        //                 write (cleared by each write).
        // Emitted edges: read → edge(last_writer, pass) (RAW); write/read_write
        // → edge(last_writer, pass) (WAW) plus edge(reader, pass) for every
        // reader in the window (WAR). A read_write row keeps its RAW edge but
        // is not kept in the window — the WAW edge of a later write already
        // covers its WAR constraint.
        //
        // This emits exactly the non-transitively-redundant conflict edges of
        // the O(E²) pairwise scan: every conflicting pair (i, j) is either
        // emitted directly or reaches j via emitted edges (i → w → j), so
        // reachability, cycle detection and Kahn output stay unchanged.
        template <typename AccessRows>
        void collect_edges(const AccessRows& events, std::vector<edge>& edges)
        {
            const auto event_count = events.passes.size();
            if (event_count == 0)
                return;

            // --- Counting-bucket the rows by logical (stable → pass order kept) ---
            resource_handle max_logical = events.logicals[0];
            for (std::size_t row = 1; row < event_count; ++row)
                max_logical = std::max(max_logical, events.logicals[row]);
            const auto bucket_count = static_cast<std::size_t>(max_logical) + 1;

            std::vector<uint32_t> bucket_begins(bucket_count + 1, 0);
            for (std::size_t row = 0; row < event_count; ++row)
                ++bucket_begins[events.logicals[row] + 1];
            std::partial_sum(bucket_begins.begin(), bucket_begins.end(), bucket_begins.begin());

            std::vector<uint32_t> rows(event_count);
            {
                auto cursor = bucket_begins;
                cursor.pop_back();
                for (std::size_t row = 0; row < event_count; ++row)
                    rows[cursor[events.logicals[row]]++] = static_cast<uint32_t>(row);
            }

            // --- Per-bucket last_writer / read-window edge building ---
            edges.reserve(edges.size() + (event_count * 2));
            std::vector<pass_handle> read_window;
            for (std::size_t bucket = 0; bucket < bucket_count; ++bucket)
            {
                pass_handle last_writer = invalid_pass;
                read_window.clear();
                for (uint32_t index = bucket_begins[bucket]; index < bucket_begins[bucket + 1]; ++index)
                {
                    const auto row  = rows[index];
                    const auto pass = events.passes[row];
                    if (events.accesses[row] == access_type::read)
                    {
                        if (last_writer != invalid_pass)
                            edges.push_back({last_writer, pass});
                        read_window.push_back(pass);
                    }
                    else
                    {
                        if (last_writer != invalid_pass)
                            edges.push_back({last_writer, pass});
                        for (const auto reader : read_window)
                            edges.push_back({reader, pass});
                        last_writer = pass;
                        read_window.clear();
                    }
                }
            }
        }

        // Flat edge list → forward + reverse CSR. Sorting by (from, to) also
        // de-duplicates parallel edges and keeps each successor list ordered,
        // which makes Kahn's tie-breaking deterministic.
        void build_csr(std::vector<edge>& edges, uint32_t pass_count, dependency_graph& graph)
        {
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

            graph.adjacency_begins.assign(pass_count + 1, 0);
            for (const auto& edge : edges)
                ++graph.adjacency_begins[edge.from + 1];
            std::partial_sum(graph.adjacency_begins.begin(), graph.adjacency_begins.end(), graph.adjacency_begins.begin());

            graph.adjacency_list.resize(edges.size());
            {
                auto cursor = graph.adjacency_begins;
                cursor.pop_back();
                for (const auto& edge : edges)
                    graph.adjacency_list[cursor[edge.from]++] = edge.to;
            }

            graph.rev_begins.assign(pass_count + 1, 0);
            for (const auto& edge : edges)
                ++graph.rev_begins[edge.to + 1];
            std::partial_sum(graph.rev_begins.begin(), graph.rev_begins.end(), graph.rev_begins.begin());

            graph.rev_list.resize(edges.size());
            {
                auto cursor = graph.rev_begins;
                cursor.pop_back();
                for (const auto& edge : edges)
                    graph.rev_list[cursor[edge.to]++] = edge.from;
            }

            graph.in_degrees.resize(pass_count);
            for (uint32_t pass = 0; pass < pass_count; ++pass)
                graph.in_degrees[pass] = graph.rev_begins[pass + 1] - graph.rev_begins[pass];
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
        std::vector<edge> edges;
        collect_edges(image_events, edges);
        collect_edges(buffer_events, edges);
        build_csr(edges, pass_count, graph);
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
            for (uint32_t index = graph.adjacency_begins[pass]; index < graph.adjacency_begins[pass + 1]; ++index)
            {
                const auto destination = graph.adjacency_list[index];
                if (active_passes[destination] != 0U && --in_degrees[destination] == 0)
                    ready.push(destination);
            }
        }
        return schedule.size() == active_count;
    }
} // namespace render_graph::core
