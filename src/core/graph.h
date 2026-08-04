#pragma once

#include <cassert>
#include <vector>

#include "resource.h"

namespace render_graph
{
    struct pass_access_event
    {
        resource_ref resource;
        access_type access = access_type::read;
        std::variant<image_access_desc, buffer_access_desc> state;
    };

    struct ordered_pass_accesses
    {
        std::vector<pass_access_event> events;
        std::vector<uint32_t> begins;
        std::vector<uint32_t> lengths;

        void clear()
        {
            events.clear();
            begins.clear();
            lengths.clear();
        }
    };

    // resource dependency

    // one dimesion array to represent the read resource of each pass
    struct read_dependency
    {
        std::vector<resource_handle> read_list;
        std::vector<uint32_t> usage_bits;
        std::vector<resource_handle> begins;
        std::vector<resource_handle> lengthes;
    };

    // one dimesion array to represent the write resource of each pass
    struct write_dependency
    {
        std::vector<resource_handle> write_list;
        std::vector<uint32_t> usage_bits;
        std::vector<resource_handle> begins;
        std::vector<resource_handle> lengthes;
    };

    struct directed_acyclic_graph
    {
        std::vector<pass_handle> adjacency_list;
        std::vector<uint32_t> adjacency_begins;
        std::vector<uint32_t> in_degrees;
        std::vector<uint32_t> out_degrees;
    };

}; // namespace render_graph
