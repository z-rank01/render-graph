// =============================================================================
// compile_result: outcome of render graph compilation — a list of diagnostics;
// an empty list means success.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "barrier.h"
#include "resource.h"

namespace render_graph
{
    // --- Error codes ---

    enum class compile_error_code : uint8_t
    {
        none = 0,
        no_output,
        image_read_out_of_range,
        image_write_out_of_range,
        buffer_read_out_of_range,
        buffer_write_out_of_range,
        image_output_out_of_range,
        buffer_output_out_of_range,
        image_read_before_write,
        buffer_read_before_write,
        invalid_image_subresource_range,
        invalid_buffer_byte_range,
        raster_pass_has_no_attachments,
        raster_attachment_mismatch,
        raster_resolve_mismatch,
        raster_render_area_out_of_range,
        cycle_detected,
        pass_limit_exceeded,
        image_limit_exceeded,
        buffer_limit_exceeded,
        access_limit_exceeded,
        backend_failure,
        unsupported_feature,
    };

    // --- Diagnostic ---

    struct compile_diagnostic
    {
        compile_error_code code = compile_error_code::none;
        pass_handle pass         = invalid_pass;
        resource_kind kind       = resource_kind::image;
        resource_handle resource = invalid_resource;
        std::string pass_name;
        std::string resource_name;
        std::string message;
    };

    // --- Result ---

    struct compile_result
    {
        std::vector<compile_diagnostic> diagnostics;

        // Compilation succeeded iff no diagnostics were reported.
        [[nodiscard]] bool succeeded() const noexcept { return diagnostics.empty(); }
        [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    };
}
