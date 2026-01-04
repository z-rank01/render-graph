#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "resource_types.h"

namespace render_graph
{
    using resource_version_handle = uint64_t; // high 32: version, low 32: index
    using resource_handle         = uint32_t;
    using version_handle          = uint32_t;
    using pass_handle             = uint32_t;

    // resource version pack/unpack tool

    inline constexpr resource_version_handle invalid_resource_version = 0xFFFFFFFFFFFFFFFFULL;

    [[nodiscard]] constexpr resource_version_handle pack(resource_handle index, version_handle version) noexcept
    {
        return (static_cast<resource_version_handle>(version) << 32) | static_cast<resource_version_handle>(index);
    }

    [[nodiscard]] constexpr resource_handle unpack_to_resource(resource_version_handle handle) noexcept
    {
        return static_cast<resource_handle>(handle & 0xFFFFFFFF);
    }

    [[nodiscard]] constexpr version_handle unpack_to_version(resource_version_handle handle) noexcept
    {
        return static_cast<version_handle>((handle >> 32) & 0xFFFFFFFF);
    }

    // Helper struct for user convenience
    struct image_info
    {
        std::string name;
        format fmt             = format::UNDEFINED;
        extent_3d extent       = {.width = 1, .height = 1, .depth = 1};
        image_usage usage      = image_usage::NONE;
        image_type type        = image_type::TYPE_2D;
        image_flags flags      = image_flags::NONE;
        uint32_t mip_levels    = 1;
        uint32_t array_layers  = 1;
        uint32_t sample_counts = 1;
        bool imported;
    };

    struct buffer_info
    {
        std::string name;
        uint64_t size      = 0;
        buffer_usage usage = buffer_usage::NONE;
        bool imported;
    };

    // Meta Table for Images (SoA)
    // Stores all creation information required to create the physical resource later.
    struct image_meta
    {
        // Generic properties (Cross-API)
        std::vector<std::string> names;
        std::vector<format> formats;
        std::vector<extent_3d> extents;
        std::vector<image_usage> usages;
        std::vector<image_type> types;
        std::vector<image_flags> flags;
        std::vector<uint32_t> mip_levels;
        std::vector<uint32_t> array_layers;
        std::vector<uint32_t> sample_counts;

        // Lifecycle / Graph properties
        std::vector<bool> is_imported;  // If true, handle is provided externally (backbuffer, etc.)
        std::vector<bool> is_transient; // If true, memory can be aliased/lazy allocated

        // Helper to add a new image meta and return its resource index(not versioned)
        resource_handle add(const image_info& info)
        {
            auto handle = static_cast<resource_handle>(names.size());
            names.push_back(info.name);
            formats.push_back(info.fmt);
            extents.push_back(info.extent);
            usages.push_back(info.usage);
            types.push_back(info.type);
            flags.push_back(info.flags);
            mip_levels.push_back(info.mip_levels);
            array_layers.push_back(info.array_layers);
            sample_counts.push_back(info.sample_counts);

            // Defaults
            is_imported.push_back(info.imported);
            is_transient.push_back(!info.imported);

            return handle;
        }

        [[nodiscard]] bool is_compatible(resource_handle a, resource_handle b) const noexcept
        {
            const auto count = static_cast<resource_handle>(names.size());
            if (a >= count || b >= count)
            {
                return false;
            }

            const auto& ea = extents[a];
            const auto& eb = extents[b];
            return formats[a] == formats[b] && ea.width == eb.width && ea.height == eb.height && ea.depth == eb.depth && usages[a] == usages[b] &&
                   types[a] == types[b] && flags[a] == flags[b] && mip_levels[a] == mip_levels[b] && array_layers[a] == array_layers[b] &&
                   sample_counts[a] == sample_counts[b];
        }

        void clear()
        {
            names.clear();
            formats.clear();
            extents.clear();
            usages.clear();
            types.clear();
            flags.clear();
            mip_levels.clear();
            array_layers.clear();
            sample_counts.clear();
            is_imported.clear();
            is_transient.clear();
        }
    };

