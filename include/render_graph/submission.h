#pragma once

// Data structures describing the render graph submission plan: per-queue
// submission batches, timeline waits, and cross-queue dependencies produced
// by frame planning.

#include <cstdint>
#include <vector>

#include "barrier.h"
#include "resource.h"

namespace render_graph
{
    // =============================================================================
    // Submission plan data types
    // =============================================================================

    // A wait on a timeline semaphore value signaled by an earlier batch.
    struct timeline_wait
    {
        submission_batch_handle source_batch = invalid_submission_batch;
        queue_class source_queue = queue_class::graphics;
        uint64_t value = 0;

        [[nodiscard]] constexpr auto operator<=>(const timeline_wait&) const noexcept = default;
    };

    // A dependency where a resource produced on one queue is consumed on
    // another; ownership_transfer marks an acquire/release ownership handoff.
    // The logical handle type is fixed by the table the dependency lives in
    // (image_cross_queue_dependencies / buffer_cross_queue_dependencies).
    template <typename Handle>
    struct cross_queue_dependency
    {
        submission_batch_handle source_batch = invalid_submission_batch;
        submission_batch_handle destination_batch = invalid_submission_batch;
        pass_handle source_pass = invalid_pass;
        pass_handle destination_pass = invalid_pass;
        queue_class source_queue = queue_class::graphics;
        queue_class destination_queue = queue_class::graphics;
        Handle logical = Handle{};
        bool ownership_transfer = false;
    };

    // Batch flag bits, packed into the `batch_flags` column of submission_plan.
    inline constexpr uint8_t submission_flag_external_acquire = 0x01U; // batch 0 waits for the swapchain image
    inline constexpr uint8_t submission_flag_external_present  = 0x02U; // last batch signals present

    // Full per-frame submission plan (SoA): one row per submission batch,
    // with per-batch CSR segments for passes, timeline waits, and the
    // release/acquire barrier references, plus the mapping from each pass to
    // the batch that executes it.
    struct submission_plan
    {
        // --- Batch scalar columns (batch handle == row index) ---
        std::vector<queue_class> batch_queues;
        std::vector<uint64_t> batch_signal_values;
        std::vector<uint8_t> batch_flags; // submission_flag_* bits

        // --- Per-batch pass CSR ---
        std::vector<uint32_t> batch_pass_begins; // size = batch_count + 1
        std::vector<pass_handle> batch_passes;

        // --- Per-batch timeline waits CSR ---
        std::vector<uint32_t> batch_wait_begins; // size = batch_count + 1
        std::vector<timeline_wait> batch_waits;

        // --- Per-batch split-barrier references (ops are not copied) ---
        std::vector<uint32_t> batch_release_begins; // size = batch_count + 1
        std::vector<synchronization_reference> release_refs;
        std::vector<uint32_t> batch_acquire_begins; // size = batch_count + 1
        std::vector<synchronization_reference> acquire_refs;

        // --- Mapping and cross-queue dependencies (kind split, no kind column) ---
        std::vector<submission_batch_handle> pass_to_batch;
        std::vector<cross_queue_dependency<image_handle>> image_cross_queue_dependencies;
        std::vector<cross_queue_dependency<buffer_handle>> buffer_cross_queue_dependencies;

        [[nodiscard]] std::size_t batch_count() const noexcept { return batch_queues.size(); }

        void clear()
        {
            batch_queues.clear();
            batch_signal_values.clear();
            batch_flags.clear();
            batch_pass_begins.clear();
            batch_passes.clear();
            batch_wait_begins.clear();
            batch_waits.clear();
            batch_release_begins.clear();
            release_refs.clear();
            batch_acquire_begins.clear();
            acquire_refs.clear();
            pass_to_batch.clear();
            image_cross_queue_dependencies.clear();
            buffer_cross_queue_dependencies.clear();
        }
    };

    struct queue_availability
    {
        bool compute = true;
        bool copy = true;
    };
}
