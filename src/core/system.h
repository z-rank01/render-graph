#pragma once

// Declares the compile pipeline: the shared compiler_state threaded through
// every stage, plus the stage functions that turn a recipe into a plan.

#include <vector>

#include "graph.h"
#include "render_graph/compiler.h"

namespace render_graph::core
{
    // =============================================================================
    // Resource state contracts
    // =============================================================================

    // Initial/final state a resource must have entering and leaving the graph.
    // Contracts live in compact rows (one row per resource with a contract);
    // existence is expressed by the per-resource index column
    // (invalid_contract_index = no contract) — a row always carries both the
    // initial and the final state, so the old has_initial/has_final mirrors
    // are gone.
    template <typename AccessDesc>
    struct resource_state_contract
    {
        AccessDesc initial_state{};
        access_type initial_access = access_type::read;
        contents_policy initial_contents = contents_policy::discard;
        AccessDesc final_state{};
        access_type final_access = access_type::read;
    };

    inline constexpr uint32_t invalid_contract_index = std::numeric_limits<uint32_t>::max();

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
        image_access_rows image_events;      // SoA, sorted by (pass, logical)
        buffer_access_rows buffer_events;    // SoA, sorted by (pass, logical)
        dependency_graph dag;
        std::vector<uint32_t> image_contract_indices; // per logical; sentinel = no contract
        std::vector<image_state_contract> image_contracts;
        std::vector<uint32_t> buffer_contract_indices; // per logical; sentinel = no contract
        std::vector<buffer_state_contract> buffer_contracts;
        // Culling intermediate state (consumed by compact_passes, then released):
        // active_pass_list is the compact active passes in declaration order,
        // pass_old_to_new maps every old pass to its compact index.
        std::vector<pass_handle> active_pass_list;
        std::vector<uint32_t> pass_old_to_new;
        uint32_t culled_pass_count = 0;
    };

    // =============================================================================
    // Compile pipeline stages
    // =============================================================================

    // Stages run in the order declared below; every stage returns false to
    // abort the compile (publish is last in the table and always succeeds).
    bool validate_recipe(compiler_state& state);
    bool build_resource_versions(compiler_state& state);
    bool build_dependency_dag(compiler_state& state);
    bool cull_passes(compiler_state& state);
    bool compact_passes(compiler_state& state);
    bool schedule_passes(compiler_state& state);
    bool compile_lifetimes(compiler_state& state);
    bool compile_synchronization(compiler_state& state);
    bool compile_submissions(compiler_state& state);
    bool publish_compiled_plan(compiler_state& state);
} // namespace render_graph::core
