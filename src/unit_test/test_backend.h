#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "render_graph/barrier.h"
#include "render_graph/resource.h"
#include "render_graph/resource_types.h"
#include "render_graph/raster.h"

namespace render_graph::unit_test
{
    enum class test_command_kind : uint8_t
    {
        barrier_batch = 0,
        user_command,
        begin_rendering,
        end_rendering,
    };

    struct test_command_record
    {
        test_command_kind kind = test_command_kind::barrier_batch;
        synchronization_scope scope = synchronization_scope::pass_prologue;
        std::vector<resource_handle> resources;
    };

    struct test_command_context
    {
        std::vector<test_command_record> records;
        bool fail_next_barrier = false;

        void record_user_command()
        {
            records.push_back(test_command_record{.kind = test_command_kind::user_command});
        }
    };

    struct test_image_desc
    {
        format fmt             = format::UNDEFINED;
        extent_3d extent       = {.width = 1, .height = 1, .depth = 1};
        image_usage usage      = image_usage::NONE;
        image_type type        = image_type::TYPE_2D;
        image_flags flags      = image_flags::NONE;
        uint32_t mip_levels    = 1;
        uint32_t array_layers  = 1;
        uint32_t sample_counts = 1;
        uint64_t alignment     = 256;
        uint32_t memory_type_bits = 1;
        bool requires_dedicated = false;
        bool supports_aliasing  = true;
    };

    struct test_buffer_desc
    {
        uint64_t size      = 0;
        buffer_usage usage = buffer_usage::NONE;
        uint64_t alignment = 256;
        uint32_t memory_type_bits = 1;
        bool requires_dedicated = false;
        bool supports_aliasing = true;
    };

    struct test_backend
    {
        using image_desc           = test_image_desc;
        using buffer_desc          = test_buffer_desc;
        using native_image_handle  = uintptr_t;
        using native_buffer_handle = uintptr_t;
        using command_context      = test_command_context;

        void set_context() {}

        static uint64_t hash_combine(uint64_t seed, uint64_t v) noexcept
        {
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }

        static uint64_t hash_image_desc(const image_desc& d) noexcept
        {
            uint64_t h = 0;
            h = hash_combine(h, static_cast<uint64_t>(d.fmt));
            h = hash_combine(h, (static_cast<uint64_t>(d.extent.width) << 32) | d.extent.height);
            h = hash_combine(h, static_cast<uint64_t>(d.extent.depth));
            h = hash_combine(h, static_cast<uint64_t>(d.usage));
            h = hash_combine(h, static_cast<uint64_t>(d.type));
            h = hash_combine(h, static_cast<uint64_t>(d.flags));
            h = hash_combine(h, (static_cast<uint64_t>(d.mip_levels) << 32) | d.array_layers);
            h = hash_combine(h, static_cast<uint64_t>(d.sample_counts));
            return h;
        }

        static uint64_t hash_buffer_desc(const buffer_desc& d) noexcept
        {
            uint64_t h = 0;
            h = hash_combine(h, static_cast<uint64_t>(d.size));
            h = hash_combine(h, static_cast<uint64_t>(d.usage));
            return h;
        }

        static bool is_compatible_image(const image_desc& a, const image_desc& b) noexcept
        {
            return a.fmt == b.fmt && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.extent.depth == b.extent.depth &&
                   a.usage == b.usage && a.type == b.type && a.flags == b.flags && a.mip_levels == b.mip_levels && a.array_layers == b.array_layers &&
                   a.sample_counts == b.sample_counts;
        }

        static bool is_compatible_buffer(const buffer_desc& a, const buffer_desc& b) noexcept
        {
            return a.size == b.size && a.usage == b.usage;
        }

        static uint32_t image_mip_levels(const image_desc& desc) noexcept { return desc.mip_levels; }
        static uint32_t image_array_layers(const image_desc& desc) noexcept { return desc.array_layers; }
        static uint64_t buffer_size(const buffer_desc& desc) noexcept { return desc.size; }
        static extent_3d image_extent(const image_desc& desc) noexcept { return desc.extent; }
        static uint32_t image_sample_count(const image_desc& desc) noexcept { return desc.sample_counts; }
        static format image_format(const image_desc& desc) noexcept { return desc.fmt; }
        static bool is_depth_format(format value) noexcept { return value == format::D32_SFLOAT; }

        static allocation_requirements get_image_allocation_requirements(const image_desc& desc) noexcept
        {
            return allocation_requirements{
                .size = static_cast<uint64_t>(desc.extent.width) * desc.extent.height * desc.extent.depth * desc.array_layers * 4,
                .alignment = desc.alignment,
                .memory_type_bits = desc.memory_type_bits,
                .requires_dedicated = desc.requires_dedicated,
                .supports_aliasing = desc.supports_aliasing,
            };
        }

        static allocation_requirements get_buffer_allocation_requirements(const buffer_desc& desc) noexcept
        {
            return allocation_requirements{
                .size = desc.size,
                .alignment = desc.alignment,
                .memory_type_bits = desc.memory_type_bits,
                .requires_dedicated = desc.requires_dedicated,
                .supports_aliasing = desc.supports_aliasing,
            };
        }

        void bind_imported_image(image_handle logical, native_image_handle native) { imported_images[logical] = native; }
        void bind_imported_buffer(buffer_handle logical, native_buffer_handle native) { imported_buffers[logical] = native; }

        void begin_frame(uint64_t frame_index, uint64_t completed_frame)
        {
            begun_frames.push_back(frame_index);
            completed_frames.push_back(completed_frame);
        }

        void commit_frame() { commit_count++; }
        void abort_frame() { abort_count++; }

        template <typename MetaTableT>
        void on_compile_resource_allocation(const MetaTableT& /*meta*/, const physical_resource_meta& /*physical_meta*/)
        {
        }

        void apply_barriers(pass_handle /*pass*/, const per_pass_barrier& /*plan*/) {}

        bool emit_barriers(command_context& commands, std::span<const synchronization_op> barriers)
        {
            if (commands.fail_next_barrier)
            {
                commands.fail_next_barrier = false;
                return false;
            }
            test_command_record record{
                .kind = test_command_kind::barrier_batch,
                .scope = barriers.front().scope,
            };
            for (const auto& barrier : barriers)
            {
                record.resources.push_back(barrier.logical);
            }
            commands.records.push_back(std::move(record));
            return true;
        }

        bool begin_raster_pass(command_context& commands, const raster_pass_desc&)
        {
            commands.records.push_back(test_command_record{.kind = test_command_kind::begin_rendering});
            return true;
        }

        bool end_raster_pass(command_context& commands)
        {
            commands.records.push_back(test_command_record{.kind = test_command_kind::end_rendering});
            return true;
        }

        [[nodiscard]] native_image_handle get_image(image_handle logical) const
        {
            const auto found = imported_images.find(logical);
            return found == imported_images.end() ? 0 : found->second;
        }

        [[nodiscard]] native_buffer_handle get_buffer(buffer_handle logical) const
        {
            const auto found = imported_buffers.find(logical);
            return found == imported_buffers.end() ? 0 : found->second;
        }

        std::unordered_map<resource_handle, native_image_handle> imported_images;
        std::unordered_map<resource_handle, native_buffer_handle> imported_buffers;
        std::vector<uint64_t> begun_frames;
        std::vector<uint64_t> completed_frames;
        uint32_t commit_count = 0;
        uint32_t abort_count = 0;
    };
}
