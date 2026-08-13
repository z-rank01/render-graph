// Core graph compiler: lifts a frame recipe into plan rows, builds the
// dependency DAG, schedules the passes, computes lifetimes and per-resource
// synchronization, then packs the submissions and publishes the compiled
// plan for the backends to record.
#include "system.h"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <type_traits>

namespace render_graph
{
    // =========================================================================
    // Plan reset: restores a compiled plan to its empty state
    // =========================================================================
    void compiled_graph_plan::clear()
    {
        cache_key = 0;
        resources.clear();
        frame_buffers.clear();
        frame_images.clear();
        upload_buffer = invalid_buffer;
        passes.clear();
        scheduled_passes.clear();
        lifetimes.clear();
        physical_resources.clear();
        synchronization.clear();
        submissions.clear();
        statistics = {};
    }
} // namespace render_graph

namespace render_graph::core
{
    // =========================================================================
    // Internal helpers
    // =========================================================================
    namespace
    {
        constexpr uint32_t invalid_source_pass = std::numeric_limits<uint32_t>::max();

        // --- Diagnostics and validation ---
        void fail(compiler_state& state,
                  compile_error_code code,
                  std::string message,
                  pass_handle pass         = invalid_pass,
                  resource_kind kind       = resource_kind::image,
                  resource_handle resource = invalid_resource)
        {
            auto diagnostic = compile_diagnostic{
                .code     = code,
                .pass     = pass,
                .kind     = kind,
                .resource = resource,
                .message  = std::move(message),
            };
            if (pass != invalid_pass && pass < state.output.plan.passes.size())
                diagnostic.pass_name = state.output.plan.passes.names[pass];
            state.output.result.diagnostics.push_back(std::move(diagnostic));
        }

        // --- Pass row construction (SoA) ---

