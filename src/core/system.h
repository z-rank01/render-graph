#pragma once

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

#include "backend.h"
#include "barrier.h"
#include "graph.h"
#include "resource.h"

namespace render_graph
{
    class render_graph_system
    {
    public:
        // resource related
        resource_meta_table meta_table;
        read_dependency image_read_deps;
        write_dependency image_write_deps;
        read_dependency buffer_read_deps;
        write_dependency buffer_write_deps;

        // Versioned dependency views generated during compile().
        // These are compile-time/internal and are derived from *_deps + versioning rules.
        std::vector<resource_version_handle> img_ver_read_handles;
        std::vector<resource_version_handle> img_ver_write_handles;
        std::vector<resource_version_handle> buf_ver_read_handles;
        std::vector<resource_version_handle> buf_ver_write_handles;

        version_producer_map producer_lookup_table;
        output_table output_table;

        resource_lifetime resource_lifetimes;
        physical_resource_meta physical_resource_metas;

        // pass related
        graph_topology graph;
        directed_acyclic_graph dag;
        std::vector<bool> active_pass_flags;
        std::vector<pass_handle> sorted_passes;

        // backend related
        backend* backend = nullptr;

        // Barrier plan generated during compile().
        // Indexed by pass_handle; only active passes are consumed by execute().
        per_pass_barrier per_pass_barriers;

        void set_backend(class backend* backend_ptr) { backend = backend_ptr; }

        // 1. Add Pass System
        // Separates resource definition (setup) from execution logic.

        template <typename SetupFn = pass_setup_func, typename ExecuteFn = pass_execute_func>
        pass_handle add_pass(SetupFn&& setup, ExecuteFn&& execute)
        {
            auto handle = static_cast<pass_handle>(graph.passes.size());
            graph.passes.push_back(handle);
            graph.setup_funcs.push_back(std::forward<SetupFn>(setup));
            graph.execute_funcs.push_back(std::forward<ExecuteFn>(execute));
            return handle;
        }

        // 2. Compile System

