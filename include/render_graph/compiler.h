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
    struct compile_environment
    {
        extent_3d extent{};
        format color_format = format::UNDEFINED;
        bool swapchain_initialized = false;
        queue_availability queues{};
        render_graph_limits limits{};
    };

    struct resource_description_api
    {
        void* state = nullptr;
        bool (*describe_buffer)(void*, device_buffer_handle, buffer_desc&) = nullptr;
        bool (*describe_image)(void*, device_image_handle, image_desc&) = nullptr;
    };

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

    struct compiled_pass_row
    {
        std::string name;
        pass_kind kind = pass_kind::raster;
        queue_class queue = queue_class::graphics;
        uint32_t source_pass = std::numeric_limits<uint32_t>::max();
        bool backend_upload = false;
        raster_pass_desc raster{};
    };

    template <typename Desc, typename Handle>
    struct compiled_resource_rows
    {
        std::vector<std::string> names;
        std::vector<Desc> descs;
        std::vector<uint64_t> desc_hashes;
        std::vector<bool> is_imported;
        std::vector<resource_lifetime_class> lifetime_classes;

        Handle add(std::string name, const Desc& desc, resource_lifetime_class lifetime,
                   uint64_t hash, bool imported = false)
        {
            const auto handle = Handle{static_cast<uint32_t>(descs.size())};
            names.push_back(std::move(name));
            descs.push_back(desc);
            desc_hashes.push_back(hash);
            is_imported.push_back(imported || lifetime == resource_lifetime_class::imported);
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
        resource_realization_rows resources;
        std::vector<buffer_handle> frame_buffers;
        std::vector<image_handle> frame_images;
        buffer_handle upload_buffer = invalid_buffer;
        std::vector<compiled_pass_row> passes;
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

    [[nodiscard]] RENDER_GRAPH_CORE_API graph_compile_output compile_graph(const graph_compile_request& request);
} // namespace render_graph
