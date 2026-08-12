#pragma once

// Declares the compile pipeline: the shared compiler_state threaded through
// every stage, plus the stage functions that turn a recipe into a plan.

#include <vector>

#include "graph.h"

namespace render_graph::core
{
    // =============================================================================
    // Resource state contracts
    // =============================================================================

    // Initial/final state a resource must have entering and leaving the graph.
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

    // =============================================================================
    // Compiler state
    // =============================================================================

    // Accumulates everything the pipeline stages produce; one instance is
    // threaded through all stages of a single compile.
    struct compiler_state
    {
        const graph_compile_request* request = nullptr;
        graph_compile_output output;
        std::vector<access_event> accesses;
        dependency_graph dag;
        std::vector<image_state_contract> image_contracts;
        std::vector<buffer_state_contract> buffer_contracts;
        std::vector<uint8_t> active_passes;  // size = pass count; 1 = active after culling
    };

    // =============================================================================
    // Compile pipeline stages
    // =============================================================================

    // Stages run in the order declared below; each returns false to abort the
    // compile, and publish_compiled_plan runs only when all stages succeed.
    bool validate_recipe(compiler_state& state);
    bool build_resource_versions(compiler_state& state);
    bool build_dependency_dag(compiler_state& state);
    bool cull_passes(compiler_state& state);
    bool schedule_passes(compiler_state& state);
    bool compile_lifetimes(compiler_state& state);
    bool compile_synchronization(compiler_state& state);
    bool compile_submissions(compiler_state& state);
    void publish_compiled_plan(compiler_state& state);
} // namespace render_graph::core
