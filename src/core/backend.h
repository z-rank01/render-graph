#pragma once

#include <cstdint>
#include <vector>

#include "barrier.h"
#include "resource.h"

namespace render_graph
{
    // Abstract interface for the render backend (Vulkan, DX12, Metal)
    // NOTE:
    // - Physical resource creation/lifetime is owned by the user side (outside the render graph).
    // - The render graph only builds an execution plan (including abstract barriers).
    class backend
    {
    public:
        using native_handle = uintptr_t;

        virtual ~backend() = default;

        // Called after render_graph_system::compile() finishes allocation/aliasing.
        // Backend may create transient physical resources based on the representative logical metas.
        virtual void on_compile_resource_allocation(const resource_meta_table& /*meta*/,
                                                    const physical_resource_meta& /*physical_meta*/)
        {
        }

        // Imported bindings (swapchain/backbuffer, externally owned resources).
        // Backends may defer binding until allocation mapping is known.
        virtual void bind_imported_image(resource_handle /*logical_image*/,
                                         native_handle /*native_image*/,
                                         native_handle /*native_view*/ = 0)
        {
        }

        virtual void bind_imported_buffer(resource_handle /*logical_buffer*/,
                                          native_handle /*native_buffer*/)
        {
        }

        // Backend consumes the compiled plan to apply synchronization and execute passes.
        // Concrete backends implement lowering to API-specific synchronization primitives.
        // (Declared in barrier.h / plan types to avoid including any graphics API headers here.)

        // Apply all barriers that must happen before executing this pass.
        virtual void apply_barriers(pass_handle pass, const per_pass_barrier& plan) = 0;
    };
}
