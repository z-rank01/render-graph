#pragma once

// Synchronization model of the render graph: intent flags and the planning-level
// synchronization op tables that backends lower into API-specific barriers.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "resource.h"

namespace render_graph
{
// =============================================================================
// Synchronization intent flags
// =============================================================================
    // Bitmask of the guarantees a synchronization op must provide; combine with |.
    enum class synchronization_intent : uint8_t
    {
        none                 = 0,
        layout_transition    = 1 << 0,
        execution_dependency = 1 << 1,
        memory_dependency    = 1 << 2,
        queue_ownership      = 1 << 3,
        aliasing             = 1 << 4,
    };

    [[nodiscard]] constexpr synchronization_intent operator|(synchronization_intent left,
                                                               synchronization_intent right) noexcept
    {
        return static_cast<synchronization_intent>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
    }

    constexpr synchronization_intent& operator|=(synchronization_intent& left,
                                                   synchronization_intent right) noexcept
    {
        left = left | right;
        return left;
    }

    [[nodiscard]] constexpr bool has_intent(synchronization_intent intents,
                                            synchronization_intent requested) noexcept
    {
        return (static_cast<uint8_t>(intents) & static_cast<uint8_t>(requested)) != 0;
    }

// =============================================================================
// Phase classification
// =============================================================================
    // full = non-split barrier; release/acquire are the two halves of a split barrier.
    // An op's scope is not stored on the row — it is implied by which CSR
    // segment of the plan the row lives in (prologue / epilogue).
    enum class synchronization_phase : uint8_t
    {
        full = 0,
        release,
        acquire,
    };

// =============================================================================
// Planning-level synchronization types
// =============================================================================
    struct abstract_resource_state
    {
        uint32_t usage_bits = 0;
        access_type access = access_type::read;
        pipeline_domain domain = pipeline_domain::any;
        queue_class queue = queue_class::graphics;
        image_subresource_range image_range{};
        buffer_byte_range buffer_range{};

        [[nodiscard]] constexpr auto operator<=>(const abstract_resource_state&) const noexcept = default;
    };

    // CSR ranges into one op table: prologue is indexed per pass, epilogue is
    // a single graph-level range. There is no pass-internal segment — the
    // compiler never emits internal ops.
    struct synchronization_segments
    {
        std::vector<uint32_t> prologue_begins;   // size = pass_count + 1
        std::vector<uint32_t> prologue_lengths;  // size = pass_count
        uint32_t epilogue_begin = 0;
        uint32_t epilogue_length = 0;

        void clear()
        {
            prologue_begins.clear();
            prologue_lengths.clear();
            epilogue_begin = 0;
            epilogue_length = 0;
        }
    };

    // SoA op table for one resource kind (image and buffer tables are
    // separate — there is no kind column, the kind is fixed by the table).
    // Ops of pass p occupy rows
    // [prologue_begins[p], prologue_begins[p] + prologue_lengths[p]), followed
    // by the graph epilogue segment.
    template <typename RangeDesc>
    struct synchronization_op_rows
    {
        synchronization_segments segments;

        // --- Per-op columns ---
        std::vector<synchronization_phase> phases;
        std::vector<synchronization_intent> intents;
        std::vector<resource_handle> logicals;
        std::vector<resource_handle> physicals;
        std::vector<resource_handle> memory_blocks;
        std::vector<resource_handle> previous_logicals; // aliasing only; sentinel otherwise
        std::vector<pass_handle> passes;
        std::vector<pass_handle> source_passes;

        // --- Before state columns ---
        std::vector<uint32_t> before_usage_bits;
        std::vector<access_type> before_accesses;
        std::vector<pipeline_domain> before_domains;
        std::vector<queue_class> before_queues;
        std::vector<RangeDesc> before_ranges;

        // --- After state columns ---
        std::vector<uint32_t> after_usage_bits;
        std::vector<access_type> after_accesses;
        std::vector<pipeline_domain> after_domains;
        std::vector<queue_class> after_queues;
        std::vector<RangeDesc> after_ranges;

        [[nodiscard]] std::size_t size() const noexcept { return phases.size(); }

        void clear()
        {
            segments.clear();
            phases.clear();
            intents.clear();
            logicals.clear();
            physicals.clear();
            memory_blocks.clear();
            previous_logicals.clear();
            passes.clear();
            source_passes.clear();
            before_usage_bits.clear();
            before_accesses.clear();
            before_domains.clear();
            before_queues.clear();
            before_ranges.clear();
            after_usage_bits.clear();
            after_accesses.clear();
            after_domains.clear();
            after_queues.clear();
            after_ranges.clear();
        }
    };

    using image_sync_op_rows  = synchronization_op_rows<image_subresource_range>;
    using buffer_sync_op_rows = synchronization_op_rows<buffer_byte_range>;

    struct synchronization_plan
    {
        image_sync_op_rows image;
        buffer_sync_op_rows buffer;

        void clear()
        {
            image.clear();
            buffer.clear();
        }
    };

    // Sentinel index meaning "this op table has no such row".
    inline constexpr uint32_t invalid_op_index = std::numeric_limits<uint32_t>::max();

    // Reference into the kind-split op tables used for split barriers: exactly
    // one index is valid (the other is invalid_op_index), and `phase` records
    // which half of the barrier the owning batch must emit.
    struct synchronization_reference
    {
        uint32_t image_op = invalid_op_index;
        uint32_t buffer_op = invalid_op_index;
        synchronization_phase phase = synchronization_phase::full;
    };

} // namespace render_graph
