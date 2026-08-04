#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "resource.h"

namespace render_graph
{
    enum class execute_error_code : uint8_t
    {
        none = 0,
        graph_not_compiled,
        unexpected_explicit_barrier,
        explicit_barrier_out_of_order,
        missing_explicit_barrier,
        backend_failure,
    };

    struct execute_diagnostic
    {
        execute_error_code code = execute_error_code::none;
        pass_handle pass = invalid_pass;
        resource_kind kind = resource_kind::image;
        resource_handle resource = invalid_resource;
        std::string pass_name;
        std::string message;
    };

    struct execute_result
    {
        std::vector<execute_diagnostic> diagnostics;

        [[nodiscard]] bool succeeded() const noexcept { return diagnostics.empty(); }
        [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    };
}
