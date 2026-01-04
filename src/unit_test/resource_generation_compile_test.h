#pragma once

namespace render_graph::unit_test
{
    // Builds a more complex graph and validates (via debugger inspection) that:
    // - image_read_deps.generations / image_write_deps.generations
    // - buffer_read_deps.generations / buffer_write_deps.generations
    // match the expected generation stream for each dependency entry.
    void resource_generation_compile_test();
}
