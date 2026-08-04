#pragma once

#include <algorithm>
#include <cstdint>
#include <compare>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "resource_types.h"

namespace render_graph
{
    template <typename Tag>
    struct typed_handle
    {
        uint32_t value = 0;

        constexpr typed_handle() noexcept = default;
        constexpr typed_handle(uint32_t value_in) noexcept : value(value_in) { }

        // Index conversion keeps the SoA/CSR implementation compact while the explicit
        // constructor prevents image/buffer/pass handles from being mixed at API calls.
        [[nodiscard]] constexpr operator uint32_t() const noexcept { return value; }
        constexpr typed_handle& operator++() noexcept
        {
            ++value;
            return *this;
        }
        constexpr typed_handle operator++(int) noexcept
        {
            const auto previous = *this;
            ++value;
            return previous;
        }
        [[nodiscard]] constexpr auto operator<=>(const typed_handle&) const noexcept = default;

        template <typename Integer>
            requires std::is_integral_v<Integer>
        [[nodiscard]] friend constexpr bool operator==(typed_handle left, Integer right) noexcept
        {
            return left.value == static_cast<uint32_t>(right);
        }

        template <typename Integer>
            requires std::is_integral_v<Integer>
        [[nodiscard]] friend constexpr bool operator==(Integer left, typed_handle right) noexcept
        {
            return right == left;
        }

        template <typename Integer>
            requires std::is_integral_v<Integer>
        [[nodiscard]] friend constexpr bool operator<(typed_handle left, Integer right) noexcept
        {
            return static_cast<uint64_t>(left.value) < static_cast<uint64_t>(right);
        }

        template <typename Integer>
            requires std::is_integral_v<Integer>
        [[nodiscard]] friend constexpr bool operator<=(typed_handle left, Integer right) noexcept
        {
            return static_cast<uint64_t>(left.value) <= static_cast<uint64_t>(right);
        }

        template <typename Integer>
            requires std::is_integral_v<Integer>
        [[nodiscard]] friend constexpr bool operator>(typed_handle left, Integer right) noexcept
        {
            return static_cast<uint64_t>(left.value) > static_cast<uint64_t>(right);
        }

        template <typename Integer>
            requires std::is_integral_v<Integer>
        [[nodiscard]] friend constexpr bool operator>=(typed_handle left, Integer right) noexcept
        {
            return static_cast<uint64_t>(left.value) >= static_cast<uint64_t>(right);
        }

        template <typename Integer>
            requires std::is_integral_v<Integer>
        [[nodiscard]] friend constexpr typed_handle operator+(typed_handle left, Integer right) noexcept
        {
            return typed_handle{static_cast<uint32_t>(left.value + right)};
        }
    };

    struct image_handle_tag;
    struct buffer_handle_tag;
    struct pass_handle_tag;

    using image_handle  = typed_handle<image_handle_tag>;
    using buffer_handle = typed_handle<buffer_handle_tag>;
    using pass_handle   = typed_handle<pass_handle_tag>;

    using resource_version_handle = uint64_t; // high 32: version, low 32: index
    using resource_handle         = uint32_t;
    using version_handle          = uint32_t;

    inline constexpr resource_handle invalid_resource                 = std::numeric_limits<resource_handle>::max();
    inline constexpr version_handle invalid_version                   = std::numeric_limits<version_handle>::max();
    inline constexpr image_handle invalid_image{std::numeric_limits<uint32_t>::max()};
    inline constexpr buffer_handle invalid_buffer{std::numeric_limits<uint32_t>::max()};
    inline constexpr pass_handle invalid_pass{std::numeric_limits<uint32_t>::max()};
    inline constexpr resource_version_handle invalid_resource_version = std::numeric_limits<resource_version_handle>::max();

    enum class resource_kind : uint8_t
    {
        image = 0,
        buffer,
    };

    enum class access_type : uint8_t
    {
        read = 0,
        write,
        read_write,
    };

    enum class pipeline_domain : uint8_t
    {
        any = 0,
        graphics,
        compute,
        copy,
    };

