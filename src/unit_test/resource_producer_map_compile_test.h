#pragma once

namespace render_graph::unit_test
{
    // Builds a small graph with:
    // - multiple images & buffers
    // - resources written by later passes (overwrite)
    // - an imported resource that is only read (no producer)
    // Then invokes render_graph_system::compile() and lets you inspect:
    // - render_graph_system::producer_lookup_table.*_version_offsets
    // - render_graph_system::producer_lookup_table.*_version_producers
    // - render_graph_system::producer_lookup_table.*_latest
    // - expected mappings stored in debugger-visible state
    void resource_producer_map_compile_test();
}
