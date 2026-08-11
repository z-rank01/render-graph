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
        using legacy_image_desc    = test_image_desc;
        using legacy_buffer_desc   = test_buffer_desc;
        using native_image_handle  = uintptr_t;
        using native_buffer_handle = uintptr_t;
        using command_context      = test_command_context;

        void set_context() {}

        static uint64_t hash_combine(uint64_t seed, uint64_t v) noexcept
        {
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }

        static image_desc normalize_image_desc(const legacy_image_desc& desc) noexcept
        {
            return image_desc{
                .fmt = desc.fmt,
                .extent = desc.extent,
                .usage = desc.usage,
                .type = desc.type,
                .flags = desc.flags,
                .mip_levels = desc.mip_levels,
                .array_layers = desc.array_layers,
                .samples = static_cast<sample_count>(desc.sample_counts),
                .memory = desc.memory_type_bits == 1 ? memory_domain::device_local : memory_domain::upload,
                .allocation = desc.requires_dedicated ? allocation_policy::dedicated : allocation_policy::automatic,
                .aliasing = desc.supports_aliasing ? aliasing_policy::automatic : aliasing_policy::forbidden,
            };
        }

        static buffer_desc normalize_buffer_desc(const legacy_buffer_desc& desc) noexcept
        {
            return buffer_desc{
                .size = desc.size,
                .usage = desc.usage,
                .memory = desc.memory_type_bits == 1 ? memory_domain::device_local : memory_domain::upload,
                .allocation = desc.requires_dedicated ? allocation_policy::dedicated : allocation_policy::automatic,
                .aliasing = desc.supports_aliasing ? aliasing_policy::automatic : aliasing_policy::forbidden,
            };
        }

        static uint64_t hash_image_desc(const image_desc& desc) noexcept
        {
            uint64_t hash = 0;
            hash = hash_combine(hash, static_cast<uint64_t>(desc.fmt));
            hash = hash_combine(hash, (static_cast<uint64_t>(desc.extent.width) << 32) | desc.extent.height);
            hash = hash_combine(hash, static_cast<uint64_t>(desc.extent.depth));
            hash = hash_combine(hash, static_cast<uint64_t>(desc.usage));
            hash = hash_combine(hash, static_cast<uint64_t>(desc.type));
            hash = hash_combine(hash, static_cast<uint64_t>(desc.flags));
            hash = hash_combine(hash, (static_cast<uint64_t>(desc.mip_levels) << 32) | desc.array_layers);
            hash = hash_combine(hash, static_cast<uint64_t>(desc.samples));
            hash = hash_combine(hash, static_cast<uint64_t>(desc.memory));
            hash = hash_combine(hash, static_cast<uint64_t>(desc.mapping));
            return hash;
        }

        static uint64_t hash_buffer_desc(const buffer_desc& desc) noexcept
        {
            uint64_t hash = 0;
            hash = hash_combine(hash, desc.size);
            hash = hash_combine(hash, static_cast<uint64_t>(desc.usage));
            hash = hash_combine(hash, static_cast<uint64_t>(desc.memory));
            hash = hash_combine(hash, static_cast<uint64_t>(desc.mapping));
            return hash;
        }

        static bool is_compatible_image(const image_desc& left, const image_desc& right) noexcept { return left == right; }
        static bool is_compatible_buffer(const buffer_desc& left, const buffer_desc& right) noexcept { return left == right; }
        static backend_capabilities capabilities() noexcept { return {}; }
        static resource_desc_diagnostic validate_image_desc(const image_desc& desc)
        {
            if (desc.mapping == mapping_policy::persistent && desc.memory == memory_domain::device_local)
                return {false, "test backend rejects persistent device-local images"};
            return {};
        }
        static resource_desc_diagnostic validate_buffer_desc(const buffer_desc& desc)
        {
            if (desc.mapping == mapping_policy::persistent && desc.memory == memory_domain::device_local)
                return {false, "test backend rejects persistent device-local buffers"};
            return {};
        }

        static uint32_t image_mip_levels(const image_desc& desc) noexcept { return desc.mip_levels; }
        static uint32_t image_array_layers(const image_desc& desc) noexcept { return desc.array_layers; }
        static uint64_t buffer_size(const buffer_desc& desc) noexcept { return desc.size; }
        static extent_3d image_extent(const image_desc& desc) noexcept { return desc.extent; }
        static uint32_t image_sample_count(const image_desc& desc) noexcept { return static_cast<uint32_t>(desc.samples); }
        static format image_format(const image_desc& desc) noexcept { return desc.fmt; }
        static bool is_depth_format(format value) noexcept { return value == format::D32_SFLOAT; }

        static allocation_requirements get_image_allocation_requirements(const image_desc& desc) noexcept
        {
            return allocation_requirements{
                .size = static_cast<uint64_t>(desc.extent.width) * desc.extent.height * desc.extent.depth * desc.array_layers * 4,
                .alignment = 256,
                .memory_type_bits = desc.memory == memory_domain::device_local ? 1U : 2U,
                .requires_dedicated = desc.allocation == allocation_policy::dedicated,
                .supports_aliasing = desc.aliasing != aliasing_policy::forbidden &&
                                     desc.allocation != allocation_policy::dedicated,
            };
        }

        static allocation_requirements get_buffer_allocation_requirements(const buffer_desc& desc) noexcept
        {
            return allocation_requirements{
                .size = desc.size,
                .alignment = 256,
                .memory_type_bits = desc.memory == memory_domain::device_local ? 1U : 2U,
                .requires_dedicated = desc.allocation == allocation_policy::dedicated,
                .supports_aliasing = desc.aliasing != aliasing_policy::forbidden &&
                                     desc.allocation != allocation_policy::dedicated,
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
