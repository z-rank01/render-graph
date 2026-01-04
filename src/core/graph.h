#pragma once

#include <cassert>
#include <vector>

#include "backend.h"
#include "rg_function.h"
#include "resource.h"

namespace render_graph
{
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

    // graph/pass context

    // context passed to the setup lambda
    struct pass_setup_context
    {
        resource_meta_table* meta_table;
        read_dependency* image_read_deps;
        write_dependency* image_write_deps;
        read_dependency* buffer_read_deps;
        write_dependency* buffer_write_deps;
        output_table* output_table;
        pass_handle current_pass;

        // create

        resource_handle create_image(const image_info& info) const { return meta_table->image_metas.add(info); }
        resource_handle create_buffer(const buffer_info& info) const { return meta_table->buffer_metas.add(info); }

        // output

        void declare_image_output(resource_handle resource) const
        {
            // validate resource is an image
            assert(resource < meta_table->image_metas.names.size());
            output_table->image_outputs.push_back(resource);
        }

        void declare_buffer_output(resource_handle resource) const
        {
            // validate resource is a buffer
            assert(resource < meta_table->buffer_metas.names.size());
            output_table->buffer_outputs.push_back(resource);
        }

        // read

        void read_image(resource_handle resource, image_usage usage) const
        {
            image_read_deps->read_list.push_back(resource);
            image_read_deps->usage_bits.push_back(static_cast<uint32_t>(usage));
            image_read_deps->lengthes[current_pass]++;
        }
        void read_buffer(resource_handle resource, buffer_usage usage) const
        {
            buffer_read_deps->read_list.push_back(resource);
            buffer_read_deps->usage_bits.push_back(static_cast<uint32_t>(usage));
            buffer_read_deps->lengthes[current_pass]++;
        }

        // write

        void write_image(resource_handle resource, image_usage usage) const
        {
            image_write_deps->write_list.push_back(resource);
            image_write_deps->usage_bits.push_back(static_cast<uint32_t>(usage));
            image_write_deps->lengthes[current_pass]++;
        }
        void write_buffer(resource_handle resource, buffer_usage usage) const
        {
            buffer_write_deps->write_list.push_back(resource);
            buffer_write_deps->usage_bits.push_back(static_cast<uint32_t>(usage));
            buffer_write_deps->lengthes[current_pass]++;
        }
    };

    // context passed to the execution lambda
    struct pass_execute_context
    {
        backend* backend;
        // void* command_buffer; // Abstract command buffer
    };

    // graph topology

    using pass_execute_func = rg_function<void(pass_execute_context&)>;
    using pass_setup_func   = rg_function<void(pass_setup_context&)>;

    struct graph_topology
    {
        std::vector<pass_handle> passes;
        std::vector<pass_setup_func> setup_funcs;
        std::vector<pass_execute_func> execute_funcs;
    };

    struct directed_acyclic_graph
    {
        std::vector<pass_handle> adjacency_list;
        std::vector<uint32_t> adjacency_begins;
        std::vector<uint32_t> in_degrees;
        std::vector<uint32_t> out_degrees;
    };

}; // namespace render_graph