        // FNV-1a 64-bit hash over the pass name (name_hashes column).
        uint64_t hash_pass_name(std::string_view name) noexcept
        {
            uint64_t hash = 14695981039346656037ULL;
            for (const auto byte : name)
            {
                hash ^= static_cast<uint8_t>(byte);
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        // Append one pass row across the scalar columns. The raster CSR rows
        // (color_begins/color_counts/depth_indices) are appended here too so
        // they stay index-aligned; color counts are finalized by the caller at
        // the end of the pass loop once attachments are appended.
        void push_pass_row(compiled_pass_rows& passes,
                           std::string name,
                           pass_kind kind,
                           queue_class queue,
                           uint32_t source_pass,
                           uint8_t flags,
                           render_area area)
        {
            passes.names.push_back(name);
            passes.name_hashes.push_back(hash_pass_name(name));
            passes.kinds.push_back(kind);
            passes.queues.push_back(queue);
            passes.source_passes.push_back(source_pass);
            passes.flags.push_back(flags);
            passes.areas.push_back(area);
            passes.layer_counts.push_back(1);
            passes.color_begins.push_back(static_cast<uint32_t>(passes.colors.size()));
            passes.color_counts.push_back(0);
            passes.depth_indices.push_back(invalid_depth_index);
        }

        bool valid_range(frame_row_range range, std::size_t size) noexcept
        {
            return range.begin <= size && range.count <= size - range.begin;
        }

        pipeline_domain domain(pass_kind kind) noexcept
        {
            if (kind == pass_kind::copy)
                return pipeline_domain::copy;
            if (kind == pass_kind::compute)
                return pipeline_domain::compute;
            return pipeline_domain::graphics;
        }

        access_type merge_access(access_type left, access_type right) noexcept
        {
            return left == right ? left : access_type::read_write;
        }

        queue_class available_queue(queue_class requested, const compile_environment& environment) noexcept
        {
            if (requested == queue_class::compute && !environment.queues.compute)
                return queue_class::graphics;
            if (requested == queue_class::copy && !environment.queues.copy)
                return queue_class::graphics;
            return requested;
        }

        // Row push into the per-kind event tables (no dedup here; sorting and
        // merging happen once after all rows are collected).
        void push_image_access(compiler_state& state,
                               pass_handle pass,
                               resource_handle logical,
                               access_type access,
                               image_usage usage,
                               pipeline_domain domain,
                               queue_class queue,
                               image_subresource_range range)
        {
            auto& table = state.image_events;
            table.passes.push_back(pass);
            table.logicals.push_back(logical);
            table.accesses.push_back(access);
            table.usages.push_back(usage);
            table.domains.push_back(domain);
            table.queues.push_back(queue);
            table.ranges.push_back(range);
        }

        void push_buffer_access(compiler_state& state,
                                pass_handle pass,
                                resource_handle logical,
                                access_type access,
                                buffer_usage usage,
                                pipeline_domain domain,
                                queue_class queue,
                                buffer_byte_range range)
        {
            auto& table = state.buffer_events;
            table.passes.push_back(pass);
            table.logicals.push_back(logical);
            table.accesses.push_back(access);
            table.usages.push_back(usage);
            table.domains.push_back(domain);
            table.queues.push_back(queue);
            table.ranges.push_back(range);
        }

        // Stable-sort one event table by (pass, logical), then merge rows that
        // share a (pass, logical) pair: usage is OR-ed, a differing range
        // collapses to whole (matches the former per-insert append_access
        // semantics, in O(E log E) instead of O(E²)).
        template <typename AccessRows>
        void sort_and_merge_rows(AccessRows& table)
        {
            const auto count = table.passes.size();
            if (count == 0)
                return;
            std::vector<uint32_t> order(count);
            std::iota(order.begin(), order.end(), 0U);
            std::stable_sort(order.begin(), order.end(),
                             [&](uint32_t left, uint32_t right)
                             {
                                 if (table.passes[left] != table.passes[right])
                                     return table.passes[left] < table.passes[right];
                                 return table.logicals[left] < table.logicals[right];
                             });

            AccessRows merged;
            merged.passes.reserve(count);
            merged.logicals.reserve(count);
            merged.accesses.reserve(count);
            merged.usages.reserve(count);
            merged.domains.reserve(count);
            merged.queues.reserve(count);
            merged.ranges.reserve(count);

            for (const auto index : order)
            {
                const auto pass    = table.passes[index];
                const auto logical = table.logicals[index];
                if (!merged.passes.empty() && merged.passes.back() == pass && merged.logicals.back() == logical)
                {
                    merged.accesses.back() = merge_access(merged.accesses.back(), table.accesses[index]);
                    merged.usages.back()   = merged.usages.back() | table.usages[index];
                    if (merged.ranges.back() != table.ranges[index])
                        merged.ranges.back() = {};
                }
                else
                {
                    merged.passes.push_back(pass);
                    merged.logicals.push_back(logical);
                    merged.accesses.push_back(table.accesses[index]);
                    merged.usages.push_back(table.usages[index]);
                    merged.domains.push_back(table.domains[index]);
                    merged.queues.push_back(table.queues[index]);
                    merged.ranges.push_back(table.ranges[index]);
                }
            }
            table = std::move(merged);
        }

        // Per-pass CSR index over a sorted event table:
        // events of pass p occupy rows [event_begins[p], event_begins[p + 1]).
        template <typename AccessRows>
        void build_event_begins(AccessRows& table, uint32_t pass_count)
        {
            table.event_begins.assign(pass_count + 1, 0);
            for (const auto pass : table.passes)
                ++table.event_begins[static_cast<uint32_t>(pass) + 1];
            for (uint32_t p = 0; p < pass_count; ++p)
                table.event_begins[p + 1] += table.event_begins[p];
        }

        // Drop rows whose pass was culled, preserving the sorted order, then
        // rebuild the per-pass CSR.
        template <typename AccessRows>
        void filter_active_rows(AccessRows& table, std::span<const uint8_t> active_passes, uint32_t pass_count)
        {
            std::size_t write = 0;
            for (std::size_t read = 0; read < table.passes.size(); ++read)
            {
                if (active_passes[table.passes[read]] == 0U)
                    continue;
                if (write != read)
                {
                    table.passes[write]   = table.passes[read];
                    table.logicals[write] = table.logicals[read];
                    table.accesses[write] = table.accesses[read];
                    table.usages[write]   = table.usages[read];
                    table.domains[write]  = table.domains[read];
                    table.queues[write]   = table.queues[read];
                    table.ranges[write]   = table.ranges[read];
                }
                ++write;
            }
            table.passes.resize(write);
            table.logicals.resize(write);
            table.accesses.resize(write);
            table.usages.resize(write);
            table.domains.resize(write);
            table.queues.resize(write);
            table.ranges.resize(write);
            build_event_begins(table, pass_count);
        }

        // --- Default memory requirements ---
        allocation_requirements default_image_requirements(const image_desc& desc)
        {
            return {
                .size =
                    std::max<uint64_t>(1, static_cast<uint64_t>(desc.extent.width) * desc.extent.height * desc.extent.depth * desc.array_layers * 4),
                .alignment          = 256,
                .memory_type_bits   = desc.memory == memory_domain::device_local ? 1U : 2U,
                .requires_dedicated = desc.allocation == allocation_policy::dedicated,
                .supports_aliasing  = desc.aliasing != aliasing_policy::forbidden && desc.allocation != allocation_policy::dedicated,
            };
        }

        allocation_requirements default_buffer_requirements(const buffer_desc& desc)
        {
            return {
                .size               = desc.size,
                .alignment          = 256,
                .memory_type_bits   = desc.memory == memory_domain::device_local ? 1U : 2U,
                .requires_dedicated = desc.allocation == allocation_policy::dedicated,
                .supports_aliasing  = desc.aliasing != aliasing_policy::forbidden && desc.allocation != allocation_policy::dedicated,
            };
        }

        // --- Transition analysis ---
        bool hazard(access_type before, access_type after) noexcept
        {
            return before != access_type::read || after != access_type::read;
        }

        synchronization_intent transition_intents(const abstract_resource_state& before,
                                                  const abstract_resource_state& after,
                                                  resource_kind kind) noexcept
        {
            synchronization_intent intents = synchronization_intent::none;
            if (kind == resource_kind::image && before.usage_bits != after.usage_bits)
                intents |= synchronization_intent::layout_transition;
            if (hazard(before.access, after.access))
                intents |= synchronization_intent::execution_dependency | synchronization_intent::memory_dependency;
            if (before.queue != after.queue)
                intents |= synchronization_intent::queue_ownership;
            return intents;
        }

        // Branch-free per-kind extraction of an event row into the shared
        // abstract state used by synchronization analysis.
        abstract_resource_state abstract_state(const image_access_rows& table, std::size_t index) noexcept
        {
            abstract_resource_state output{.access = table.accesses[index]};
            output.usage_bits  = static_cast<uint32_t>(table.usages[index]);
            output.domain      = table.domains[index];
            output.queue       = table.queues[index];
            output.image_range = table.ranges[index];
            return output;
        }

        abstract_resource_state abstract_state(const buffer_access_rows& table, std::size_t index) noexcept
        {
            abstract_resource_state output{.access = table.accesses[index]};
            output.usage_bits   = static_cast<uint32_t>(table.usages[index]);
            output.domain       = table.domains[index];
            output.queue        = table.queues[index];
            output.buffer_range = table.ranges[index];
            return output;
        }

        // Mix one field into the running cache key.
        uint64_t combine(uint64_t seed, uint64_t value) noexcept
        {
            return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
        }
    } // namespace

    // =========================================================================
    // Stage 1: recipe validation
    // =========================================================================
    bool validate_recipe(compiler_state& state)
    {
        if ((state.request == nullptr) || (state.request->frame == nullptr))
        {
            fail(state, compile_error_code::no_output, "graph compile request has no frame plan");
            return false;
        }

        const auto& frame = *state.request->frame;
        if (frame.passes.empty())
            fail(state, compile_error_code::no_output, "frame plan contains no passes");
        if (frame.passes.size() + (state.request->inject_stable_upload_pass ? 1U : 0U) > state.request->environment.limits.max_passes)
            fail(state, compile_error_code::pass_limit_exceeded, "frame pass limit exceeded");

        for (uint32_t index = 0; index < frame.passes.size(); ++index)
        {
            const auto& pass = frame.passes[index];
            if (!valid_range(pass.buffer_accesses, frame.buffer_accesses.size()) || !valid_range(pass.image_accesses, frame.image_accesses.size()) ||
                !valid_range(pass.attachments, frame.attachments.size()) || !valid_range(pass.buffer_copies, frame.buffer_copies.size()) ||
                !valid_range(pass.dispatches, frame.dispatches.size()) ||
                !valid_range(pass.indexed_indirect_draws, frame.indexed_indirect_draws.size()) ||
                pass.push_constant_offset > frame.push_constants.size() ||
                pass.push_constant_size > frame.push_constants.size() - pass.push_constant_offset)
                fail(state, compile_error_code::access_limit_exceeded, "frame pass contains an out-of-range row span", pass_handle{index});
            if ((pass.kind == pass_kind::raster && (pass.buffer_copies.count != 0 || pass.dispatches.count != 0)) ||
                (pass.kind == pass_kind::compute &&
                 (pass.attachments.count != 0 || pass.buffer_copies.count != 0 || pass.indexed_indirect_draws.count != 0)) ||
                (pass.kind == pass_kind::copy &&
                 (pass.attachments.count != 0 || pass.dispatches.count != 0 || pass.indexed_indirect_draws.count != 0)))
                fail(state, compile_error_code::unsupported_feature, "frame commands are incompatible with the pass kind", pass_handle{index});
            if (pass.kind == pass_kind::raster && !state.request->capabilities.supports_graphics)
                fail(state, compile_error_code::unsupported_feature, "graphics passes are unsupported", pass_handle{index});
            if (pass.kind == pass_kind::compute && !state.request->capabilities.supports_compute)
                fail(state, compile_error_code::unsupported_feature, "compute passes are unsupported", pass_handle{index});
            if (pass.kind == pass_kind::copy && !state.request->capabilities.supports_copy)
                fail(state, compile_error_code::unsupported_feature, "copy passes are unsupported", pass_handle{index});
        }
        return state.output.result.succeeded();
    }

    // =========================================================================
    // Stage 2: resource lifting
    // =========================================================================
    bool build_resource_versions(compiler_state& state)
    {
        const auto& request = *state.request;
        const auto& frame   = *request.frame;
        auto& plan          = state.output.plan;
        plan.frame_buffers.assign(frame.resources.size(), invalid_buffer);
        plan.frame_images.assign(frame.resources.size(), invalid_image);

        // --- Optional stable upload pass: copy persistent buffers in one stage ---
        if (request.inject_stable_upload_pass)
        {
            auto desc     = request.upload_buffer_desc;
            desc.lifetime = resource_lifetime_class::imported;
            plan.upload_buffer = plan.resources.buffer_metas.add("UploadArena", desc, resource_lifetime_class::imported, hash_resource_desc(desc), true);
            state.buffer_contract_indices.push_back(invalid_contract_index);
            push_pass_row(plan.passes, "UploadPass", pass_kind::copy,
                          available_queue(queue_class::copy, request.environment), invalid_source_pass,
                          pass_flag_backend_upload, {});
        }

        // --- Frame resources: register buffers and images into the plan ---
        for (uint32_t index = 0; index < frame.resources.size(); ++index)
        {
            const auto& resource = frame.resources[index];
            if (resource.name.empty())
            {
                fail(state, compile_error_code::backend_failure, "frame resource name must not be empty");
                continue;
            }
            if (resource.source == frame_resource_source::persistent_buffer || resource.source == frame_resource_source::transient_buffer)
            {
                buffer_desc desc    = resource.buffer_description;
                const bool imported = resource.source == frame_resource_source::persistent_buffer;
                if (imported && ((request.descriptions.describe_buffer == nullptr) ||
                                 !request.descriptions.describe_buffer(request.descriptions.state, resource.buffer, desc)))
                {
                    fail(state, compile_error_code::backend_failure, "persistent buffer handle is stale", invalid_pass, resource_kind::buffer, index);
                    continue;
                }
                desc.lifetime         = imported ? resource_lifetime_class::imported : resource_lifetime_class::transient;
                const auto validation = (request.validation.validate_buffer != nullptr)
                                            ? request.validation.validate_buffer(request.validation.state, desc)
                                            : validate_resource_desc(nullptr, desc);
                if (!validation)
                {
                    fail(state, compile_error_code::unsupported_feature, validation.message, invalid_pass, resource_kind::buffer, index);
                    continue;
                }
                plan.frame_buffers[index] =
                    plan.resources.buffer_metas.add(std::string(resource.name), desc, desc.lifetime, hash_resource_desc(desc), imported);
                state.buffer_contract_indices.push_back(invalid_contract_index);
            }
            else
            {
                image_desc desc = resource.image_description;
                bool imported =
                    resource.source == frame_resource_source::persistent_image || resource.source == frame_resource_source::swapchain_image;
                if (resource.source == frame_resource_source::persistent_image &&
                    ((request.descriptions.describe_image == nullptr) ||
                     !request.descriptions.describe_image(request.descriptions.state, resource.image, desc)))
                {
                    fail(state, compile_error_code::backend_failure, "persistent image handle is stale", invalid_pass, resource_kind::image, index);
                    continue;
                }
                if (resource.source == frame_resource_source::swapchain_image)
                {
                    desc = image_desc{
                        .fmt      = request.environment.color_format,
                        .extent   = request.environment.extent,
                        .usage    = image_usage::COLOR_ATTACHMENT | image_usage::PRESENT,
                        .aliasing = aliasing_policy::forbidden,
                        .lifetime = resource_lifetime_class::imported,
                    };
                }
                else
                    desc.lifetime = imported ? resource_lifetime_class::imported : resource_lifetime_class::transient;
                const auto validation = (request.validation.validate_image != nullptr)
                                            ? request.validation.validate_image(request.validation.state, desc)
                                            : validate_resource_desc(nullptr, desc);
                if (!validation)
                {
                    fail(state, compile_error_code::unsupported_feature, validation.message, invalid_pass, resource_kind::image, index);
                    continue;
                }
                plan.frame_images[index] =
                    plan.resources.image_metas.add(std::string(resource.name), desc, desc.lifetime, hash_resource_desc(desc), imported);
                state.image_contract_indices.push_back(invalid_contract_index);
            }
        }

        // --- Pass rows: one compiled row per frame pass ---
        const auto frame_area = render_area{.width = request.environment.extent.width, .height = request.environment.extent.height};
        for (uint32_t source = 0; source < frame.passes.size(); ++source)
        {
            const auto& row   = frame.passes[source];
            const auto flags  = row.side_effect ? pass_flag_side_effect : 0U;
            push_pass_row(plan.passes, std::string(row.name), row.kind,
                          available_queue(row.queue, request.environment), source, flags, frame_area);
        }

        // --- Upload pass access events: the arena as source, buffers as targets ---
        const auto pass_offset = request.inject_stable_upload_pass ? 1U : 0U;
        if (request.inject_stable_upload_pass)
        {
            push_buffer_access(state,
                               pass_handle{0},
                               static_cast<resource_handle>(plan.upload_buffer),
                               access_type::read,
                               buffer_usage::TRANSFER_SRC,
                               pipeline_domain::copy,
                               plan.passes.queues[0],
                               {});
            for (uint32_t index = 0; index < frame.resources.size(); ++index)
            {
                if (frame.resources[index].source != frame_resource_source::persistent_buffer || plan.frame_buffers[index] == invalid_buffer)
                    continue;
                const auto logical = plan.frame_buffers[index];
                const auto& desc   = plan.resources.buffer_metas.descs[logical];
                if ((desc.usage & buffer_usage::TRANSFER_DST) == buffer_usage::NONE)
                    continue;
                push_buffer_access(state,
                                   pass_handle{0},
                                   static_cast<resource_handle>(logical),
                                   access_type::write,
                                   buffer_usage::TRANSFER_DST,
                                   pipeline_domain::copy,
                                   plan.passes.queues[0],
                                   {});
            }
        }

        // --- Access events: buffer reads, image reads, raster attachments ---
        for (uint32_t source = 0; source < frame.passes.size(); ++source)
        {
            const auto pass        = pass_handle{source + pass_offset};
            const auto& row        = frame.passes[source];
            const auto pass_domain = domain(row.kind);
            const auto pass_queue  = plan.passes.queues[pass];
            for (uint32_t at = row.buffer_accesses.begin; at < row.buffer_accesses.begin + row.buffer_accesses.count; ++at)
            {
                const auto& access = frame.buffer_accesses[at];
                if (access.resource.index >= frame.resources.size() || plan.frame_buffers[access.resource.index] == invalid_buffer)
                {
                    fail(state,
                         compile_error_code::buffer_read_out_of_range,
                         "buffer access references a non-buffer resource",
                         pass,
                         resource_kind::buffer,
                         access.resource.index);
                    continue;
                }
                push_buffer_access(state,
                                   pass,
                                   static_cast<resource_handle>(plan.frame_buffers[access.resource.index]),
                                   access.access,
                                   access.usage,
                                   pass_domain,
                                   pass_queue,
                                   access.range);
            }
            for (uint32_t at = row.image_accesses.begin; at < row.image_accesses.begin + row.image_accesses.count; ++at)
            {
                const auto& access = frame.image_accesses[at];
                if (access.resource.index >= frame.resources.size() || plan.frame_images[access.resource.index] == invalid_image)
                {
                    fail(state,
                         compile_error_code::image_read_out_of_range,
                         "image access references a non-image resource",
                         pass,
                         resource_kind::image,
                         access.resource.index);
                    continue;
                }
                push_image_access(state,
                                  pass,
                                  static_cast<resource_handle>(plan.frame_images[access.resource.index]),
                                  access.access,
                                  access.usage,
                                  pass_domain,
                                  pass_queue,
                                  access.range);
            }
            for (uint32_t at = row.attachments.begin; at < row.attachments.begin + row.attachments.count; ++at)
            {
                const auto& attachment = frame.attachments[at];
                if (attachment.resource.index >= frame.resources.size() || plan.frame_images[attachment.resource.index] == invalid_image)
                {
                    fail(state,
                         compile_error_code::raster_attachment_mismatch,
                         "attachment references a non-image resource",
                         pass,
                         resource_kind::image,
                         attachment.resource.index);
                    continue;
                }
                const auto logical = plan.frame_images[attachment.resource.index];
                const auto usage =
                    attachment.kind == frame_attachment_kind::color ? image_usage::COLOR_ATTACHMENT : image_usage::DEPTH_STENCIL_ATTACHMENT;
                push_image_access(state,
                                  pass,
                                  static_cast<resource_handle>(logical),
                                  attachment.load == attachment_load_op::load ? access_type::read_write : access_type::write,
                                  usage,
                                  pipeline_domain::graphics,
                                  pass_queue,
                                  {});
                auto raster_attachment_row = raster_attachment{
                    .image = logical,
                    .load  = attachment.load,
                    .store = attachment.store,
                    .clear = attachment.clear,
                };
                if (attachment.kind == frame_attachment_kind::color)
                    plan.passes.colors.push_back(raster_attachment_row);
                else
                {
                    plan.passes.depth_indices[pass] = static_cast<uint32_t>(plan.passes.depths.size());
                    plan.passes.depths.push_back(raster_attachment_row);
                }
            }
            // Finalize the color CSR for this pass, then check for empty raster passes.
            plan.passes.color_counts[pass] =
                static_cast<uint32_t>(plan.passes.colors.size() - plan.passes.color_begins[pass]);
            if (row.kind == pass_kind::raster && plan.passes.color_counts[pass] == 0 &&
                plan.passes.depth_indices[pass] == invalid_depth_index)
                fail(state, compile_error_code::raster_pass_has_no_attachments, "raster pass has no attachments", pass);
        }

        // --- Swapchain contract: PRESENT before the frame, PRESENT after ---
        for (uint32_t index = 0; index < frame.resources.size(); ++index)
        {
            if (plan.frame_images[index] != invalid_image &&
                frame.resources[index].source == frame_resource_source::swapchain_image)
            {
                const auto row = static_cast<uint32_t>(state.image_contracts.size());
                state.image_contract_indices[plan.frame_images[index]] = row;
                state.image_contracts.push_back({
                    .initial_state    = {.usage  = request.environment.swapchain_initialized ? image_usage::PRESENT : image_usage::NONE,
                                         .domain = pipeline_domain::graphics},
                    .initial_access   = access_type::read,
                    .initial_contents = request.environment.swapchain_initialized ? contents_policy::preserve : contents_policy::discard,
                    .final_state      = {.usage = image_usage::PRESENT, .domain = pipeline_domain::graphics},
                });
            }
        }

        // --- Sort and merge the collected event rows, then build the per-pass CSR ---
        sort_and_merge_rows(state.image_events);
        sort_and_merge_rows(state.buffer_events);
        const auto pass_count = static_cast<uint32_t>(plan.passes.size());
        build_event_begins(state.image_events, pass_count);
        build_event_begins(state.buffer_events, pass_count);
        return state.output.result.succeeded();
    }

    // =========================================================================
    // Stage 3: dependency DAG construction
    // =========================================================================
    bool build_dependency_dag(compiler_state& state)
    {
        core::build_dependency_dag(state.image_events, state.buffer_events,
                                   static_cast<uint32_t>(state.output.plan.passes.size()), state.dag);
        return true;
    }

    // =========================================================================
    // Stage 4: pass culling (reverse DFS from roots)
    // =========================================================================
    bool cull_passes(compiler_state& state)
    {
        const auto pass_count = static_cast<uint32_t>(state.output.plan.passes.size());
        std::vector<uint8_t> marked(pass_count, 0U);

        // --- 1. Collect root passes ---
        const auto& passes = state.output.plan.passes;
        for (uint32_t p = 0; p < pass_count; ++p)
        {
            // (a) backend upload pass is always a root
            if (passes.is_backend_upload(p)) { marked[p] = 1U; continue; }
            // (b) explicit side_effect marker
            if (passes.is_side_effect(p)) { marked[p] = 1U; continue; }
        }
        // (c) writes to swapchain / imported resources: scan both event tables.
        const auto mark_imported_writers = [&](const auto& events, const auto& metas)
        {
            for (std::size_t e = 0; e < events.passes.size(); ++e)
            {
                if (events.accesses[e] == access_type::read) continue;
                if (metas.is_imported[events.logicals[e]])
                    marked[events.passes[e]] = 1U;
            }
        };
        mark_imported_writers(state.image_events, state.output.plan.resources.image_metas);
        mark_imported_writers(state.buffer_events, state.output.plan.resources.buffer_metas);

        // --- 2. Direct-indexed last-writer arrays (per logical resource) ---
        const auto image_count = state.output.plan.resources.image_metas.descs.size();
        const auto buffer_count = state.output.plan.resources.buffer_metas.descs.size();
        std::vector<pass_handle> image_producers(image_count, invalid_pass);
        std::vector<pass_handle> buffer_producers(buffer_count, invalid_pass);

        for (std::size_t e = 0; e < state.image_events.passes.size(); ++e)
            if (state.image_events.accesses[e] != access_type::read)
                image_producers[state.image_events.logicals[e]] = state.image_events.passes[e];
        for (std::size_t e = 0; e < state.buffer_events.passes.size(); ++e)
            if (state.buffer_events.accesses[e] != access_type::read)
                buffer_producers[state.buffer_events.logicals[e]] = state.buffer_events.passes[e];

        // --- 3. Reverse traversal along reads (flat-vector stack DFS) ---
        std::vector<pass_handle> worklist;
        for (uint32_t p = 0; p < pass_count; ++p)
            if (marked[p] != 0U) worklist.push_back(p);

        const auto enqueue_read_producers = [&](const auto& events, const auto& producers, pass_handle pass)
        {
            const auto begin = events.event_begins[pass];
            const auto end   = events.event_begins[pass + 1];
            for (uint32_t e = begin; e < end; ++e)
            {
                if (events.accesses[e] == access_type::write) continue;
                const auto producer = producers[events.logicals[e]];
                if (producer == invalid_pass || marked[producer] != 0U) continue;
                marked[producer] = 1U;
                worklist.push_back(producer);
            }
        };

        while (!worklist.empty())
        {
            const auto pass = worklist.back();
            worklist.pop_back();
            // For every resource this pass *reads*, enqueue its producer
            enqueue_read_producers(state.image_events, image_producers, pass);
            enqueue_read_producers(state.buffer_events, buffer_producers, pass);
        }

        // --- 4. Record the active-pass compaction map (consumed by compact_passes) ---
        state.pass_old_to_new.assign(pass_count, 0U);
        state.active_pass_list.clear();
        for (uint32_t p = 0; p < pass_count; ++p)
            if (marked[p] != 0U)
            {
                state.pass_old_to_new[p] = static_cast<uint32_t>(state.active_pass_list.size());
                state.active_pass_list.push_back(p);
            }
        state.culled_pass_count = pass_count - static_cast<uint32_t>(state.active_pass_list.size());
        return true;
    }

    // =========================================================================
    // Stage 4.5: physical pass-table compaction
    // =========================================================================
    bool compact_passes(compiler_state& state)
    {
        // Nothing was culled: the pass table is already index-aligned, so the
        // whole gather/remap pass would be an expensive no-op. Skip it.
        if (state.culled_pass_count == 0)
            return true;

        auto& plan   = state.output.plan;
        auto& passes = plan.passes;
        const auto& list = state.active_pass_list;

        // --- Gather surviving pass rows into place. The list is ascending, so
        // reading from later rows while writing earlier slots never clobbers. ---
        for (std::size_t write = 0; write < list.size(); ++write)
        {
            const auto read = list[write];
            passes.names[write]         = std::move(passes.names[read]);
            passes.name_hashes[write]   = passes.name_hashes[read];
            passes.kinds[write]         = passes.kinds[read];
            passes.queues[write]        = passes.queues[read];
            passes.source_passes[write] = passes.source_passes[read];
            passes.flags[write]         = passes.flags[read];
            passes.areas[write]         = passes.areas[read];
            passes.layer_counts[write]  = passes.layer_counts[read];
        }

        // --- Raster CSR: move surviving attachment spans into place ---
        std::vector<raster_attachment> compacted_colors;
        compacted_colors.reserve(passes.colors.size());
        std::vector<raster_attachment> compacted_depths;
        compacted_depths.reserve(passes.depths.size());
        for (std::size_t write = 0; write < list.size(); ++write)
        {
            const auto read  = list[write];
            const auto begin = passes.color_begins[read];
            const auto count = passes.color_counts[read];
            passes.color_begins[write] = static_cast<uint32_t>(compacted_colors.size());
            passes.color_counts[write] = count;
            compacted_colors.insert(compacted_colors.end(), passes.colors.begin() + begin,
                                    passes.colors.begin() + begin + count);
            const auto depth_index = passes.depth_indices[read];
            if (depth_index == invalid_depth_index)
                passes.depth_indices[write] = invalid_depth_index;
            else
            {
                passes.depth_indices[write] = static_cast<uint32_t>(compacted_depths.size());
                compacted_depths.push_back(passes.depths[depth_index]);
            }
        }

        passes.names.resize(list.size());
        passes.name_hashes.resize(list.size());
        passes.kinds.resize(list.size());
        passes.queues.resize(list.size());
        passes.source_passes.resize(list.size());
        passes.flags.resize(list.size());
        passes.areas.resize(list.size());
        passes.layer_counts.resize(list.size());
        passes.color_begins.resize(list.size() + 1);
        passes.color_counts.resize(list.size());
        passes.depth_indices.resize(list.size());
        passes.colors = std::move(compacted_colors);
        passes.depths = std::move(compacted_depths);

        // --- Event tables: shrink to surviving rows, remap pass handles, and
        // rebuild the per-pass CSR in the compacted space ---
        const auto old_pass_count = static_cast<uint32_t>(state.pass_old_to_new.size());
        std::vector<uint8_t> mask(old_pass_count, 0U);
        for (const auto pass : list)
            mask[pass] = 1U;
        filter_active_rows(state.image_events, mask, old_pass_count);
        filter_active_rows(state.buffer_events, mask, old_pass_count);
        const auto new_pass_count = static_cast<uint32_t>(list.size());
        for (auto& pass : state.image_events.passes)
            pass = pass_handle{state.pass_old_to_new[pass]};
        for (auto& pass : state.buffer_events.passes)
            pass = pass_handle{state.pass_old_to_new[pass]};
        build_event_begins(state.image_events, new_pass_count);
        build_event_begins(state.buffer_events, new_pass_count);

        // --- Dependency DAG: remap both CSR directions into the compacted space ---
        core::remap_dependency_graph(state.dag, state.pass_old_to_new, new_pass_count);

        // Culling intermediates are consumed; release them.
        state.active_pass_list.clear();
        state.pass_old_to_new.clear();
        return true;
    }

    // =========================================================================
    // Stage 5: pass scheduling (topological order over the compacted graph)
    // =========================================================================
    bool schedule_passes(compiler_state& state)
    {
        if (!core::schedule_passes(state.dag, state.output.plan.scheduled_passes))
        {
            fail(state, compile_error_code::cycle_detected, "render graph contains a dependency cycle");
            return false;
        }
        return true;
    }

    // =========================================================================
    // Stage 6: lifetime analysis and aliasing
    // =========================================================================
    bool compile_lifetimes(compiler_state& state)
    {
        auto& plan              = state.output.plan;
        const auto image_count  = plan.resources.image_metas.descs.size();
        const auto buffer_count = plan.resources.buffer_metas.descs.size();
        plan.lifetimes.image_first_used_pass.assign(image_count, invalid_pass);
        plan.lifetimes.image_last_used_pass.assign(image_count, invalid_pass);
        plan.lifetimes.buffer_first_used_pass.assign(buffer_count, invalid_pass);
        plan.lifetimes.buffer_last_used_pass.assign(buffer_count, invalid_pass);
        // --- First and last used pass, folded into scheduled order ---
        std::vector<uint32_t> order(plan.passes.size());
        for (uint32_t index = 0; index < plan.scheduled_passes.size(); ++index)
            order[plan.scheduled_passes[index]] = index;
        std::vector<uint32_t> image_first_order(image_count, std::numeric_limits<uint32_t>::max());
        std::vector<uint32_t> image_last_order(image_count, 0);
        std::vector<uint32_t> buffer_first_order(buffer_count, std::numeric_limits<uint32_t>::max());
        std::vector<uint32_t> buffer_last_order(buffer_count, 0);
        const auto fold_lifetimes = [&](const auto& events, auto& first, auto& last, auto& first_order, auto& last_order)
        {
            for (std::size_t e = 0; e < events.passes.size(); ++e)
            {
                const auto at      = order[events.passes[e]];
                const auto logical = events.logicals[e];
                if (at < first_order[logical])
                {
                    first_order[logical] = at;
                    first[logical]       = events.passes[e];
                }
                if (last[logical] == invalid_pass || at > last_order[logical])
                {
                    last_order[logical] = at;
                    last[logical]       = events.passes[e];
                }
            }
        };
        fold_lifetimes(state.image_events, plan.lifetimes.image_first_used_pass, plan.lifetimes.image_last_used_pass,
                       image_first_order, image_last_order);
        fold_lifetimes(state.buffer_events, plan.lifetimes.buffer_first_used_pass, plan.lifetimes.buffer_last_used_pass,
                       buffer_first_order, buffer_last_order);

        // --- Physical resources: reuse dead transient memory via alias handoffs.
        // Candidates are bucketed by desc hash (full-field equality is
        // re-verified inside the bucket — the hash is only a pre-filter); the
        // first declaration-earlier candidate with a disjoint lifetime wins,
        // preserving the original first-fit semantics. One shared pass serves
        // images and buffers.
        auto& physical = plan.physical_resources;
        const auto build_aliasing = [&](const auto& metas, const auto& first_order, const auto& last_order,
                                        auto& handle_to_physical, auto& handle_to_memory_block, auto& physical_meta,
                                        auto& memory_blocks, resource_kind kind, auto requirements_of)
        {
            const auto count = static_cast<resource_handle>(metas.descs.size());
            handle_to_physical.assign(count, invalid_resource);
            handle_to_memory_block.assign(count, invalid_resource);

            // Hash-bucket the logical rows; equal hashes keep declaration order.
            std::vector<resource_handle> by_hash(count);
            std::iota(by_hash.begin(), by_hash.end(), 0U);
            std::stable_sort(by_hash.begin(), by_hash.end(), [&](resource_handle a, resource_handle b)
            {
                return metas.desc_hashes[a] < metas.desc_hashes[b];
            });

            constexpr uint32_t never = std::numeric_limits<uint32_t>::max();
            for (resource_handle logical = 0; logical < count; ++logical)
            {
                const auto& desc = metas.descs[logical];
                resource_handle reuse = invalid_resource;
                if (desc.lifetime == resource_lifetime_class::transient && desc.aliasing != aliasing_policy::forbidden &&
                    first_order[logical] != never)
                {
                    const auto hash_value = metas.desc_hashes[logical];
                    const auto bucket     = std::lower_bound(by_hash.begin(), by_hash.end(), hash_value,
                                                    [&](resource_handle row, uint64_t hash) { return metas.desc_hashes[row] < hash; });
                    const auto bucket_end = std::upper_bound(bucket, by_hash.end(), hash_value,
                                                    [&](uint64_t hash, resource_handle row) { return hash < metas.desc_hashes[row]; });
                    for (auto it = bucket; it != bucket_end; ++it)
                    {
                        const auto candidate = *it;
                        if (candidate >= logical) break; // bucket keeps declaration order
                        if (metas.descs[candidate] != desc) continue;
                        if (first_order[candidate] != never && last_order[candidate] < first_order[logical])
                        {
                            reuse = candidate;
                            break;
                        }
                    }
                }
                if (reuse != invalid_resource)
                {
                    handle_to_physical[logical]    = handle_to_physical[reuse];
                    handle_to_memory_block[logical] = handle_to_memory_block[reuse];
                    physical.alias_handoffs.push_back({.kind         = kind,
                                                       .previous     = reuse,
                                                       .next         = logical,
                                                       .memory_block = handle_to_memory_block[reuse],
                                                       .at_pass      = kind == resource_kind::image
                                                                           ? plan.lifetimes.image_first_used_pass[logical]
                                                                           : plan.lifetimes.buffer_first_used_pass[logical]});
                }
                else
                {
                    // Culled transient resources have no lifecycle — skip physical allocation.
                    if (desc.lifetime == resource_lifetime_class::transient && first_order[logical] == never)
                        continue;
                    handle_to_physical[logical] = static_cast<resource_handle>(physical_meta.size());
                    physical_meta.push_back(logical);
                    if (!metas.is_imported[logical])
                    {
                        handle_to_memory_block[logical] = static_cast<resource_handle>(memory_blocks.size());
                        memory_blocks.push_back(requirements_of(desc));
                    }
                }
            }
        };
        build_aliasing(plan.resources.image_metas, image_first_order, image_last_order,
                       physical.handle_to_physical_img_id, physical.handle_to_image_memory_block,
                       physical.physical_image_meta, physical.image_memory_blocks, resource_kind::image,
                       [&](const image_desc& desc)
                       {
                           return (state.request->allocations.image_requirements != nullptr)
                                      ? state.request->allocations.image_requirements(state.request->allocations.state, desc)
                                      : default_image_requirements(desc);
                       });
        build_aliasing(plan.resources.buffer_metas, buffer_first_order, buffer_last_order,
                       physical.handle_to_physical_buf_id, physical.handle_to_buffer_memory_block,
                       physical.physical_buffer_meta, physical.buffer_memory_blocks, resource_kind::buffer,
                       [&](const buffer_desc& desc)
                       {
                           return (state.request->allocations.buffer_requirements != nullptr)
                                      ? state.request->allocations.buffer_requirements(state.request->allocations.state, desc)
                                      : default_buffer_requirements(desc);
                       });
        return true;
    }

    // =========================================================================
    // Stage 7: synchronization generation
    // =========================================================================
    bool compile_synchronization(compiler_state& state)
    {
        auto& plan = state.output.plan;
        std::vector<std::vector<synchronization_op>> prologues(plan.passes.size());
        std::vector<synchronization_op> epilogue;
        struct last_state
        {
            bool valid = false;
            abstract_resource_state state{};
            pass_handle pass = invalid_pass;
        };
        std::vector<last_state> images(plan.resources.image_metas.descs.size());
        std::vector<last_state> buffers(plan.resources.buffer_metas.descs.size());

        for (const auto pass : plan.scheduled_passes)
        {
            // Emit prologue ops for one event table's rows of this pass
            // (kind is fixed per table — no runtime dispatch).
            const auto emit_events = [&](const auto& events, auto& previous_states, const auto& contract_indices,
                                         const auto& contracts, const auto& handle_to_physical,
                                         const auto& handle_to_block, resource_kind kind)
            {
                for (uint32_t e = events.event_begins[pass]; e < events.event_begins[pass + 1]; ++e)
                {
                    auto& previous   = previous_states[events.logicals[e]];
                    const auto after = abstract_state(events, e);
                    abstract_resource_state before{};
                    bool have_before = previous.valid;
                    if (previous.valid)
                        before = previous.state;
                    else
                    {
                        const auto row = contract_indices[events.logicals[e]];
                        if (row != invalid_contract_index)
                        {
                            const auto& contract = contracts[row];
                            before.access     = contract.initial_access;
                            before.usage_bits = static_cast<uint32_t>(contract.initial_state.usage);
                            before.domain     = contract.initial_state.domain;
                            before.queue      = contract.initial_state.queue;
                            if constexpr (std::is_same_v<std::decay_t<decltype(events)>, image_access_rows>)
                                before.image_range = contract.initial_state.subresource;
                            else
                                before.buffer_range = contract.initial_state.bytes;
                            have_before = true;
                        }
                    }
                    if (!have_before)
                    {
                        before            = after;
                        before.usage_bits = 0;
                        before.access     = access_type::read;
                    }
                    const auto intents = transition_intents(before, after, kind);
                    if (intents != synchronization_intent::none)
                    {
                        prologues[pass].push_back({
                            .scope        = synchronization_scope::pass_prologue,
                            .phase        = before.queue == after.queue ? synchronization_phase::full : synchronization_phase::acquire,
                            .intents      = intents,
                            .kind         = kind,
                            .logical      = events.logicals[e],
                            .physical     = handle_to_physical[events.logicals[e]],
                            .memory_block = handle_to_block[events.logicals[e]],
                            .pass         = pass,
                            .source_pass  = previous.pass,
                            .before       = before,
                            .after        = after,
                        });
                    }
                    previous = {.valid = true, .state = after, .pass = pass};
                }
            };

            emit_events(state.image_events, images, state.image_contract_indices, state.image_contracts,
                        plan.physical_resources.handle_to_physical_img_id,
                        plan.physical_resources.handle_to_image_memory_block, resource_kind::image);
            emit_events(state.buffer_events, buffers, state.buffer_contract_indices, state.buffer_contracts,
                        plan.physical_resources.handle_to_physical_buf_id,
                        plan.physical_resources.handle_to_buffer_memory_block, resource_kind::buffer);
        }
        // --- Alias handoffs: barrier between the previous and next user ---
        const auto first_state_after = [&](const auto& events, resource_handle logical)
        {
            for (std::size_t e = 0; e < events.passes.size(); ++e)
                if (events.logicals[e] == logical)
                    return abstract_state(events, e);
            return abstract_resource_state{};
        };
        for (const auto& handoff : plan.physical_resources.alias_handoffs)
        {
            auto after = handoff.kind == resource_kind::image
                             ? first_state_after(state.image_events, handoff.next)
                             : first_state_after(state.buffer_events, handoff.next);
            prologues[handoff.at_pass].push_back({
                .scope            = synchronization_scope::pass_prologue,
                .intents          = synchronization_intent::aliasing | synchronization_intent::memory_dependency,
                .kind             = handoff.kind,
                .logical          = handoff.next,
                .physical         = handoff.kind == resource_kind::image ? plan.physical_resources.handle_to_physical_img_id[handoff.next]
                                                                         : plan.physical_resources.handle_to_physical_buf_id[handoff.next],
                .memory_block     = handoff.memory_block,
                .previous_logical = handoff.previous,
                .pass             = handoff.at_pass,
                .after            = after,
            });
        }
        // --- Graph epilogue: transition to each final contract state ---
        for (resource_handle logical = 0; logical < images.size(); ++logical)
        {
            const auto row = state.image_contract_indices[logical];
            if (row == invalid_contract_index || !images[logical].valid)
                continue;
            const auto& contract = state.image_contracts[row];
            abstract_resource_state after{.access = contract.final_access};
            after.usage_bits  = static_cast<uint32_t>(contract.final_state.usage);
            after.domain      = contract.final_state.domain;
            after.queue       = contract.final_state.queue;
            after.image_range = contract.final_state.subresource;
            const auto intents = transition_intents(images[logical].state, after, resource_kind::image);
            if (intents == synchronization_intent::none)
                continue;
            epilogue.push_back({
                .scope        = synchronization_scope::graph_epilogue,
                .intents      = intents,
                .kind         = resource_kind::image,
                .logical      = logical,
                .physical     = plan.physical_resources.handle_to_physical_img_id[logical],
                .memory_block = plan.physical_resources.handle_to_image_memory_block[logical],
                .source_pass  = images[logical].pass,
                .before       = images[logical].state,
                .after        = after,
            });
        }
        // --- Pack prologues and epilogue into the flat op stream ---
        auto& sync = plan.synchronization;
        sync.prologue_begins.resize(plan.passes.size());
        sync.prologue_lengths.resize(plan.passes.size());
        sync.internal_begins.assign(plan.passes.size(), 0);
        sync.internal_lengths.assign(plan.passes.size(), 0);
        for (uint32_t pass = 0; pass < prologues.size(); ++pass)
        {
            sync.prologue_begins[pass]  = static_cast<uint32_t>(sync.ops.size());
            sync.prologue_lengths[pass] = static_cast<uint32_t>(prologues[pass].size());
            sync.ops.insert(sync.ops.end(), prologues[pass].begin(), prologues[pass].end());
        }
        sync.epilogue_begin  = static_cast<uint32_t>(sync.ops.size());
        sync.epilogue_length = static_cast<uint32_t>(epilogue.size());
        sync.ops.insert(sync.ops.end(), epilogue.begin(), epilogue.end());
        return true;
    }

    // =========================================================================
    // Stage 8: submission batching
    // =========================================================================
    bool compile_submissions(compiler_state& state)
    {
        auto& plan        = state.output.plan;
        auto& submissions = plan.submissions;
        submissions.pass_to_batch.assign(plan.passes.size(), invalid_submission_batch);

        // --- Group consecutive passes on the same queue into batches ---
        for (const auto pass : plan.scheduled_passes)
        {
            const auto queue = plan.passes.queues[pass];
            if (submissions.batches.empty() || submissions.batches.back().queue != queue)
            {
                const auto handle = submission_batch_handle{static_cast<uint32_t>(submissions.batches.size())};
                submissions.batches.push_back({.handle = handle, .queue = queue, .signal_value = static_cast<uint64_t>(handle.value) + 1});
            }
            auto& batch = submissions.batches.back();
            batch.passes.push_back(pass);
            submissions.pass_to_batch[pass] = batch.handle;
        }
        if (!submissions.batches.empty())
        {
            submissions.batches.front().waits_for_external_acquire = true;
            submissions.batches.back().signals_external_present    = true;
        }
        // --- Timeline waits: one per cross-batch DAG edge ---
        for (pass_handle source = 0; source < plan.passes.size(); ++source)
        {
            const auto begin = state.dag.adjacency_begins[source];
            const auto end   = state.dag.adjacency_begins[source + 1];
            for (uint32_t index = begin; index < end; ++index)
            {
                const auto destination = state.dag.adjacency_list[index];
                const auto source_batch      = submissions.pass_to_batch[source];
                const auto destination_batch = submissions.pass_to_batch[destination];
                if (source_batch == destination_batch)
                    continue;
                auto& batch = submissions.batches[destination_batch];
                if (std::ranges::none_of(batch.waits, [&](const timeline_wait& wait) { return wait.source_batch == source_batch; }))
                    batch.waits.push_back({.source_batch = source_batch,
                                           .source_queue = submissions.batches[source_batch].queue,
                                           .value        = submissions.batches[source_batch].signal_value});
            }
        }
        // --- Cross-queue ownership: split barriers into release + acquire ---
        for (const auto& op : plan.synchronization.ops)
        {
            if (!has_intent(op.intents, synchronization_intent::queue_ownership) || op.source_pass == invalid_pass || op.pass == invalid_pass)
                continue;
            const auto source_batch      = submissions.pass_to_batch[op.source_pass];
            const auto destination_batch = submissions.pass_to_batch[op.pass];
            if (source_batch == destination_batch)
                continue;
            auto release  = op;
            release.phase = synchronization_phase::release;
            release.pass  = op.source_pass;
            submissions.batches[source_batch].release_barriers.push_back(release);
            auto acquire  = op;
            acquire.phase = synchronization_phase::acquire;
            submissions.batches[destination_batch].acquire_barriers.push_back(acquire);
            submissions.cross_queue_dependencies.push_back({
                .source_batch       = source_batch,
                .destination_batch  = destination_batch,
                .source_pass        = op.source_pass,
                .destination_pass   = op.pass,
                .source_queue       = submissions.batches[source_batch].queue,
                .destination_queue  = submissions.batches[destination_batch].queue,
                .kind               = op.kind,
                .logical            = op.logical,
                .ownership_transfer = true,
            });
        }
        return true;
    }

    // =========================================================================
    // Stage 9: plan publishing
    // =========================================================================
    void publish_compiled_plan(compiler_state& state)
    {
        auto& plan    = state.output.plan;

        // --- Cache key: environment, resources, passes, access states ---
        uint64_t hash = state.request->frame->cache_key;
        hash = combine(hash, (static_cast<uint64_t>(state.request->environment.extent.width) << 32) | state.request->environment.extent.height);
        hash = combine(hash, static_cast<uint64_t>(state.request->environment.color_format));
        hash = combine(hash, static_cast<uint64_t>(state.request->environment.swapchain_initialized));
        for (const auto& desc : plan.resources.image_metas.descs)
            hash = combine(hash, hash_resource_desc(desc));
        for (const auto& desc : plan.resources.buffer_metas.descs)
            hash = combine(hash, hash_resource_desc(desc));
        const auto hash_attachment = [&](const raster_attachment& attachment)
        {
            hash = combine(hash, attachment.image.value);
            hash = combine(hash, static_cast<uint64_t>(attachment.load));
            hash = combine(hash, static_cast<uint64_t>(attachment.store));
            hash = combine(hash, static_cast<uint64_t>(attachment.subresource.aspects));
            hash = combine(hash, attachment.subresource.base_mip_level);
            hash = combine(hash, attachment.subresource.mip_level_count);
            hash = combine(hash, attachment.subresource.base_array_layer);
            hash = combine(hash, attachment.subresource.array_layer_count);
        };
        for (pass_handle pass = 0; pass < plan.passes.size(); ++pass)
        {
            hash = combine(hash, static_cast<uint64_t>(plan.passes.kinds[pass]));
            hash = combine(hash, static_cast<uint64_t>(plan.passes.queues[pass]));
            for (const auto value : plan.passes.names[pass])
                hash = combine(hash, static_cast<uint8_t>(value));
            hash = combine(hash, plan.passes.layer_counts[pass]);
            const auto depth_index = plan.passes.depth_indices[pass];
            hash = combine(hash, static_cast<uint64_t>(depth_index != invalid_depth_index));
            const auto begin = plan.passes.color_begins[pass];
            for (uint32_t index = begin; index < begin + plan.passes.color_counts[pass]; ++index)
                hash_attachment(plan.passes.colors[index]);
            if (depth_index != invalid_depth_index)
                hash_attachment(plan.passes.depths[depth_index]);
        }
        const auto hash_events = [&](const auto& events, resource_kind kind)
        {
            for (std::size_t e = 0; e < events.passes.size(); ++e)
            {
                hash            = combine(hash, events.passes[e].value);
                hash            = combine(hash, events.logicals[e]);
                hash            = combine(hash, static_cast<uint64_t>(kind));
                hash            = combine(hash, static_cast<uint64_t>(events.accesses[e]));
                const auto state = abstract_state(events, e);
                hash            = combine(hash, state.usage_bits);
                hash            = combine(hash, static_cast<uint64_t>(state.domain));
                hash            = combine(hash, static_cast<uint64_t>(state.queue));
                if constexpr (std::is_same_v<std::decay_t<decltype(events)>, image_access_rows>)
                {
                    hash = combine(hash, static_cast<uint64_t>(state.image_range.aspects));
                    hash = combine(hash, state.image_range.base_mip_level);
                    hash = combine(hash, state.image_range.mip_level_count);
                    hash = combine(hash, state.image_range.base_array_layer);
                    hash = combine(hash, state.image_range.array_layer_count);
                }
                else
                {
                    hash = combine(hash, state.buffer_range.offset);
                    hash = combine(hash, state.buffer_range.size);
                }
            }
        };
        hash_events(state.image_events, resource_kind::image);
        hash_events(state.buffer_events, resource_kind::buffer);
        plan.cache_key  = hash;

        // --- Statistics: counts for one frame of the compiled plan ---
        plan.statistics = {
            .pass_count                = static_cast<uint32_t>(plan.passes.size()),
            .active_pass_count         = static_cast<uint32_t>(plan.scheduled_passes.size()),
            .image_count               = static_cast<uint32_t>(plan.resources.image_metas.descs.size()),
            .buffer_count              = static_cast<uint32_t>(plan.resources.buffer_metas.descs.size()),
            .access_event_count        = static_cast<uint32_t>(state.image_events.passes.size() + state.buffer_events.passes.size()),
            .synchronization_op_count  = static_cast<uint32_t>(plan.synchronization.ops.size()),
            .submission_batch_count    = static_cast<uint32_t>(plan.submissions.batches.size()),
            .image_memory_block_count  = static_cast<uint32_t>(plan.physical_resources.image_memory_blocks.size()),
            .buffer_memory_block_count = static_cast<uint32_t>(plan.physical_resources.buffer_memory_blocks.size()),
            .culled_pass_count         = state.culled_pass_count,
        };
    }
} // namespace render_graph::core

namespace render_graph
{
    // =========================================================================
    // Compile driver: runs the compiler stages in order
    // =========================================================================
    graph_compile_output compile_graph(const graph_compile_request& request)
    {
        core::compiler_state state{.request = &request};
        // Every stage reports its own diagnostics; stop at the first failure.
        if (!core::validate_recipe(state))
            return std::move(state.output);
        if (!core::build_resource_versions(state))
            return std::move(state.output);
        if (!core::build_dependency_dag(state))
            return std::move(state.output);
        if (!core::cull_passes(state))
            return std::move(state.output);
        if (!core::compact_passes(state))
            return std::move(state.output);
        if (!core::schedule_passes(state))
            return std::move(state.output);
        if (!core::compile_lifetimes(state))
            return std::move(state.output);
        if (!core::compile_synchronization(state))
            return std::move(state.output);
        if (!core::compile_submissions(state))
            return std::move(state.output);
        core::publish_compiled_plan(state);
        return std::move(state.output);
    }
} // namespace render_graph
