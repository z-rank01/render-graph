#pragma once

#include <cstdint>

namespace render_graph
{
    struct render_graph_limits
    {
        uint32_t max_passes = 4096;
        uint32_t max_images = 65536;
        uint32_t max_buffers = 65536;
        uint32_t max_access_events = 1u << 20;
    };

    struct render_graph_statistics
    {
        uint32_t pass_count = 0;
        uint32_t active_pass_count = 0;
        uint32_t image_count = 0;
        uint32_t buffer_count = 0;
        uint32_t access_event_count = 0;
        uint32_t synchronization_op_count = 0;
        uint32_t submission_batch_count = 0;
        uint32_t image_memory_block_count = 0;
        uint32_t buffer_memory_block_count = 0;
    };
}