        void compile()
        {
            const auto pass_count   = graph.passes.size();
            const auto invalid_pass = std::numeric_limits<pass_handle>::max();
            const auto invalid_resource = std::numeric_limits<resource_handle>::max();

            // Reset dependency storage
            image_read_deps.read_list.clear();
            image_read_deps.usage_bits.clear();
            image_read_deps.begins.assign(pass_count, 0);
            image_read_deps.lengthes.assign(pass_count, 0);
            image_write_deps.write_list.clear();
            image_write_deps.usage_bits.clear();
            image_write_deps.begins.assign(pass_count, 0);
            image_write_deps.lengthes.assign(pass_count, 0);
            buffer_read_deps.read_list.clear();
            buffer_read_deps.usage_bits.clear();
            buffer_read_deps.begins.assign(pass_count, 0);
            buffer_read_deps.lengthes.assign(pass_count, 0);
            buffer_write_deps.write_list.clear();
            buffer_write_deps.usage_bits.clear();
            buffer_write_deps.begins.assign(pass_count, 0);
            buffer_write_deps.lengthes.assign(pass_count, 0);
            output_table.image_outputs.clear();
            output_table.buffer_outputs.clear();

            img_ver_read_handles.clear();
            img_ver_write_handles.clear();
            buf_ver_read_handles.clear();
            buf_ver_write_handles.clear();

            // Step A: Invoke Setup Functions
            // Invoke setup function to collect resource usages so that we
            // can compute the topology of pass and execute succeeding phases.
            // - Read: graph.passes, graph.setup_funcs
            // - Write: meta_table, image_read_deps, image_write_deps, buffer_read_deps, buffer_write_deps

            pass_setup_context setup_ctx{.meta_table        = &meta_table,
                                         .image_read_deps   = &image_read_deps,
                                         .image_write_deps  = &image_write_deps,
                                         .buffer_read_deps  = &buffer_read_deps,
                                         .buffer_write_deps = &buffer_write_deps,
                                         .output_table      = &output_table,
                                         .current_pass      = 0};
            for (size_t i = 0; i < pass_count; i++)
            {
                setup_ctx.current_pass = graph.passes[i];

                // Mark begin offsets for this pass (SoA range encoding)
                image_read_deps.begins[setup_ctx.current_pass]   = static_cast<pass_handle>(image_read_deps.read_list.size());
                image_write_deps.begins[setup_ctx.current_pass]  = static_cast<pass_handle>(image_write_deps.write_list.size());
                buffer_read_deps.begins[setup_ctx.current_pass]  = static_cast<pass_handle>(buffer_read_deps.read_list.size());
                buffer_write_deps.begins[setup_ctx.current_pass] = static_cast<pass_handle>(buffer_write_deps.write_list.size());

                auto setup_func = graph.setup_funcs[i];
                setup_func(setup_ctx);
            }

            const auto image_count  = meta_table.image_metas.names.size();
            const auto buffer_count = meta_table.buffer_metas.names.size();

            // Step B: Compute Resource Version (pack handle + version)
            // User-facing setup stage uses resource_handle only.
            // Here we derive a versioned view for internal compile-time algorithms.
            // IMPORTANT:
            // - Never use resource_version_handle (packed u64) as a vector index.
            // - Only index SoA arrays by resource_handle (low 32 bits).
            // - Read: graph.passes, *_deps
            // - Write: *_versions

            img_ver_read_handles.resize(image_read_deps.read_list.size());
            img_ver_write_handles.resize(image_write_deps.write_list.size());
            buf_ver_read_handles.resize(buffer_read_deps.read_list.size());
            buf_ver_write_handles.resize(buffer_write_deps.write_list.size());

            std::vector<version_handle> image_next_versions(image_count, 0);
            std::vector<version_handle> buffer_next_versions(buffer_count, 0);

            for (size_t i = 0; i < pass_count; i++)
            {
                const auto current_pass = graph.passes[i];

                // image reads
                {
                    const auto read_begin  = image_read_deps.begins[current_pass];
                    const auto read_length = image_read_deps.lengthes[current_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        const auto image        = image_read_deps.read_list[j];
                        const auto next_version = (image < image_next_versions.size()) ? image_next_versions[image] : 0;
                        if (next_version == 0)
                        {
                            // Unwritten (or imported-only) at this point; treat as having no producer.
                            // Validation should catch illegal read-before-write for non-imported resources.
                            img_ver_read_handles[j] = invalid_resource_version;
                        }
                        else
                        {
                            const auto version      = static_cast<version_handle>(next_version - 1);
                            img_ver_read_handles[j] = pack(image, version);
                        }
                    }
                }

                // image writes
                {
                    const auto write_begin  = image_write_deps.begins[current_pass];
                    const auto write_length = image_write_deps.lengthes[current_pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        const auto image = image_write_deps.write_list[j];
                        if (image >= image_count)
                        {
                            img_ver_write_handles[j] = invalid_resource_version;
                            continue;
                        }
                        const auto next_version    = image_next_versions[image];
                        img_ver_write_handles[j]   = pack(image, next_version);
                        image_next_versions[image] = static_cast<version_handle>(next_version + 1);
                    }
                }

                // buffer reads
                {
                    const auto read_begin  = buffer_read_deps.begins[current_pass];
                    const auto read_length = buffer_read_deps.lengthes[current_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        const auto buffer       = buffer_read_deps.read_list[j];
                        const auto next_version = (buffer < buffer_next_versions.size()) ? buffer_next_versions[buffer] : 0;
                        if (next_version == 0)
                        {
                            buf_ver_read_handles[j] = invalid_resource_version;
                        }
                        else
                        {
                            const auto version      = static_cast<version_handle>(next_version - 1);
                            buf_ver_read_handles[j] = pack(buffer, version);
                        }
                    }
                }

                // buffer writes
                {
                    const auto write_begin  = buffer_write_deps.begins[current_pass];
                    const auto write_length = buffer_write_deps.lengthes[current_pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        const auto buffer = buffer_write_deps.write_list[j];
                        if (buffer >= buffer_count)
                        {
                            buf_ver_write_handles[j] = invalid_resource_version;
                            continue;
                        }
                        const auto next_version      = buffer_next_versions[buffer];
                        buf_ver_write_handles[j]     = pack(buffer, next_version);
                        buffer_next_versions[buffer] = static_cast<version_handle>(next_version + 1);
                    }
                }
            }

            // Step C: Build resource-producer map (+ latest version per handle)
            // Build version -> producer lookup in a flat array (DOD/SoA friendly):
            // - offsets are indexed by resource_handle
            // - producers are indexed by (offset + version)
            // - Read: graph.passes, image_write_deps, buffer_write_deps, *_write_versions
            // - Write: producer_lookup_table

            // Build image offsets + latest
            producer_lookup_table.img_version_offsets.assign(static_cast<size_t>(image_count) + 1, 0);
            producer_lookup_table.latest_img.assign(image_count, invalid_resource_version);
            {
                uint32_t running = 0;
                for (resource_handle image = 0; image < image_count; image++)
                {
                    producer_lookup_table.img_version_offsets[image] = running;
                    const auto version                               = image_next_versions[image];
                    if (version > 0)
                    {
                        producer_lookup_table.latest_img[image] = pack(image, static_cast<version_handle>(version - 1));
                    }
                    running = (running + static_cast<uint32_t>(version));
                }
                producer_lookup_table.img_version_offsets[image_count] = running;
                producer_lookup_table.img_version_producers.assign(running, invalid_pass);
            }

            // Build buffer offsets + latest
            producer_lookup_table.buf_version_offsets.assign(static_cast<size_t>(buffer_count) + 1, 0);
            producer_lookup_table.latest_buf.assign(buffer_count, invalid_resource_version);
            {
                uint32_t running = 0;
                for (resource_handle buffer = 0; buffer < buffer_count; buffer++)
                {
                    producer_lookup_table.buf_version_offsets[buffer] = running;
                    const auto version                                = buffer_next_versions[buffer];
                    if (version > 0)
                    {
                        producer_lookup_table.latest_buf[buffer] = pack(buffer, static_cast<version_handle>(version - 1));
                    }
                    running = (running + static_cast<uint32_t>(version));
                }
                producer_lookup_table.buf_version_offsets[buffer_count] = running;
                producer_lookup_table.buf_version_producers.assign(running, invalid_pass);
            }

            // Fill image producers for each (image, version)
            for (size_t i = 0; i < pass_count; i++)
            {
                const auto current_pass = graph.passes[i];
                const auto begin        = image_write_deps.begins[current_pass];
                const auto length       = image_write_deps.lengthes[current_pass];
                for (auto j = begin; j < begin + length; j++)
                {
                    const auto image_version_handle = img_ver_write_handles[j];
                    const auto image                = unpack_to_resource(image_version_handle);
                    const auto version              = unpack_to_version(image_version_handle);
                    if (image >= image_count)
                    {
                        continue;
                    }
                    const auto base = producer_lookup_table.img_version_offsets[image];
                    const auto end  = producer_lookup_table.img_version_offsets[image + 1];
                    const auto idx  = static_cast<uint32_t>(base + version);
                    if (idx < end)
                    {
                        producer_lookup_table.img_version_producers[idx] = current_pass;
                    }
                }
            }

            // Fill buffer producers for each (buffer, version)
            for (size_t i = 0; i < pass_count; i++)
            {
                const auto current_pass = graph.passes[i];
                const auto begin        = buffer_write_deps.begins[current_pass];
                const auto length       = buffer_write_deps.lengthes[current_pass];
                for (auto j = begin; j < begin + length; j++)
                {
                    const auto buffer_version_handle = buf_ver_write_handles[j];
                    const auto buffer                = unpack_to_resource(buffer_version_handle);
                    const auto version               = unpack_to_version(buffer_version_handle);
                    if (buffer >= buffer_count)
                    {
                        continue;
                    }
                    const auto base = producer_lookup_table.buf_version_offsets[buffer];
                    const auto end  = producer_lookup_table.buf_version_offsets[buffer + 1];
                    const auto idx  = static_cast<uint32_t>(base + version);
                    if (idx < end)
                    {
                        producer_lookup_table.buf_version_producers[idx] = current_pass;
                    }
                }
            }

            // Step D: Culling
            // Analyze dependencies and mark passes as active/inactive

            active_pass_flags.assign(pass_count, false);
            std::queue<pass_handle> culling_worklist;
            // invalid_pass is defined above for producer map.

            auto enqueue_pass = [&](pass_handle pass)
            {
                if (pass == invalid_pass || pass >= pass_count)
                {
                    return;
                }
                if (!active_pass_flags[pass])
                {
                    active_pass_flags[pass] = true;
                    culling_worklist.push(pass);
                }
            };

            auto enqueue_image_producer = [&](resource_version_handle version)
            {
                if (version == invalid_resource_version)
                {
                    return;
                }
                const auto image = unpack_to_resource(version);
                const auto ver   = unpack_to_version(version);
                if (image >= image_count)
                {
                    return;
                }
                const auto base = producer_lookup_table.img_version_offsets[image];
                const auto end  = producer_lookup_table.img_version_offsets[image + 1];
                const auto idx  = static_cast<uint32_t>(base + ver);
                if (idx >= end)
                {
                    return;
                }
                enqueue_pass(producer_lookup_table.img_version_producers[idx]);
            };

            auto enqueue_buffer_producer = [&](resource_version_handle version)
            {
                if (version == invalid_resource_version)
                {
                    return;
                }
                const auto buffer = unpack_to_resource(version);
                const auto ver    = unpack_to_version(version);
                if (buffer >= buffer_count)
                {
                    return;
                }
                const auto base = producer_lookup_table.buf_version_offsets[buffer];
                const auto end  = producer_lookup_table.buf_version_offsets[buffer + 1];
                const auto idx  = static_cast<uint32_t>(base + ver);
                if (idx >= end)
                {
                    return;
                }
                enqueue_pass(producer_lookup_table.buf_version_producers[idx]);
            };

            auto get_image_producer = [&](resource_version_handle version) -> pass_handle
            {
                if (version == invalid_resource_version)
                {
                    return invalid_pass;
                }
                const auto image_handle = unpack_to_resource(version);
                const auto ver          = unpack_to_version(version);
                if (image_handle >= image_count)
                {
                    return invalid_pass;
                }
                const auto base = producer_lookup_table.img_version_offsets[image_handle];
                const auto end  = producer_lookup_table.img_version_offsets[image_handle + 1];
                const auto idx  = static_cast<uint32_t>(base + ver);
                if (idx >= end)
                {
                    return invalid_pass;
                }
                return producer_lookup_table.img_version_producers[idx];
            };

            auto get_buffer_producer = [&](resource_version_handle version) -> pass_handle
            {
                if (version == invalid_resource_version)
                {
                    return invalid_pass;
                }
                const auto buffer_handle = unpack_to_resource(version);
                const auto ver           = unpack_to_version(version);
                if (buffer_handle >= buffer_count)
                {
                    return invalid_pass;
                }
                const auto base = producer_lookup_table.buf_version_offsets[buffer_handle];
                const auto end  = producer_lookup_table.buf_version_offsets[buffer_handle + 1];
                const auto idx  = static_cast<uint32_t>(base + ver);
                if (idx >= end)
                {
                    return invalid_pass;
                }
                return producer_lookup_table.buf_version_producers[idx];
            };

            // Seed roots from declared outputs (images/buffers)
            for (const auto output_image : output_table.image_outputs)
            {
                if (output_image < image_count)
                {
                    enqueue_image_producer(producer_lookup_table.latest_img[output_image]);
                }
            }
            for (const auto output_buffer : output_table.buffer_outputs)
            {
                if (output_buffer < buffer_count)
                {
                    enqueue_buffer_producer(producer_lookup_table.latest_buf[output_buffer]);
                }
            }

            // Reverse traversal: if a live pass reads a resource, its producer must be live.
            while (!culling_worklist.empty())
            {
                const auto current_pass = culling_worklist.front();
                culling_worklist.pop();

                // image read dependencies
                {
                    const auto read_begin  = image_read_deps.begins[current_pass];
                    const auto read_length = image_read_deps.lengthes[current_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        enqueue_image_producer(img_ver_read_handles[j]);
                    }
                }

                // buffer read dependencies
                {
                    const auto read_begin  = buffer_read_deps.begins[current_pass];
                    const auto read_length = buffer_read_deps.lengthes[current_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        enqueue_buffer_producer(buf_ver_read_handles[j]);
                    }
                }
            }

            // Step E: Validate Resource
            // Validate graph correctness early and fail fast in debug builds.
            // Typical checks:
            // - Read-before-write on non-imported resources (producer == invalid_pass)
            // - Out-of-range resource handles in deps lists
            // - Empty output set (no roots) -> everything will be culled

            // check outputs
            assert((!output_table.image_outputs.empty() || !output_table.buffer_outputs.empty()) && "Error: No outputs declared");

            // check read-before-write issues and out-of-range handles
            for (size_t i = 0; i < pass_count; i++)
            {
                if (!active_pass_flags[i])
                    continue;

                const auto current_pass = graph.passes[i];
                // image reads
                {
                    const auto read_begin  = image_read_deps.begins[current_pass];
                    const auto read_length = image_read_deps.lengthes[current_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        const auto image_handle = image_read_deps.read_list[j];
                        if (image_handle >= image_count)
                        {
                            assert(false && "Error: Image read out-of-range detected!");
                        }

                        const auto version_handle = img_ver_read_handles[j];
                        const bool is_imported    = meta_table.image_metas.is_imported[image_handle];
                        const auto producer       = get_image_producer(version_handle);

                        if (version_handle == invalid_resource_version)
                        {
                            // next_version==0: no internal write happened before this read.
                            // This is only legal for imported resources.
                            assert(is_imported && "Error: Image read-before-write detected!");
                        }
                        else
                        {
                            assert((is_imported || producer != invalid_pass) && "Error: Image read-before-write detected!");
                        }
                    }
                }
                // buffer reads
                {
                    const auto read_begin  = buffer_read_deps.begins[current_pass];
                    const auto read_length = buffer_read_deps.lengthes[current_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        const auto buffer_handle = buffer_read_deps.read_list[j];
                        if (buffer_handle >= buffer_count)
                        {
                            assert(false && "Error: Buffer read out-of-range detected!");
                        }

                        const auto version_handle = buf_ver_read_handles[j];
                        const bool is_imported    = meta_table.buffer_metas.is_imported[buffer_handle];
                        const auto producer       = get_buffer_producer(version_handle);

                        if (version_handle == invalid_resource_version)
                        {
                            assert(is_imported && "Error: Buffer read-before-write detected!");
                        }
                        else
                        {
                            assert((is_imported || producer != invalid_pass) && "Error: Buffer read-before-write detected!");
                        }
                    }
                }
                // image writes
                {
                    const auto write_begin  = image_write_deps.begins[current_pass];
                    const auto write_length = image_write_deps.lengthes[current_pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        const auto image_handle = image_write_deps.write_list[j];
                        assert(image_handle < image_count && "Error: Image write out-of-range detected!");
                        assert(img_ver_write_handles[j] != invalid_resource_version && "Error: Image write out-of-range detected!");
                    }
                }
                // buffer writes
                {
                    const auto write_begin  = buffer_write_deps.begins[current_pass];
                    const auto write_length = buffer_write_deps.lengthes[current_pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        const auto buffer_handle = buffer_write_deps.write_list[j];
                        assert(buffer_handle < buffer_count && "Error: Buffer write out-of-range detected!");
                        assert(buf_ver_write_handles[j] != invalid_resource_version && "Error: Buffer write out-of-range detected!");
                    }
                }
            }

            // Step F: DAG Construction (Not yet implemented)
            // Build pass-to-pass edges based on read dependencies and producer lookup:
            // - For each live pass P and each resource R in P.read_list:
            //   producer = proc_map[R]; if producer valid and producer != P => add edge producer -> P
            // Output:
            // - adjacency list (or CSR) for passes
            // - in-degree counts for topo sort

            // Forward adjacency (CSR): producer -> consumer.
            // We build edges for all active passes (already culled from declared outputs).
            std::vector<std::vector<pass_handle>> outgoing(pass_count);
            auto add_edge = [&](pass_handle from, pass_handle to)
            {
                if (from == invalid_pass || to == invalid_pass)
                {
                    return;
                }
                if (from >= pass_count || to >= pass_count)
                {
                    return;
                }
                if (from == to)
                {
                    return;
                }
                if (!active_pass_flags[from] || !active_pass_flags[to])
                {
                    return;
                }
                outgoing[from].push_back(to);
            };

            for (size_t i = 0; i < pass_count; i++)
            {
                const auto consumer_pass = graph.passes[i];
                if (!active_pass_flags[consumer_pass])
                {
                    continue;
                }

                // image read dependencies: producer(img_ver_read) -> consumer
                {
                    const auto read_begin  = image_read_deps.begins[consumer_pass];
                    const auto read_length = image_read_deps.lengthes[consumer_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        const auto producer = get_image_producer(img_ver_read_handles[j]);
                        add_edge(producer, consumer_pass);
                    }
                }

                // buffer read dependencies: producer(buf_ver_read) -> consumer
                {
                    const auto read_begin  = buffer_read_deps.begins[consumer_pass];
                    const auto read_length = buffer_read_deps.lengthes[consumer_pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        const auto producer = get_buffer_producer(buf_ver_read_handles[j]);
                        add_edge(producer, consumer_pass);
                    }
                }
            }

            dag.adjacency_list.clear();
            dag.adjacency_begins.assign(static_cast<size_t>(pass_count) + 1, 0);
            dag.in_degrees.assign(pass_count, 0);
            dag.out_degrees.assign(pass_count, 0);

            // De-duplicate edges per producer and compute degrees.
            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                auto& list = outgoing[pass];
                std::sort(list.begin(), list.end());
                list.erase(std::unique(list.begin(), list.end()), list.end());
            }
            for (pass_handle from = 0; from < pass_count; from++)
            {
                dag.out_degrees[from] = static_cast<uint32_t>(outgoing[from].size());
                for (const auto dst_pass : outgoing[from])
                {
                    dag.in_degrees[dst_pass]++;
                }
            }