    // Meta Table for Buffers (SoA)
    struct buffer_meta
    {
        std::vector<std::string> names;
        std::vector<uint64_t> sizes;
        std::vector<buffer_usage> usages;

        // Lifecycle / Graph properties
        std::vector<bool> is_imported;  // If true, handle is provided externally (backbuffer, etc.)
        std::vector<bool> is_transient; // If true, memory can be aliased/lazy allocated

        // Helper to add a new buffer meta and return its resource index(not versioned)
        resource_handle add(const buffer_info& info)
        {
            auto handle = static_cast<resource_handle>(names.size());
            names.push_back(info.name);
            sizes.push_back(info.size);
            usages.push_back(info.usage);
            is_imported.push_back(info.imported);
            is_transient.push_back(!info.imported);
            return handle;
        }

        [[nodiscard]] bool is_compatible(resource_handle a, resource_handle b) const noexcept
        {
            const auto count = static_cast<resource_handle>(names.size());
            if (a >= count || b >= count)
            {
                return false;
            }
            return sizes[a] == sizes[b] && usages[a] == usages[b];
        }

        void clear()
        {
            names.clear();
            sizes.clear();
            usages.clear();
        }
    };

    // The main registry that holds all resource descriptions
    struct resource_meta_table
    {
        image_meta image_metas;
        buffer_meta buffer_metas;

        void clear()
        {
            image_metas.clear();
            buffer_metas.clear();
        }
    };

    // Version -> producer lookup in DOD (flat array) form.
    //
    // For each resource_handle h, all its versions [0..N) occupy a contiguous range:
    //   base = *_version_offsets[h]
    //   producer(h, v) = *_version_producers[ base + v ]
    // with version count N = offsets[h+1] - offsets[h].
    //
    // NOTE: resource_version_handle (packed u64) is NOT a valid vector index.
    // Always unpack to (resource_handle, version_handle) first.
    struct version_producer_map
    {
        // Images
        std::vector<uint32_t> img_version_offsets;       // size = image_count + 1
        std::vector<pass_handle> img_version_producers;  // size = total image versions
        std::vector<resource_version_handle> latest_img; // size = image_count, pack(h, latest_version)

        // Buffers
        std::vector<uint32_t> buf_version_offsets;       // size = buffer_count + 1
        std::vector<pass_handle> buf_version_producers;  // size = total buffer versions
        std::vector<resource_version_handle> latest_buf; // size = buffer_count, pack(h, latest_version)

        void clear()
        {
            img_version_offsets.clear();
            img_version_producers.clear();
            latest_img.clear();
            buf_version_offsets.clear();
            buf_version_producers.clear();
            latest_buf.clear();
        }
    };

    struct output_table
    {
        std::vector<resource_handle> image_outputs;  // Indexed by image handle
        std::vector<resource_handle> buffer_outputs; // Indexed by buffer handle
    };

    struct resource_lifetime
    {
        std::vector<pass_handle> image_first_used_pass; // Indexed by resource handle
        std::vector<pass_handle> image_last_used_pass;  // Indexed by resource handle
        std::vector<pass_handle> buffer_first_used_pass; // Indexed by resource handle
        std::vector<pass_handle> buffer_last_used_pass;  // Indexed by resource handle

        void clear()
        {
            image_first_used_pass.clear();
            image_last_used_pass.clear();
        }
    };

    struct physical_resource_meta
    {
        std::vector<resource_handle> physical_image_meta;
        std::vector<uint32_t> handle_to_physical_img_id; // Indexed by resource_handle
        std::vector<resource_handle> physical_buffer_meta;
        std::vector<uint32_t> handle_to_physical_buf_id; // Indexed by resource_handle

        void clear()
        {
            physical_image_meta.clear();
            physical_buffer_meta.clear();
            handle_to_physical_img_id.clear();
            handle_to_physical_buf_id.clear();
        }
    };

} // namespace render_graph
