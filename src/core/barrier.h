#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "resource.h"
#include "resource_types.h"

namespace render_graph
{
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

    // Coarse pipeline domain; concrete backends can ignore or refine this.
    enum class pipeline_domain : uint8_t
    {
        any = 0,
        graphics,
        compute,
        copy,
    };

    enum class barrier_op_type : uint8_t
    {
        transition = 0,
        uav,
        aliasing,
    };

    // API-agnostic barrier op.
    // Backends should lower these into Vulkan barriers / DX12 barriers+fences / Metal fences/events.
    struct barrier_op
    {
        barrier_op_type type = barrier_op_type::transition;
        resource_kind kind   = resource_kind::image;

        // The logical resource handle (as declared by user).
        resource_handle logical = 0;

        // The physical resource id (after aliasing); index into backend/user-side physical tables.
        // NOTE: This is NOT an API object handle; it's an RG-defined id.
        resource_handle physical = 0;

        pipeline_domain src_domain = pipeline_domain::any;
        pipeline_domain dst_domain = pipeline_domain::any;

        access_type src_access = access_type::read;
        access_type dst_access = access_type::read;

        // For images: stores image_usage bits.
        // For buffers: stores buffer_usage bits.
        uint32_t src_usage_bits = 0;
        uint32_t dst_usage_bits = 0;

        // For aliasing barrier: previous logical resource sharing the same physical id.
        resource_handle prev_logical = 0;
    };

    struct per_pass_barrier
    {
        // Per-pass ranges into the SoA arrays below (CSR style).
        // For pass p: ops are in [pass_begins[p], pass_begins[p] + pass_lengths[p]).
        // pass_begins.size() = pass_count + 1
        // pass_lengths.size() = pass_count
        std::vector<uint32_t> pass_begins;
        std::vector<uint32_t> pass_lengths;

        std::vector<barrier_op_type> types;
        std::vector<resource_kind> kinds;
        
        std::vector<resource_handle> logicals;
        std::vector<resource_handle> physicals;
        
        std::vector<pipeline_domain> src_domains;
        std::vector<pipeline_domain> dst_domains;
        
        std::vector<access_type> src_accesses;
        std::vector<access_type> dst_accesses;
        
        std::vector<uint32_t> src_usage_bits;
        std::vector<uint32_t> dst_usage_bits;
        
        std::vector<resource_handle> prev_logicals;

        void clear()
        {
            pass_begins.clear();
            pass_lengths.clear();
            types.clear();
            kinds.clear();
            logicals.clear();
            physicals.clear();
            src_domains.clear();
            dst_domains.clear();
            src_accesses.clear();
            dst_accesses.clear();
            src_usage_bits.clear();
            dst_usage_bits.clear();
            prev_logicals.clear();
        }

        void resize_passes(size_t pass_count)
        {
            pass_begins.assign(pass_count + 1, 0);
            pass_lengths.assign(pass_count, 0);
        }

        void resize_ops(size_t op_count)
        {
            types.resize(op_count);
            kinds.resize(op_count);
            logicals.resize(op_count);
            physicals.resize(op_count);
            src_domains.resize(op_count);
            dst_domains.resize(op_count);
            src_accesses.resize(op_count);
            dst_accesses.resize(op_count);
            src_usage_bits.resize(op_count);
            dst_usage_bits.resize(op_count);
            prev_logicals.resize(op_count);
        }
    };

} // namespace render_graph