            // Build CSR arrays.
            uint32_t running = 0;
            for (pass_handle from = 0; from < pass_count; from++)
            {
                dag.adjacency_begins[from] = running;
                const auto& list           = outgoing[from];
                dag.adjacency_list.insert(dag.adjacency_list.end(), list.begin(), list.end());
                running = static_cast<uint32_t>(dag.adjacency_list.size());
            }
            dag.adjacency_begins[pass_count] = running;

            // Step G: Scheduling / Topological Order
            // Compute execution order for live passes (Kahn's algorithm).
            // This also validates that there are no cycles.

            sorted_passes.clear();
            sorted_passes.reserve(pass_count);
            std::vector<uint32_t> in_degrees_copy = dag.in_degrees;
            std::queue<pass_handle> zero_in_degree_queue;
            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                if (active_pass_flags[pass] && in_degrees_copy[pass] == 0)
                {
                    zero_in_degree_queue.push(pass);
                }
            }

            while (!zero_in_degree_queue.empty())
            {
                const auto current_pass = zero_in_degree_queue.front();
                zero_in_degree_queue.pop();

                sorted_passes.push_back(current_pass);

                const auto begin = dag.adjacency_begins[current_pass];
                const auto end   = dag.adjacency_begins[current_pass + 1];
                for (auto j = begin; j < end; j++)
                {
                    const auto dst_pass = dag.adjacency_list[j];
                    in_degrees_copy[dst_pass]--;
                    if (in_degrees_copy[dst_pass] == 0)
                    {
                        zero_in_degree_queue.push(dst_pass);
                    }
                }
            }
            const size_t active_pass_count = static_cast<size_t>(std::count(active_pass_flags.begin(), active_pass_flags.end(), true));
            assert(sorted_passes.size() == active_pass_count && "Error: Cycle detected in render graph!");

