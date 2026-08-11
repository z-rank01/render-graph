#pragma once

#include <cstdint>
#include <vector>

#include "barrier.h"
#include "resource.h"

namespace render_graph
{
    struct timeline_wait
    {
        submission_batch_handle source_batch = invalid_submission_batch;
        queue_class source_queue = queue_class::graphics;
        uint64_t value = 0;

        [[nodiscard]] constexpr auto operator<=>(const timeline_wait&) const noexcept = default;
    };

    struct cross_queue_dependency
    {
        submission_batch_handle source_batch = invalid_submission_batch;
        submission_batch_handle destination_batch = invalid_submission_batch;
        pass_handle source_pass = invalid_pass;
        pass_handle destination_pass = invalid_pass;
        queue_class source_queue = queue_class::graphics;
        queue_class destination_queue = queue_class::graphics;
        resource_kind kind = resource_kind::image;
        resource_handle logical = invalid_resource;
        bool ownership_transfer = false;
    };

    struct queue_submission_batch
    {
        submission_batch_handle handle = invalid_submission_batch;
        queue_class queue = queue_class::graphics;
        std::vector<pass_handle> passes;
        std::vector<timeline_wait> waits;
        uint64_t signal_value = 0;
        bool waits_for_external_acquire = false;
        bool signals_external_present = false;
        std::vector<synchronization_op> acquire_barriers;
        std::vector<synchronization_op> epilogue_barriers;
        std::vector<synchronization_op> release_barriers;
    };

    struct submission_plan
    {
        std::vector<queue_submission_batch> batches;
        std::vector<submission_batch_handle> pass_to_batch;
        std::vector<cross_queue_dependency> cross_queue_dependencies;

        void clear()
        {
            batches.clear();
            pass_to_batch.clear();
            cross_queue_dependencies.clear();
        }
    };

    struct queue_availability
    {
        bool compute = true;
        bool copy = true;
    };
}
