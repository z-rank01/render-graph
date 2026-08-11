#pragma once

#include <cstdint>

// This header intentionally contains *no* virtual backend interface.
//
// Physical execution remains a compile-time adapter used by render_graph_system<BackendT>.
// Compilation itself consumes only API-independent resource descriptions and explicit
// resource_validation_api callbacks; it does not query native backend types.
//
// A BackendT is expected to provide:
// - types:
//   - native_image_handle, native_buffer_handle
//   - command_context
// - setup:
//   - set_context(...)
//   - bind_imported_image(resource_handle, native_image_handle)
//   - bind_imported_buffer(resource_handle, native_buffer_handle)
// - realization: on_compile_resource_allocation(const MetaTableT&, const physical_resource_meta&)
// - execute:
//   - emit_barriers(command_context&, span<const synchronization_op>) -> bool
//   - get_image(resource_handle)  -> native_image_handle
//   - get_buffer(resource_handle) -> native_buffer_handle

namespace render_graph
{
    struct backend
    {
        using native_handle = uintptr_t;
    };
}
