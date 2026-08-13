#pragma once

// Core resource vocabulary of the render graph: typed handles, access
// descriptors, subresource range math, and the lifetime/aliasing metadata
// consumed by the compile pass.

#include <algorithm>
#include <cstdint>
#include <compare>
#include <limits>
#include <vector>

#include "resource_types.h"

namespace render_graph
{
// =============================================================================
// Typed handles
// =============================================================================
    // Newtype over a dense 32-bit row index. The tag exists at compile time
    // only: SoA columns stay plain uint32_t arrays with identical layout and
    // cache behavior. Construction from a raw index is explicit and there is
    // no implicit conversion back, so image/buffer/pass index spaces cannot
    // be mixed silently at call sites. The default value is the invalid
    // sentinel ("data absence", not a flag).
    template <typename Tag>
    struct typed_handle
    {
        uint32_t value = std::numeric_limits<uint32_t>::max();

        constexpr typed_handle() noexcept = default;
        explicit constexpr typed_handle(uint32_t index) noexcept : value(index) { }

        [[nodiscard]] constexpr uint32_t index() const noexcept { return value; }
        // Explicit conversion only: keeps existing `static_cast<uint32_t>(h)`
        // index-conversion code compiling, while implicit decay stays
        // ill-formed. Cross-space and integer comparisons are also ill-formed.
        explicit constexpr operator uint32_t() const noexcept { return value; }

        [[nodiscard]] constexpr auto operator<=>(const typed_handle&) const noexcept = default;
    };

    // --- Handle aliases and invalid sentinels ---
    struct image_handle_tag;
    struct buffer_handle_tag;
    struct pass_handle_tag;
    struct submission_batch_handle_tag;

    using image_handle  = typed_handle<image_handle_tag>;
    using buffer_handle = typed_handle<buffer_handle_tag>;
    using pass_handle   = typed_handle<pass_handle_tag>;
    using submission_batch_handle = typed_handle<submission_batch_handle_tag>;

    using resource_handle         = uint32_t;

    // Physical-object and memory-block index spaces (compile output side).
    struct physical_image_id_tag;
    struct physical_buffer_id_tag;
    struct memory_block_id_tag;

    using physical_image_id   = typed_handle<physical_image_id_tag>;
    using physical_buffer_id  = typed_handle<physical_buffer_id_tag>;
    using memory_block_id     = typed_handle<memory_block_id_tag>;

    inline constexpr resource_handle invalid_resource                 = std::numeric_limits<resource_handle>::max();
    inline constexpr image_handle invalid_image{std::numeric_limits<uint32_t>::max()};
    inline constexpr buffer_handle invalid_buffer{std::numeric_limits<uint32_t>::max()};
    inline constexpr pass_handle invalid_pass{std::numeric_limits<uint32_t>::max()};
    inline constexpr submission_batch_handle invalid_submission_batch{std::numeric_limits<uint32_t>::max()};
    inline constexpr physical_image_id invalid_physical_image_id{std::numeric_limits<uint32_t>::max()};
    inline constexpr physical_buffer_id invalid_physical_buffer_id{std::numeric_limits<uint32_t>::max()};
    inline constexpr memory_block_id invalid_memory_block_id{std::numeric_limits<uint32_t>::max()};

// =============================================================================
// Enumerations
// =============================================================================
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

    enum class queue_class : uint8_t
    {
        graphics = 0,
        compute,
        copy,
    };

    enum class contents_policy : uint8_t
    {
        discard = 0,
        preserve,
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

// =============================================================================
// Subresource ranges and access descriptors
// =============================================================================
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
        queue_class queue                  = queue_class::graphics;
        image_subresource_range subresource{};

        [[nodiscard]] constexpr auto operator<=>(const image_access_desc&) const noexcept = default;
    };

    struct buffer_access_desc
    {
        buffer_usage usage     = buffer_usage::NONE;
        pipeline_domain domain = pipeline_domain::any;
        queue_class queue       = queue_class::graphics;
        buffer_byte_range bytes{};

