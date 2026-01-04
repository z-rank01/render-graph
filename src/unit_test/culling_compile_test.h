#pragma once

namespace render_graph::unit_test
{
    // Builds a graph with multiple branches and outputs, then compiles.
    // Inspect render_graph_system::active_pass_flags in debugger after compile StepD.
    // Also inspect debugger-visible expected flags stored in test state.
    void culling_compile_test();
}
