#pragma once

#include <cstdint>

// This header intentionally contains *no* virtual backend interface.
//
// The render-graph core is template-based (render_graph_system<BackendT>), and BackendT is a
// compile-time concept rather than a runtime-polymorphic base class.
//
// A BackendT is expected to provide:
// - types:
//   - image_desc, buffer_desc                (user-provided, API-specific resource descriptions)
//   - native_image_handle, native_buffer_handle
// - setup:
//   - set_context(...)
//   - bind_imported_image(resource_handle, native_image_handle)
//   - bind_imported_buffer(resource_handle, native_buffer_handle)
// - compile:
//   - static hash_image_desc(const image_desc&) -> uint64_t
//   - static hash_buffer_desc(const buffer_desc&) -> uint64_t
//   - static is_compatible_image(const image_desc&, const image_desc&) -> bool
//   - static is_compatible_buffer(const buffer_desc&, const buffer_desc&) -> bool
//   - on_compile_resource_allocation(const MetaTableT&, const physical_resource_meta&)
// - execute:
//   - apply_barriers(pass_handle, const per_pass_barrier&)
//   - get_image(resource_handle)  -> native_image_handle
//   - get_buffer(resource_handle) -> native_buffer_handle

namespace render_graph
{
    struct backend
    {
        using native_handle = uintptr_t; // legacy convenience alias (optional)
    };
}
