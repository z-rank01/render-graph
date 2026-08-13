// Compiler interface: compiles a frame_plan into a compiled_graph_plan —
// realized resources, scheduled passes, synchronization and submissions —
// ready for the backend to execute.
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "barrier.h"
#include "compile_result.h"
#include "hardening.h"
#include "render_device.h"
#include "submission.h"
#include "visibility.h"

namespace render_graph
{
    // =============================================================================
    // Compile inputs: environment, host callbacks, and request
    // =============================================================================

    struct compile_environment
    {
        extent_3d extent{};
        format color_format = format::UNDEFINED;
        bool swapchain_initialized = false;
        queue_availability queues{};
        render_graph_limits limits{};
    };

    // Resolves device resource handles into their descs (host-provided callbacks).
    struct resource_description_api
    {
        void* state = nullptr;
        bool (*describe_buffer)(void*, device_buffer_handle, buffer_desc&) = nullptr;
        bool (*describe_image)(void*, device_image_handle, image_desc&) = nullptr;
    };

    // Reports allocation requirements for a desc (host-provided callbacks).
    struct resource_allocation_api
    {
        void* state = nullptr;
        allocation_requirements (*image_requirements)(void*, const image_desc&) = nullptr;
        allocation_requirements (*buffer_requirements)(void*, const buffer_desc&) = nullptr;
    };

    struct graph_compile_request
    {
        const frame_plan* frame = nullptr;
        compile_environment environment{};
        backend_capabilities capabilities{};
        resource_validation_api validation{};
        resource_description_api descriptions{};
        resource_allocation_api allocations{};
        bool inject_stable_upload_pass = false;
        buffer_desc upload_buffer_desc{};
    };

    // =============================================================================
    // Compiled output: SoA resource rows and the final graph plan
    // =============================================================================

    // Pass flag bits, packed into the `flags` column of compiled_pass_rows.
    inline constexpr uint8_t pass_flag_backend_upload = 0x01U; // synthesized upload pass (always culling root)
    inline constexpr uint8_t pass_flag_side_effect    = 0x02U; // copied from frame_pass_row; marks culling root

    // Compiled pass rows (SoA). Rows are appended in frame order during
    // build_resource_versions and physically compacted by culling: inactive
    // rows are removed and every column stays index-aligned with `pass_handle`.
    // Raster attachments use a per-pass CSR (colors of pass p are
    // colors[color_begins[p], color_begins[p] + color_counts[p])) and a depth
    // sentinel column (pass p has no depth attachment when
    // depth_indices[p] == invalid_depth_index).
    struct compiled_pass_rows
    {
        // --- Scalar columns, one row per pass ---
        std::vector<std::string> names;
        std::vector<uint64_t> name_hashes;   // FNV-1a over the pass name (cache-key column)
        std::vector<pass_kind> kinds;
        std::vector<queue_class> queues;
        std::vector<uint32_t> source_passes; // frame pass index; max = synthesized upload pass
        std::vector<uint8_t> flags;          // pass_flag_* bits
        std::vector<render_area> areas;
        std::vector<uint32_t> layer_counts;

        // --- Raster attachments, CSR over passes ---
        std::vector<uint32_t> color_begins;
        std::vector<uint32_t> color_counts;
        std::vector<raster_attachment> colors;
        std::vector<uint32_t> depth_indices;
        std::vector<raster_attachment> depths;

        [[nodiscard]] std::size_t size() const noexcept { return kinds.size(); }

        [[nodiscard]] bool is_backend_upload(std::size_t pass) const noexcept
        {
            return (flags[pass] & pass_flag_backend_upload) != 0;
        }
        [[nodiscard]] bool is_side_effect(std::size_t pass) const noexcept
        {
            return (flags[pass] & pass_flag_side_effect) != 0;
        }

        void clear()
        {
            names.clear();
            name_hashes.clear();
            kinds.clear();
            queues.clear();
            source_passes.clear();
            flags.clear();
            areas.clear();
            layer_counts.clear();
            color_begins.clear();
            color_counts.clear();
            colors.clear();
            depth_indices.clear();
            depths.clear();
        }
    };

    inline constexpr uint32_t invalid_depth_index = std::numeric_limits<uint32_t>::max();

    template <typename Desc, typename Handle>
    struct compiled_resource_rows
    {
        std::vector<std::string> names;
        std::vector<Desc> descs;
        std::vector<uint64_t> desc_hashes;
        std::vector<uint8_t> is_imported; // packed flag column (SoA, no proxy object)
        std::vector<resource_lifetime_class> lifetime_classes;

        Handle add(std::string name, const Desc& desc, resource_lifetime_class lifetime,
                   uint64_t hash, bool imported = false)
        {
            // The handle is the row index; a resource with imported lifetime is
            // always marked imported regardless of the `imported` argument.
            const auto handle = Handle{static_cast<uint32_t>(descs.size())};
            names.push_back(std::move(name));
            descs.push_back(desc);
            desc_hashes.push_back(hash);
            is_imported.push_back(static_cast<uint8_t>(imported || lifetime == resource_lifetime_class::imported));
            lifetime_classes.push_back(lifetime);
            return handle;
        }

        void clear()
        {
            names.clear();
            descs.clear();
            desc_hashes.clear();
            is_imported.clear();
            lifetime_classes.clear();
        }
    };

    struct resource_realization_rows
    {
        compiled_resource_rows<image_desc, image_handle> image_metas;
        compiled_resource_rows<buffer_desc, buffer_handle> buffer_metas;
        void clear() { image_metas.clear(); buffer_metas.clear(); }
    };

    struct compiled_graph_plan
    {
        uint64_t cache_key = 0;

        // --- Realized resources referenced by this plan ---
        resource_realization_rows resources;
        std::vector<buffer_handle> frame_buffers;
        std::vector<image_handle> frame_images;
        buffer_handle upload_buffer = invalid_buffer;

        // --- Scheduled passes and derived plans ---
        compiled_pass_rows passes;
        std::vector<pass_handle> scheduled_passes;
        resource_lifetime lifetimes;
        physical_resource_meta physical_resources;
        synchronization_plan synchronization;
        submission_plan submissions;
        render_graph_statistics statistics;

        RENDER_GRAPH_CORE_API void clear();
    };

    struct graph_compile_output
    {
        compiled_graph_plan plan;
        compile_result result;

        [[nodiscard]] bool succeeded() const noexcept { return result.succeeded(); }
        [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    };

    // =============================================================================
    // Public entry point
    // =============================================================================

    [[nodiscard]] RENDER_GRAPH_CORE_API graph_compile_output compile_graph(const graph_compile_request& request);
} // namespace render_graph
