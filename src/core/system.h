#pragma once

#include <vector>

#include "graph.h"

namespace render_graph::core
{
    template <typename AccessDesc>
    struct resource_state_contract
    {
        bool has_initial_state = false;
        AccessDesc initial_state{};
        access_type initial_access = access_type::read;
        contents_policy initial_contents = contents_policy::discard;
        bool has_final_state = false;
        AccessDesc final_state{};
        access_type final_access = access_type::read;
    };

    using image_state_contract = resource_state_contract<image_access_desc>;
    using buffer_state_contract = resource_state_contract<buffer_access_desc>;

    struct compiler_state
    {
        const graph_compile_request* request = nullptr;
        graph_compile_output output;
        std::vector<access_event> accesses;
        dependency_graph dag;
        std::vector<image_state_contract> image_contracts;
        std::vector<buffer_state_contract> buffer_contracts;
    };

    bool validate_recipe(compiler_state& state);
    bool build_resource_versions(compiler_state& state);
    bool build_dependency_dag(compiler_state& state);
    bool schedule_passes(compiler_state& state);
    bool compile_lifetimes(compiler_state& state);
    bool compile_synchronization(compiler_state& state);
    bool compile_submissions(compiler_state& state);
    void publish_compiled_plan(compiler_state& state);
} // namespace render_graph::core
