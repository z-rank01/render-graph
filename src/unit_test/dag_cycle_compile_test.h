#pragma once

namespace render_graph::unit_test
{
    // Validates cycle detection logic:
    // - case 0: acyclic render graph compile should pass
    // - case 1: inject a cyclic DAG and expect an assert
    void dag_cycle_compile_test();
}
