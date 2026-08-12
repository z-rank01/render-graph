// Minimal diagnostic plumbing for the render graph: a severity level plus a
// type-erased sink callback. A default-constructed sink is a silent no-op, so
// emit() can be called unconditionally at diagnostic sites.
#pragma once

#include <cstdint>
#include <string_view>

namespace render_graph
{
    // Severity levels, ordered from least to most severe.
    enum class diagnostic_severity : uint8_t
    {
        info = 0,
        warning,
        error,
    };

    // Type-erased output target: opaque state plus a free-function write
    // callback. Copyable and cheap to pass around by value.
    struct diagnostic_sink
    {
        void* state = nullptr;
        void (*write)(void*, diagnostic_severity, std::string_view) = nullptr;

        // Forwards the message to the sink; no-op when no callback is set.
        void emit(diagnostic_severity severity, std::string_view message) const noexcept
        {
            if (write != nullptr) write(state, severity, message);
        }

        // True when a sink is actually installed (i.e. emit() will do something).
        [[nodiscard]] explicit operator bool() const noexcept { return write != nullptr; }
    };
} // namespace render_graph