        [[nodiscard]] constexpr auto operator<=>(const buffer_access_desc&) const noexcept = default;
    };

    // --- Backing-memory requirements ---
    struct allocation_requirements
    {
        uint64_t size             = 0;
        uint64_t alignment        = 1;
        uint32_t memory_type_bits = std::numeric_limits<uint32_t>::max();
        bool requires_dedicated   = false;
        bool supports_aliasing    = false;

        [[nodiscard]] constexpr auto operator<=>(const allocation_requirements&) const noexcept = default;
    };

// =============================================================================
// Range overlap and coverage helpers
// =============================================================================
    [[nodiscard]] constexpr uint64_t range_end(uint64_t begin, uint64_t count) noexcept
    {
        // Saturate at the sentinel instead of overflowing begin + count.
        if (count == whole_buffer_size || count > whole_buffer_size - begin)
        {
            return whole_buffer_size;
        }
        return begin + count;
    }

    [[nodiscard]] constexpr uint64_t range_end(uint32_t begin, uint32_t count) noexcept
    {
        // Saturate at the sentinel instead of overflowing begin + count.
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
        // Sweep writes in offset order; any gap ahead of the cursor means partial coverage.
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

            // Collect mip/layer cut points from every write overlapping this aspect.
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

            // Sort and deduplicate the cut points into a grid of candidate cells.
            std::sort(mip_boundaries.begin(), mip_boundaries.end());
            mip_boundaries.erase(std::unique(mip_boundaries.begin(), mip_boundaries.end()), mip_boundaries.end());
            std::sort(layer_boundaries.begin(), layer_boundaries.end());
            layer_boundaries.erase(std::unique(layer_boundaries.begin(), layer_boundaries.end()), layer_boundaries.end());

            // Every grid cell must be fully covered by at least one write range.
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

// =============================================================================
// Lifetime and aliasing metadata
// =============================================================================
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
        // One aliasing handoff per physical object: `previous` and `next` are
        // the logical handles sharing the object (kind fixed by the table the
        // handoff lives in — no runtime kind column).
        template <typename Handle>
        struct alias_handoff
        {
            Handle previous = Handle{};
            Handle next     = Handle{};
            memory_block_id memory_block = invalid_memory_block_id;
            pass_handle at_pass          = invalid_pass;
        };

        // Native object reuse plan. Multiple logical resources may map to one object
        // only when their descs are compatible and their execution lifetimes do not overlap.
        std::vector<image_handle> physical_image_meta;          // physical id → logical handle
        std::vector<physical_image_id> handle_to_physical_img_id; // logical handle → physical id
        std::vector<buffer_handle> physical_buffer_meta;        // physical id → logical handle
        std::vector<physical_buffer_id> handle_to_physical_buf_id; // logical handle → physical id

        // Memory alias plan is distinct from object reuse: different native objects may
        // occupy the same allocation block when backend requirements permit it.
        std::vector<allocation_requirements> image_memory_blocks;
        std::vector<allocation_requirements> buffer_memory_blocks;
        std::vector<memory_block_id> handle_to_image_memory_block;
        std::vector<memory_block_id> handle_to_buffer_memory_block;
        std::vector<alias_handoff<image_handle>> image_alias_handoffs;
        std::vector<alias_handoff<buffer_handle>> buffer_alias_handoffs;

        void clear()
        {
            physical_image_meta.clear();
            physical_buffer_meta.clear();
            handle_to_physical_img_id.clear();
            handle_to_physical_buf_id.clear();
            image_memory_blocks.clear();
            buffer_memory_blocks.clear();
            handle_to_image_memory_block.clear();
            handle_to_buffer_memory_block.clear();
            image_alias_handoffs.clear();
            buffer_alias_handoffs.clear();
        }
    };

} // namespace render_graph
