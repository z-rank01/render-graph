#pragma once

#include <vector>

#include "render_graph/graph.h"
#include "render_graph/resource.h"

namespace render_graph
{
    template <typename BackendT>
    class render_graph_system;
}

namespace render_graph::unit_test
{
    // Unit-test-only access shim.
    // Intended for tests that must inject invalid internal state to validate debug assertions.
    struct system_test_access
    {
        template <typename BackendT>
        static uint32_t pass_count(const render_graph_system<BackendT>& system)
        {
            return static_cast<uint32_t>(system.graph.passes.size());
        }

        template <typename BackendT>
        static uint32_t image_count(const render_graph_system<BackendT>& system)
        {
            return static_cast<uint32_t>(system.meta_table.image_metas.names.size());
        }

        template <typename BackendT>
        static uint32_t buffer_count(const render_graph_system<BackendT>& system)
        {
            return static_cast<uint32_t>(system.meta_table.buffer_metas.names.size());
        }

        template <typename BackendT>
        static void inject_cycle_2node(render_graph_system<BackendT>& system)
        {
            system.active_pass_flags.assign(2, true);

            system.dag.adjacency_begins = {0, 1, 2};
            system.dag.adjacency_list   = {1, 0};
            system.dag.in_degrees       = {1, 1};
            system.dag.out_degrees      = {1, 1};
        }

        template <typename BackendT>
        static const directed_acyclic_graph& dag(const render_graph_system<BackendT>& system)
        {
            return system.dag;
        }

        template <typename BackendT>
        static const std::vector<bool>& active_pass_flags(const render_graph_system<BackendT>& system)
        {
            return system.active_pass_flags;
        }

        template <typename BackendT>
        static const std::vector<pass_handle>& sorted_passes(const render_graph_system<BackendT>& system)
        {
            return system.sorted_passes;
        }

        template <typename BackendT>
        static const resource_lifetime& resource_lifetimes(const render_graph_system<BackendT>& system)
        {
            return system.resource_lifetimes;
        }

        template <typename BackendT>
        static bool image_is_imported(const render_graph_system<BackendT>& system, image_handle image)
        {
            return system.meta_table.image_metas.is_imported[image];
        }

        template <typename BackendT>
        static resource_lifetime_class image_lifetime_class(const render_graph_system<BackendT>& system, image_handle image)
        {
            return system.meta_table.image_metas.lifetime_classes[image];
        }

        template <typename BackendT>
        static const read_dependency& image_read_dependencies(const render_graph_system<BackendT>& system)
        {
            return system.image_read_deps;
        }

        template <typename BackendT>
        static const write_dependency& image_write_dependencies(const render_graph_system<BackendT>& system)
        {
            return system.image_write_deps;
        }

        template <typename BackendT>
        static const read_dependency& buffer_read_dependencies(const render_graph_system<BackendT>& system)
        {
            return system.buffer_read_deps;
        }

        template <typename BackendT>
        static const write_dependency& buffer_write_dependencies(const render_graph_system<BackendT>& system)
        {
            return system.buffer_write_deps;
        }

        template <typename BackendT>
        static const std::vector<resource_version_handle>& image_read_versions(const render_graph_system<BackendT>& system)
        {
            return system.img_ver_read_handles;
        }

        template <typename BackendT>
        static const std::vector<resource_version_handle>& image_write_versions(const render_graph_system<BackendT>& system)
        {
            return system.img_ver_write_handles;
        }

        template <typename BackendT>
        static const std::vector<resource_version_handle>& buffer_read_versions(const render_graph_system<BackendT>& system)
        {
            return system.buf_ver_read_handles;
        }

        template <typename BackendT>
        static const std::vector<resource_version_handle>& buffer_write_versions(const render_graph_system<BackendT>& system)
        {
            return system.buf_ver_write_handles;
        }

        template <typename BackendT>
        static const version_producer_map& producer_map(const render_graph_system<BackendT>& system)
        {
            return system.producer_lookup_table;
        }

        template <typename BackendT>
        static const ordered_pass_accesses& ordered_access_stream(const render_graph_system<BackendT>& system)
        {
            return system.ordered_accesses;
        }
    };
}