    enum class image_aspect : uint8_t
    {
        none    = 0,
        color   = 1 << 0,
        depth   = 1 << 1,
        stencil = 1 << 2,
        all     = (1 << 0) | (1 << 1) | (1 << 2),
    };

    [[nodiscard]] constexpr image_aspect operator|(image_aspect left, image_aspect right) noexcept
    {
        return static_cast<image_aspect>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
    }

    [[nodiscard]] constexpr image_aspect operator&(image_aspect left, image_aspect right) noexcept
    {
        return static_cast<image_aspect>(static_cast<uint8_t>(left) & static_cast<uint8_t>(right));
    }

    inline constexpr uint32_t remaining_subresources = std::numeric_limits<uint32_t>::max();
    inline constexpr uint64_t whole_buffer_size      = std::numeric_limits<uint64_t>::max();

    struct image_subresource_range
    {
        image_aspect aspects       = image_aspect::all;
        uint32_t base_mip_level    = 0;
        uint32_t mip_level_count   = remaining_subresources;
        uint32_t base_array_layer  = 0;
        uint32_t array_layer_count = remaining_subresources;

        [[nodiscard]] constexpr auto operator<=>(const image_subresource_range&) const noexcept = default;
    };

    struct buffer_byte_range
    {
        uint64_t offset = 0;
        uint64_t size   = whole_buffer_size;

        [[nodiscard]] constexpr auto operator<=>(const buffer_byte_range&) const noexcept = default;
    };

    struct image_access_desc
    {
        image_usage usage                = image_usage::NONE;
        pipeline_domain domain            = pipeline_domain::any;
        image_subresource_range subresource{};

        [[nodiscard]] constexpr auto operator<=>(const image_access_desc&) const noexcept = default;
    };

    struct buffer_access_desc
    {
        buffer_usage usage     = buffer_usage::NONE;
        pipeline_domain domain = pipeline_domain::any;
        buffer_byte_range bytes{};

        [[nodiscard]] constexpr auto operator<=>(const buffer_access_desc&) const noexcept = default;
    };

    using resource_ref = std::variant<image_handle, buffer_handle>;

    [[nodiscard]] constexpr uint64_t range_end(uint64_t begin, uint64_t count) noexcept
    {
        if (count == whole_buffer_size || count > whole_buffer_size - begin)
        {
            return whole_buffer_size;
        }
        return begin + count;
    }

    [[nodiscard]] constexpr uint64_t range_end(uint32_t begin, uint32_t count) noexcept
    {
        if (count == remaining_subresources || count > remaining_subresources - begin)
        {
            return remaining_subresources;
        }
        return static_cast<uint64_t>(begin) + count;
    }

    [[nodiscard]] constexpr bool overlaps(const buffer_byte_range& left, const buffer_byte_range& right) noexcept
    {
        return left.offset < range_end(right.offset, right.size) && right.offset < range_end(left.offset, left.size);
    }

    [[nodiscard]] constexpr bool overlaps(const image_subresource_range& left, const image_subresource_range& right) noexcept
    {
        const auto common_aspects = static_cast<uint8_t>(left.aspects & right.aspects);
        const bool mip_overlap = left.base_mip_level < range_end(right.base_mip_level, right.mip_level_count) &&
                                 right.base_mip_level < range_end(left.base_mip_level, left.mip_level_count);
        const bool layer_overlap = left.base_array_layer < range_end(right.base_array_layer, right.array_layer_count) &&
                                   right.base_array_layer < range_end(left.base_array_layer, left.array_layer_count);
        return common_aspects != 0 && mip_overlap && layer_overlap;
    }

