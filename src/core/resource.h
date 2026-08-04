#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "resource_types.h"

namespace render_graph
{
    using resource_version_handle = uint64_t; // high 32: version, low 32: index
    using resource_handle         = uint32_t;
    using version_handle          = uint32_t;
    using pass_handle             = uint32_t;

    inline constexpr resource_handle invalid_resource                 = std::numeric_limits<resource_handle>::max();
    inline constexpr version_handle invalid_version                   = std::numeric_limits<version_handle>::max();
    inline constexpr pass_handle invalid_pass                         = std::numeric_limits<pass_handle>::max();
    inline constexpr resource_version_handle invalid_resource_version = std::numeric_limits<resource_version_handle>::max();

    // resource version pack/unpack tool

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

    // DOD-style meta tables.
    // - desc 由用户/后端侧定义的“具体 API desc”提供（例如 VkImageCreateInfo / D3D12_RESOURCE_DESC）。
    // - RG 在 compile() 期只需要：
    //   1) desc_hash: 便于快速分组（alias/reuse 候选）
    //   2) imported/transient: 生命周期规则
    //   3) desc 本体：backend 在资源创建/兼容性校验时消费
    // NOTE: desc 的 hash/兼容性规则由 backend 维护；核心不假设 desc 的字段结构。

    template <typename ImageDesc>
    struct image_meta
    {
        using image_desc = ImageDesc;

        std::vector<std::string> names;
        std::vector<image_desc> descs;
        std::vector<uint64_t> desc_hashes;

        std::vector<bool> is_imported;
        std::vector<bool> is_transient;

        resource_handle add(const std::string& name, const image_desc& desc, bool imported, uint64_t desc_hash = 0)
        {
            const auto handle = static_cast<resource_handle>(names.size());
            names.push_back(name);
            descs.push_back(desc);
            desc_hashes.push_back(desc_hash);
            is_imported.push_back(imported);
            is_transient.push_back(!imported);
            return handle;
        }

        void clear()
        {
            names.clear();
            descs.clear();
            desc_hashes.clear();
            is_imported.clear();
            is_transient.clear();
        }
    };

    template <typename BufferDesc>
    struct buffer_meta
    {
        using buffer_desc = BufferDesc;

        std::vector<std::string> names;
        std::vector<buffer_desc> descs;
        std::vector<uint64_t> desc_hashes;

        std::vector<bool> is_imported;
        std::vector<bool> is_transient;

        resource_handle add(const std::string& name, const buffer_desc& desc, bool imported, uint64_t desc_hash = 0)
        {
            const auto handle = static_cast<resource_handle>(names.size());
            names.push_back(name);
            descs.push_back(desc);
            desc_hashes.push_back(desc_hash);
            is_imported.push_back(imported);
            is_transient.push_back(!imported);
            return handle;
        }

        void clear()
        {
            names.clear();
            descs.clear();
            desc_hashes.clear();
            is_imported.clear();
            is_transient.clear();
        }
    };

    template <typename ImageDesc, typename BufferDesc>
    struct resource_meta_table
    {
        using image_desc  = ImageDesc;
        using buffer_desc = BufferDesc;

        image_meta<image_desc> image_metas;
        buffer_meta<buffer_desc> buffer_metas;

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
        std::vector<pass_handle> image_first_used_pass;  // Indexed by resource handle
        std::vector<pass_handle> image_last_used_pass;   // Indexed by resource handle
        std::vector<pass_handle> buffer_first_used_pass; // Indexed by resource handle
        std::vector<pass_handle> buffer_last_used_pass;  // Indexed by resource handle

        void clear()
        {
            image_first_used_pass.clear();
            image_last_used_pass.clear();
            buffer_first_used_pass.clear();
            buffer_last_used_pass.clear();
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
