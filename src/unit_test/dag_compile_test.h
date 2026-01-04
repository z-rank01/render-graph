#pragma once

namespace render_graph::unit_test
{
    // Builds a small linear dependency chain and validates DAG CSR + degrees.
    void dag_compile_test();
}