    [[nodiscard]] inline bool fully_covers(const std::vector<buffer_byte_range>& writes, const buffer_byte_range& query)
    {
        const auto query_end = range_end(query.offset, query.size);
        auto cursor = query.offset;
        std::vector<buffer_byte_range> sorted = writes;
        std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) { return left.offset < right.offset; });
        for (const auto& write : sorted)
        {
            const auto write_end = range_end(write.offset, write.size);
            if (write_end <= cursor || write.offset >= query_end)
            {
                continue;
            }
            if (write.offset > cursor)
            {
                return false;
            }
            cursor = std::max(cursor, write_end);
            if (cursor >= query_end)
            {
                return true;
            }
        }
        return cursor >= query_end;
    }

    [[nodiscard]] inline bool fully_covers(const std::vector<image_subresource_range>& writes, const image_subresource_range& query)
    {
        const auto query_mip_end   = range_end(query.base_mip_level, query.mip_level_count);
        const auto query_layer_end = range_end(query.base_array_layer, query.array_layer_count);

        for (const uint8_t aspect_bit : {uint8_t{1}, uint8_t{2}, uint8_t{4}})
        {
            if ((static_cast<uint8_t>(query.aspects) & aspect_bit) == 0)
            {
                continue;
            }

            std::vector<uint64_t> mip_boundaries{query.base_mip_level, query_mip_end};
            std::vector<uint64_t> layer_boundaries{query.base_array_layer, query_layer_end};
            for (const auto& write : writes)
            {
                if ((static_cast<uint8_t>(write.aspects) & aspect_bit) == 0 || !overlaps(write, query))
                {
                    continue;
                }
                mip_boundaries.push_back(std::max<uint64_t>(query.base_mip_level, write.base_mip_level));
                mip_boundaries.push_back(std::min<uint64_t>(query_mip_end, range_end(write.base_mip_level, write.mip_level_count)));
                layer_boundaries.push_back(std::max<uint64_t>(query.base_array_layer, write.base_array_layer));
                layer_boundaries.push_back(std::min<uint64_t>(query_layer_end, range_end(write.base_array_layer, write.array_layer_count)));
            }

            std::sort(mip_boundaries.begin(), mip_boundaries.end());
            mip_boundaries.erase(std::unique(mip_boundaries.begin(), mip_boundaries.end()), mip_boundaries.end());
            std::sort(layer_boundaries.begin(), layer_boundaries.end());
            layer_boundaries.erase(std::unique(layer_boundaries.begin(), layer_boundaries.end()), layer_boundaries.end());

            for (size_t mip_index = 0; mip_index + 1 < mip_boundaries.size(); mip_index++)
            {
                for (size_t layer_index = 0; layer_index + 1 < layer_boundaries.size(); layer_index++)
                {
                    const auto mip_begin = mip_boundaries[mip_index];
                    const auto mip_end   = mip_boundaries[mip_index + 1];
                    const auto layer_begin = layer_boundaries[layer_index];
                    const auto layer_end   = layer_boundaries[layer_index + 1];
                    const bool covered = std::ranges::any_of(
                        writes,
                        [&](const image_subresource_range& write)
                        {
                            return (static_cast<uint8_t>(write.aspects) & aspect_bit) != 0 &&
                                   write.base_mip_level <= mip_begin && range_end(write.base_mip_level, write.mip_level_count) >= mip_end &&
                                   write.base_array_layer <= layer_begin && range_end(write.base_array_layer, write.array_layer_count) >= layer_end;
                        });
                    if (!covered)
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // resource version pack/unpack tool

    [[nodiscard]] constexpr resource_version_handle pack(resource_handle index, version_handle version) noexcept
    {
        return (static_cast<resource_version_handle>(version) << 32) | static_cast<resource_version_handle>(index);
    }

    [[nodiscard]] constexpr resource_version_handle pack(image_handle index, version_handle version) noexcept
    {
        return pack(static_cast<resource_handle>(index), version);
    }

    [[nodiscard]] constexpr resource_version_handle pack(buffer_handle index, version_handle version) noexcept
    {
        return pack(static_cast<resource_handle>(index), version);
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

        image_handle add(const std::string& name, const image_desc& desc, bool imported, uint64_t desc_hash = 0)
        {
            const auto handle = image_handle{static_cast<uint32_t>(names.size())};
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

        buffer_handle add(const std::string& name, const buffer_desc& desc, bool imported, uint64_t desc_hash = 0)
        {
            const auto handle = buffer_handle{static_cast<uint32_t>(names.size())};
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
        std::vector<image_handle> image_outputs;
        std::vector<buffer_handle> buffer_outputs;
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
