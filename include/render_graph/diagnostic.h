#pragma once

#include <cstdint>
#include <string_view>

namespace render_graph
{
    enum class diagnostic_severity : uint8_t
    {
        info = 0,
        warning,
        error,
    };

    struct diagnostic_sink
    {
        void* state = nullptr;
        void (*write)(void*, diagnostic_severity, std::string_view) = nullptr;

        void emit(diagnostic_severity severity, std::string_view message) const noexcept
        {
            if (write != nullptr) write(state, severity, message);
        }

        [[nodiscard]] explicit operator bool() const noexcept { return write != nullptr; }
    };
} // namespace render_graph
