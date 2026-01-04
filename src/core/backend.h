#pragma once

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
        virtual ~backend() = default;

        // Backend consumes the compiled plan to apply synchronization and execute passes.
        // Concrete backends implement lowering to API-specific synchronization primitives.
        // (Declared in barrier.h / plan types to avoid including any graphics API headers here.)

        // Apply all barriers that must happen before executing this pass.
        virtual void apply_barriers(pass_handle pass, const per_pass_barrier& plan) = 0;
    };
}