            // Step H: Lifetime Analysis & Aliasing
            // For each resource version, compute first/last use across the scheduled pass order.
            // Use this to:
            // - allocate transient resources from pools
            // - alias memory for non-overlapping lifetimes

            // 1. Build Pass Index Map (Handle -> Execution Order Index)
            // We need strictly monotonic indices to compare lifetimes correctly.
            std::vector<uint32_t> sorted_pass_indices(pass_count, 0);
            for (uint32_t i = 0; i < sorted_passes.size(); i++)
            {
                sorted_pass_indices[sorted_passes[i]] = i;
            }

            resource_lifetimes.clear();
            resource_lifetimes.image_first_used_pass.assign(image_count, invalid_pass);
            resource_lifetimes.image_last_used_pass.assign(image_count, 0);
            resource_lifetimes.buffer_first_used_pass.assign(buffer_count, invalid_pass);
            resource_lifetimes.buffer_last_used_pass.assign(buffer_count, 0);

            // 2. Compute Lifetimes (using execution indices)
            for (const auto pass : sorted_passes)
            {
                const uint32_t actual_pass_index = sorted_pass_indices[pass];

                auto update_lifetime = [&](std::vector<uint32_t>& firsts, std::vector<uint32_t>& lasts, resource_handle res, size_t count)
                {
                    if (res >= count) return;
                    if (firsts[res] == invalid_pass)
                    {
                        firsts[res] = actual_pass_index;
                    }
                    lasts[res] = actual_pass_index;
                };

                // image reads
                {
                    const auto read_begin  = image_read_deps.begins[pass];
                    const auto read_length = image_read_deps.lengthes[pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        update_lifetime(resource_lifetimes.image_first_used_pass, resource_lifetimes.image_last_used_pass, image_read_deps.read_list[j], image_count);
                    }
                }
                // image writes
                {
                    const auto write_begin  = image_write_deps.begins[pass];
                    const auto write_length = image_write_deps.lengthes[pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        update_lifetime(resource_lifetimes.image_first_used_pass, resource_lifetimes.image_last_used_pass, image_write_deps.write_list[j], image_count);
                    }
                }
                // buffer reads
                {
                    const auto read_begin  = buffer_read_deps.begins[pass];
                    const auto read_length = buffer_read_deps.lengthes[pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        update_lifetime(resource_lifetimes.buffer_first_used_pass, resource_lifetimes.buffer_last_used_pass, buffer_read_deps.read_list[j], buffer_count);
                    }
                }
                // buffer writes
                {
                    const auto write_begin  = buffer_write_deps.begins[pass];
                    const auto write_length = buffer_write_deps.lengthes[pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        update_lifetime(resource_lifetimes.buffer_first_used_pass, resource_lifetimes.buffer_last_used_pass, buffer_write_deps.write_list[j], buffer_count);
                    }
                }
            }

