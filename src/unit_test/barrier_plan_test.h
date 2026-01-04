#pragma once

namespace render_graph::unit_test
{
    // Builds a multi-stage pipeline (compute -> gbuffer -> lighting -> tonemap -> present)
    // and validates the generated per-pass barrier plan (CSR + SoA).
    void barrier_plan_test();
}
