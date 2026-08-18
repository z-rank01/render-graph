// Debug introspection: serializes a compiled_graph_plan into a deterministic
// JSON document (scheduled passes, derived dependency edges, synchronization
// ops, attachments, aliasing handoffs, resources, statistics) for external
// tooling such as a frame-debugger DAG view.
#pragma once

#include <string>

#include "compiler.h"
#include "visibility.h"

namespace render_graph
{
    // Pure function over the compiled plan. The output is deterministic for a
    // given plan (schedule/CSR iteration order, sorted deduplicated edges), so
    // two calls on the same plan compare byte-identical. Pass references
    // (edges/barriers/aliases/lifetimes) are emitted as schedule indices into
    // the passes array; unscheduled/invalid references become null.
    [[nodiscard]] RENDER_GRAPH_CORE_API std::string debug_dump(const compiled_graph_plan& plan);
} // namespace render_graph