            // 3. Aliasing (Greedy First-Fit)
            // Group resources that can share memory (transient & non-overlapping).
            physical_resource_metas.clear();
            
            auto is_overlapping = [](uint32_t start_a, uint32_t end_a, uint32_t start_b, uint32_t end_b)
            {
                return std::max(start_a, start_b) <= std::min(end_a, end_b);
            };

            // Images
            {
                // Stores intervals for each unique resource: unique_id -> vector<{start, end}>
                std::vector<std::vector<std::pair<uint32_t, uint32_t>>> life_intervals;
                
                // Resize mapping table
                physical_resource_metas.handle_to_physical_img_id.assign(image_count, invalid_resource);

                for (resource_handle img = 0; img < image_count; img++)
                {
                    const auto first = resource_lifetimes.image_first_used_pass[img];
                    const auto last  = resource_lifetimes.image_last_used_pass[img];

                    // Skip unused
                    if (first == invalid_pass) continue;

                    // Imported resources cannot be aliased (they are external)
                    // We assign them a unique ID but don't merge them.
                    if (meta_table.image_metas.is_imported[img])
                    {
                        const auto unique_id = static_cast<resource_handle>(physical_resource_metas.physical_image_meta.size());
                        physical_resource_metas.physical_image_meta.push_back(img);
                        physical_resource_metas.handle_to_physical_img_id[img] = unique_id;
                        // We don't track intervals for imported resources as we don't manage their memory
                        life_intervals.emplace_back(); 
                        continue;
                    }

                    bool assigned = false;
                    for (size_t u = 0; u < life_intervals.size(); u++)
                    {
                        // Skip if this unique resource slot is for an imported resource (empty intervals)
                        if (life_intervals[u].empty()) continue;

                        // Check 1: Compatibility (Format, Size, etc.)
                        // For now, we require strict equality of meta.
                        const auto rep_img = physical_resource_metas.physical_image_meta[u];
                        if (!meta_table.image_metas.is_compatible(rep_img, img)) continue;

                        // Check 2: Overlap
                        bool overlaps = false;
                        for (const auto& interval : life_intervals[u])
                        {
                            if (is_overlapping(first, last, interval.first, interval.second))
                            {
                                overlaps = true;
                                break;
                            }
                        }

                        if (!overlaps)
                        {
                            life_intervals[u].emplace_back(first, last);
                            physical_resource_metas.handle_to_physical_img_id[img] = static_cast<resource_handle>(u);
                            assigned = true;
                            break;
                        }
                    }

                    if (!assigned)
                    {
                        const auto unique_id = static_cast<resource_handle>(physical_resource_metas.physical_image_meta.size());
                        physical_resource_metas.physical_image_meta.push_back(img);
                        physical_resource_metas.handle_to_physical_img_id[img] = unique_id;
                        life_intervals.push_back({{first, last}});
                    }
                }
            }

