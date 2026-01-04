#pragma once

namespace render_graph::unit_test
{
    // Simulates a minimal deferred rendering pipeline (GBuffer -> Lighting -> Tonemap -> Swapchain)
    // and invokes render_graph_system::add_pass + compile().
    // No output: inspect `render_graph_system` state in debugger after compile StepA.
    void deferred_rendering_compile_test();
}