            // Buffers
            {
                std::vector<std::vector<std::pair<uint32_t, uint32_t>>> life_intervals;
                physical_resource_metas.handle_to_physical_buf_id.assign(buffer_count, invalid_resource);

                for (resource_handle buf = 0; buf < buffer_count; buf++)
                {
                    const auto first = resource_lifetimes.buffer_first_used_pass[buf];
                    const auto last  = resource_lifetimes.buffer_last_used_pass[buf];

                    if (first == invalid_pass) continue;

                    if (meta_table.buffer_metas.is_imported[buf])
                    {
                        const auto unique_id = static_cast<resource_handle>(physical_resource_metas.physical_buffer_meta.size());
                        physical_resource_metas.physical_buffer_meta.push_back(buf);
                        physical_resource_metas.handle_to_physical_buf_id[buf] = unique_id;
                        life_intervals.emplace_back();
                        continue;
                    }

                    bool assigned = false;
                    for (size_t u = 0; u < life_intervals.size(); u++)
                    {
                        if (life_intervals[u].empty()) continue;

                        // Check Compatibility
                        const auto rep_buf = physical_resource_metas.physical_buffer_meta[u];
                        if (!meta_table.buffer_metas.is_compatible(rep_buf, buf)) continue;

                        bool overlaps = false;
                        for (const auto& interval : life_intervals[u])
                        {
                            if (is_overlapping(first, last, interval.first, interval.second))
                            {
                                overlaps = true;
                                break;
                            }
                        }

                        if (!overlaps)
                        {
                            life_intervals[u].emplace_back(first, last);
                            physical_resource_metas.handle_to_physical_buf_id[buf] = static_cast<resource_handle>(u);
                            assigned = true;
                            break;
                        }
                    }

                    if (!assigned)
                    {
                        const auto unique_id = static_cast<resource_handle>(physical_resource_metas.physical_buffer_meta.size());
                        physical_resource_metas.physical_buffer_meta.push_back(buf);
                        physical_resource_metas.handle_to_physical_buf_id[buf] = unique_id;
                        life_intervals.push_back({{first, last}});
                    }
                }
            }

            // Step I: Build Synchronization Plan  (Barriers)
            // Build an API-agnostic per-pass barrier list based on scheduled order.

            per_pass_barriers.clear();
            per_pass_barriers.resize_passes(pass_count);

            // Scratch per-pass AoS; we will flatten into per_pass_barrier (CSR + SoA) afterwards.
            std::vector<std::vector<barrier_op>> scratch(pass_count);

            struct last_use
            {
                resource_handle logical   = 0;
                uint32_t usage_bits       = 0;
                pipeline_domain domain    = pipeline_domain::any;
                access_type access        = access_type::read;
                bool valid                = false;
            };

            const auto invalid_physical = invalid_resource;
            std::vector<last_use> last_img_use(physical_resource_metas.physical_image_meta.size());
            std::vector<last_use> last_buf_use(physical_resource_metas.physical_buffer_meta.size());

            auto to_access = [](bool has_read, bool has_write) -> access_type
            {
                if (has_read && has_write) return access_type::read_write;
                if (has_write) return access_type::write;
                return access_type::read;
            };

            auto needs_uav_like = [](resource_kind kind, uint32_t usage_bits) -> bool
            {
                if (kind == resource_kind::image)
                {
                    return (usage_bits & static_cast<uint32_t>(image_usage::STORAGE)) != 0;
                }
                return (usage_bits & static_cast<uint32_t>(buffer_usage::STORAGE_BUFFER)) != 0;
            };

            auto insert_barrier = [&](pass_handle pass,
                                    resource_kind kind,
                                    resource_handle logical,
                                    resource_handle physical,
                                    access_type desired_access,
                                    uint32_t desired_usage_bits)
            {
                // validate physical id
                if (physical == invalid_physical) return;

                // get last use record
                auto& last_vec = (kind == resource_kind::image) ? last_img_use : last_buf_use;
                if (physical >= last_vec.size()) return;
                auto& last = last_vec[physical];

                // if this physical id was previously used by a different logical resource, insert an aliasing barrier.
                if (last.valid && last.logical != logical)
                {
                    barrier_op op;
                    op.type         = barrier_op_type::aliasing;
                    op.kind         = kind;
                    op.logical      = logical;
                    op.prev_logical = last.logical;
                    op.physical     = physical;
                    scratch[pass].push_back(op);
                }

                // if state/usage changed across passes, insert a transition op.
                // note: backends decide what 'transition' means (Vk layout+barrier, D3D12 state transition, etc.).
                if (last.valid)
                {
                    const bool changed = (last.usage_bits != desired_usage_bits) || (last.access != desired_access) || (last.domain != pipeline_domain::any);
                    if (changed)
                    {
                        barrier_op op;
                        op.type          = barrier_op_type::transition;
                        op.kind          = kind;
                        op.logical       = logical;
                        op.physical      = physical;
                        op.src_domain    = last.domain;
                        op.dst_domain    = pipeline_domain::any;
                        op.src_access    = last.access;
                        op.dst_access    = desired_access;
                        op.src_usage_bits = last.usage_bits;
                        op.dst_usage_bits = desired_usage_bits;
                        scratch[pass].push_back(op);
                    }

                    // UAV-like ordering: write -> (read/write) on storage resources.
                    if (last.access != access_type::read && needs_uav_like(kind, desired_usage_bits))
                    {
                        barrier_op op;
                        op.type     = barrier_op_type::uav;
                        op.kind     = kind;
                        op.logical  = logical;
                        op.physical = physical;
                        scratch[pass].push_back(op);
                    }
                }
                
                // Update last use info
                last.valid      = true;
                last.logical    = logical;
                last.access     = desired_access;
                last.domain     = pipeline_domain::any;
                last.usage_bits = desired_usage_bits;
            };

            // Walk scheduled passes and build barriers for all resources they touch.
            for (const auto pass : sorted_passes)
            {
                // Images used by this pass
                {
                    std::unordered_map<resource_handle, std::pair<bool, bool>> rw; // logical -> {read, write}
                    std::unordered_map<resource_handle, uint32_t> usage;

                    const auto r_begin  = image_read_deps.begins[pass];
                    const auto r_len    = image_read_deps.lengthes[pass];
                    for (auto j = r_begin; j < r_begin + r_len; j++)
                    {
                        const auto logical = image_read_deps.read_list[j];
                        rw[logical].first  = true;
                        usage[logical] |= image_read_deps.usage_bits[j];
                    }

                    const auto w_begin = image_write_deps.begins[pass];
                    const auto w_len   = image_write_deps.lengthes[pass];
                    for (auto j = w_begin; j < w_begin + w_len; j++)
                    {
                        const auto logical  = image_write_deps.write_list[j];
                        rw[logical].second  = true;
                        usage[logical] |= image_write_deps.usage_bits[j];
                    }

                    for (const auto& [logical, flags] : rw)
                    {
                        const auto physical = (logical < physical_resource_metas.handle_to_physical_img_id.size())
                                                  ? static_cast<resource_handle>(physical_resource_metas.handle_to_physical_img_id[logical])
                                                  : invalid_physical;
                        insert_barrier(pass, resource_kind::image, logical, physical, to_access(flags.first, flags.second), usage[logical]);
                    }
                }

                // Buffers used by this pass
                {
                    std::unordered_map<resource_handle, std::pair<bool, bool>> rw; // logical -> {read, write}
                    std::unordered_map<resource_handle, uint32_t> usage;

                    const auto r_begin  = buffer_read_deps.begins[pass];
                    const auto r_len    = buffer_read_deps.lengthes[pass];
                    for (auto j = r_begin; j < r_begin + r_len; j++)
                    {
                        const auto logical = buffer_read_deps.read_list[j];
                        rw[logical].first  = true;
                        usage[logical] |= buffer_read_deps.usage_bits[j];
                    }

                    const auto w_begin = buffer_write_deps.begins[pass];
                    const auto w_len   = buffer_write_deps.lengthes[pass];
                    for (auto j = w_begin; j < w_begin + w_len; j++)
                    {
                        const auto logical = buffer_write_deps.write_list[j];
                        rw[logical].second = true;
                        usage[logical] |= buffer_write_deps.usage_bits[j];
                    }

                    for (const auto& [logical, flags] : rw)
                    {
                        const auto physical = (logical < physical_resource_metas.handle_to_physical_buf_id.size())
                                                  ? static_cast<resource_handle>(physical_resource_metas.handle_to_physical_buf_id[logical])
                                                  : invalid_physical;
                        insert_barrier(pass, resource_kind::buffer, logical, physical, to_access(flags.first, flags.second), usage[logical]);
                    }
                }
            }

            // Flatten scratch into per_pass_barrier (CSR + SoA).
            uint32_t barrier_running = 0;
            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                per_pass_barriers.pass_begins[pass]  = barrier_running;
                per_pass_barriers.pass_lengths[pass] = static_cast<uint32_t>(scratch[pass].size());
                barrier_running += per_pass_barriers.pass_lengths[pass];
            }
            per_pass_barriers.pass_begins[pass_count] = barrier_running;

            per_pass_barriers.resize_ops(barrier_running);

            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                const auto base = per_pass_barriers.pass_begins[pass];
                const auto len  = per_pass_barriers.pass_lengths[pass];
                for (uint32_t i = 0; i < len; i++)
                {
                    const auto& op = scratch[pass][i];
                    const auto idx = base + i;

                    per_pass_barriers.types[idx] = op.type;
                    per_pass_barriers.kinds[idx] = op.kind;
                    per_pass_barriers.logicals[idx] = op.logical;
                    per_pass_barriers.physicals[idx] = op.physical;
                    per_pass_barriers.src_domains[idx] = op.src_domain;
                    per_pass_barriers.dst_domains[idx] = op.dst_domain;
                    per_pass_barriers.src_accesses[idx] = op.src_access;
                    per_pass_barriers.dst_accesses[idx] = op.dst_access;
                    per_pass_barriers.src_usage_bits[idx] = op.src_usage_bits;
                    per_pass_barriers.dst_usage_bits[idx] = op.dst_usage_bits;
                    per_pass_barriers.prev_logicals[idx] = op.prev_logical;
                }
            }


            // Step J: Physical Resource Allocation (Not yet implemented)
            // Create actual GPU resources for live, non-imported resources.
            // - Filter out culled passes and unused resources
            // - Imported resources: do not create; expect bind_imported_* later (frame loop)
            // - Call backend to create/realize resources (possibly from pools)

            if (backend != nullptr)
            {
                backend->on_compile_resource_allocation(meta_table, physical_resource_metas);
            }
        }

        // 3. Execution System
        void execute()
        {
            if (backend == nullptr)
            {
                return;
            }

            pass_execute_context exec_ctx{.backend = backend};

            for (const auto pass : sorted_passes)
            {
                backend->apply_barriers(pass, per_pass_barriers);

                if (pass < graph.execute_funcs.size() && graph.execute_funcs[pass])
                {
                    graph.execute_funcs[pass](exec_ctx);
                }
            }
        }

        void clear()
        {
            meta_table.clear();
        }

        // Kahn-based cycle validation for a pass dependency DAG.
        // NOTE: This is primarily for debug validation / unit tests.
        static void assert_no_cycles(const directed_acyclic_graph& dag, const std::vector<bool>& active_pass_flags)
        {
            const auto pass_count = active_pass_flags.size();
            if (dag.in_degrees.size() != pass_count || dag.adjacency_begins.size() != pass_count + 1)
            {
                assert(false && "Error: DAG arrays shape mismatch!");
            }

            std::vector<uint32_t> in_degrees_copy = dag.in_degrees;
            std::queue<pass_handle> zero_in_degree_queue;

            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                if (active_pass_flags[pass] && in_degrees_copy[pass] == 0)
                {
                    zero_in_degree_queue.push(pass);
                }
            }

            size_t visited = 0;
            while (!zero_in_degree_queue.empty())
            {
                const auto current_pass = zero_in_degree_queue.front();
                zero_in_degree_queue.pop();
                visited++;

                const auto begin = dag.adjacency_begins[current_pass];
                const auto end   = dag.adjacency_begins[current_pass + 1];
                for (auto j = begin; j < end; j++)
                {
                    const auto dst_pass = dag.adjacency_list[j];
                    if (!active_pass_flags[dst_pass])
                    {
                        continue;
                    }
                    in_degrees_copy[dst_pass]--;
                    if (in_degrees_copy[dst_pass] == 0)
                    {
                        zero_in_degree_queue.push(dst_pass);
                    }
                }
            }

            const size_t active_pass_count = static_cast<size_t>(std::count(active_pass_flags.begin(), active_pass_flags.end(), true));
            assert(visited == active_pass_count && "Error: Cycle detected in render graph!");
        }
    };

} // namespace render_graph
