// Core graph compiler: lifts a frame recipe into plan rows, builds the
// dependency DAG, schedules the passes, computes lifetimes and per-resource
// synchronization, then packs the submissions and publishes the compiled
// plan for the backends to record.
#include "system.h"

#include <algorithm>
#include <array>
#include <limits>
#include <queue>
#include <unordered_map>

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
                diagnostic.pass_name = state.output.plan.passes[pass].name;
            state.output.result.diagnostics.push_back(std::move(diagnostic));
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

        // Merge a new access into an existing event for the same (pass, resource).
        void append_access(compiler_state& state, access_event event)
        {
            for (auto& existing : state.accesses)
            {
                if (existing.pass != event.pass || existing.kind != event.kind || existing.logical != event.logical)
                    continue;
                existing.access = merge_access(existing.access, event.access);
                if (event.kind == resource_kind::buffer)
                {
                    auto& before      = std::get<buffer_access_desc>(existing.state);
                    const auto& after = std::get<buffer_access_desc>(event.state);
                    before.usage      = before.usage | after.usage;
                    if (before.bytes != after.bytes)
                        before.bytes = {};
                }
                else
                {
                    auto& before      = std::get<image_access_desc>(existing.state);
                    const auto& after = std::get<image_access_desc>(event.state);
                    before.usage      = before.usage | after.usage;
                    if (before.subresource != after.subresource)
                        before.subresource = {};
                }
                return;
            }
            state.accesses.push_back(std::move(event));
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

        abstract_resource_state abstract_state(const access_event& event)
        {
            abstract_resource_state output{.access = event.access};
            if (event.kind == resource_kind::image)
            {
                const auto& value  = std::get<image_access_desc>(event.state);
                output.usage_bits  = static_cast<uint32_t>(value.usage);
                output.domain      = value.domain;
                output.queue       = value.queue;
                output.image_range = value.subresource;
            }
            else
            {
                const auto& value   = std::get<buffer_access_desc>(event.state);
                output.usage_bits   = static_cast<uint32_t>(value.usage);
                output.domain       = value.domain;
                output.queue        = value.queue;
                output.buffer_range = value.bytes;
            }
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
            plan.upload_buffer =
                plan.resources.buffer_metas.add("UploadArena", desc, resource_lifetime_class::imported, hash_resource_desc(desc), true);
            state.buffer_contracts.emplace_back();
            plan.passes.push_back({.name           = "UploadPass",
                                   .kind           = pass_kind::copy,
                                   .queue          = available_queue(queue_class::copy, request.environment),
                                   .source_pass    = invalid_source_pass,
                                   .backend_upload = true});
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
                state.buffer_contracts.emplace_back();
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
                state.image_contracts.emplace_back();
            }
        }

        // --- Pass rows: one compiled row per frame pass ---
        for (uint32_t source = 0; source < frame.passes.size(); ++source)
        {
            const auto& row = frame.passes[source];
            compiled_pass_row pass{
                .name        = std::string(row.name),
                .kind        = row.kind,
                .queue       = available_queue(row.queue, request.environment),
                .source_pass = source,
                .active      = true,
                .side_effect = row.side_effect,
            };
            pass.raster.area = {.width = request.environment.extent.width, .height = request.environment.extent.height};
            plan.passes.push_back(std::move(pass));
        }

        // --- Upload pass access events: the arena as source, buffers as targets ---
        const auto pass_offset = request.inject_stable_upload_pass ? 1u : 0u;
        if (request.inject_stable_upload_pass)
        {
            append_access(
                state,
                access_event{
                    .pass    = pass_handle{0},
                    .kind    = resource_kind::buffer,
                    .logical = static_cast<resource_handle>(plan.upload_buffer),
                    .access  = access_type::read,
                    .state   = buffer_access_desc{.usage = buffer_usage::TRANSFER_SRC, .domain = pipeline_domain::copy, .queue = plan.passes[0].queue},
                });
            for (uint32_t index = 0; index < frame.resources.size(); ++index)
            {
                if (frame.resources[index].source != frame_resource_source::persistent_buffer || plan.frame_buffers[index] == invalid_buffer)
                    continue;
                const auto logical = plan.frame_buffers[index];
                const auto& desc   = plan.resources.buffer_metas.descs[logical];
                if ((desc.usage & buffer_usage::TRANSFER_DST) == buffer_usage::NONE)
                    continue;
                append_access(
                    state,
                    access_event{
                        .pass    = pass_handle{0},
                        .kind    = resource_kind::buffer,
                        .logical = static_cast<resource_handle>(logical),
                        .access  = access_type::write,
                        .state   = buffer_access_desc{.usage = buffer_usage::TRANSFER_DST, .domain = pipeline_domain::copy, .queue = plan.passes[0].queue},
                    });
            }
        }

        // --- Access events: buffer reads, image reads, raster attachments ---
        for (uint32_t source = 0; source < frame.passes.size(); ++source)
        {
            const auto pass        = pass_handle{source + pass_offset};
            const auto& row        = frame.passes[source];
            const auto pass_domain = domain(row.kind);
            const auto pass_queue  = plan.passes[pass].queue;
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
                append_access(
                    state,
                    access_event{
                        .pass    = pass,
                        .kind    = resource_kind::buffer,
                        .logical = static_cast<resource_handle>(plan.frame_buffers[access.resource.index]),
                        .access  = access.access,
                        .state   = buffer_access_desc{.usage = access.usage, .domain = pass_domain, .queue = pass_queue, .bytes = access.range},
                    });
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
                append_access(
                    state,
                    access_event{
                        .pass    = pass,
                        .kind    = resource_kind::image,
                        .logical = static_cast<resource_handle>(plan.frame_images[access.resource.index]),
                        .access  = access.access,
                        .state   = image_access_desc{.usage = access.usage, .domain = pass_domain, .queue = pass_queue, .subresource = access.range},
                    });
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
                append_access(state,
                              access_event{
                                  .pass    = pass,
                                  .kind    = resource_kind::image,
                                  .logical = static_cast<resource_handle>(logical),
                                  .access  = attachment.load == attachment_load_op::load ? access_type::read_write : access_type::write,
                                  .state   = image_access_desc{.usage = usage, .domain = pipeline_domain::graphics, .queue = pass_queue},
                              });
                auto raster_attachment_row = raster_attachment{
                    .image = logical,
                    .load  = attachment.load,
                    .store = attachment.store,
                    .clear = attachment.clear,
                };
                auto& raster = plan.passes[pass].raster;
                if (attachment.kind == frame_attachment_kind::color)
                    raster.colors.push_back(raster_attachment_row);
                else
                {
                    raster.has_depth_stencil = true;
                    raster.depth_stencil     = raster_attachment_row;
                }
            }
            if (row.kind == pass_kind::raster && plan.passes[pass].raster.colors.empty() && !plan.passes[pass].raster.has_depth_stencil)
                fail(state, compile_error_code::raster_pass_has_no_attachments, "raster pass has no attachments", pass);
        }

        // --- Swapchain contract: PRESENT before the frame, PRESENT after ---
        for (uint32_t index = 0; index < frame.resources.size(); ++index)
        {
            if (plan.frame_images[index] != invalid_image)
            {
                auto& contract = state.image_contracts[plan.frame_images[index]];
                if (frame.resources[index].source == frame_resource_source::swapchain_image)
                {
                    contract.has_initial_state = true;
                    contract.initial_state     = {.usage  = request.environment.swapchain_initialized ? image_usage::PRESENT : image_usage::NONE,
                                                  .domain = pipeline_domain::graphics};
                    contract.initial_access    = access_type::read;
                    contract.initial_contents  = request.environment.swapchain_initialized ? contents_policy::preserve : contents_policy::discard;
                    contract.has_final_state   = true;
                    contract.final_state       = {.usage = image_usage::PRESENT, .domain = pipeline_domain::graphics};
                }
            }
        }
        return state.output.result.succeeded();
    }

    // =========================================================================
    // Stage 3: dependency DAG construction
    // =========================================================================
    bool build_dependency_dag(compiler_state& state)
    {
        core::build_dependency_dag(state.accesses, static_cast<uint32_t>(state.output.plan.passes.size()), state.dag);
        return true;
    }

    // =========================================================================
    // Stage 4: pass culling (reverse BFS from roots)
    // =========================================================================
    bool cull_passes(compiler_state& state)
    {
        const auto pass_count = static_cast<uint32_t>(state.output.plan.passes.size());
        state.active_passes.assign(pass_count, 0);

        // --- 1. Collect root passes ---
        for (uint32_t p = 0; p < pass_count; ++p)
        {
            const auto& pass = state.output.plan.passes[p];
            // (a) backend upload pass is always a root
            if (pass.backend_upload) { state.active_passes[p] = 1; continue; }
            // (b) explicit side_effect marker
            if (pass.side_effect) { state.active_passes[p] = 1; continue; }
            // (c) writes to swapchain / imported resources: check access events
        }
        // Also detect roots via access events: swapchain attachment writes and
        // imported-resource writes.  We need the compiled plan's imported flags.
        for (const auto& event : state.accesses)
        {
            if (event.access == access_type::read) continue;
            const bool is_imported =
                (event.kind == resource_kind::image)
                    ? state.output.plan.resources.image_metas.is_imported[event.logical]
                    : state.output.plan.resources.buffer_metas.is_imported[event.logical];
            if (is_imported) state.active_passes[event.pass] = 1;
        }

        // --- 2. Build producer map: last writer per resource (declaration order) ---
        struct resource_key { resource_kind kind; resource_handle logical; };
        struct resource_key_hash
        {
            size_t operator()(const resource_key& k) const noexcept
            {
                return (static_cast<uint64_t>(k.kind) << 32) | k.logical;
            }
        };
        struct resource_key_eq
        {
            bool operator()(const resource_key& a, const resource_key& b) const noexcept
            { return a.kind == b.kind && a.logical == b.logical; }
        };
        std::unordered_map<resource_key, pass_handle, resource_key_hash, resource_key_eq> producers;
        for (const auto& event : state.accesses)
        {
            if (event.access == access_type::read) continue;
            producers[{event.kind, event.logical}] = event.pass;
        }

        // --- 3. BFS reverse traversal ---
        std::queue<pass_handle> worklist;
        for (uint32_t p = 0; p < pass_count; ++p)
            if (state.active_passes[p]) worklist.push(pass_handle{p});

        while (!worklist.empty())
        {
            const auto pass = worklist.front();
            worklist.pop();
            // For every resource this pass *reads*, enqueue its producer
            for (const auto& event : state.accesses)
            {
                if (event.pass != pass || event.access == access_type::write) continue;
                auto it = producers.find({event.kind, event.logical});
                if (it == producers.end()) continue;
                const auto producer = it->second;
                if (!state.active_passes[producer])
                {
                    state.active_passes[producer] = 1;
                    worklist.push(producer);
                }
            }
        }

        // --- 4. Mark compiled_pass_row.active and shrink access events ---
        uint32_t active_count = 0;
        for (uint32_t p = 0; p < pass_count; ++p)
        {
            if (state.active_passes[p])
                ++active_count;
            else
                state.output.plan.passes[p].active = false;
        }

        const auto removed = std::erase_if(
            state.accesses,
            [&](const access_event& e) { return !state.active_passes[e.pass]; });
        (void)removed;

        return true;
    }

    // =========================================================================
    // Stage 5: pass scheduling (active sub-graph only)
    // =========================================================================
    bool schedule_passes(compiler_state& state)
    {
        if (!core::schedule_passes(state.dag, state.active_passes, state.output.plan.scheduled_passes))
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
        for (const auto& event : state.accesses)
        {
            auto update = [&](auto& first, auto& last, auto& first_order, auto& last_order)
            {
                const auto at = order[event.pass];
                if (at < first_order[event.logical])
                {
                    first_order[event.logical] = at;
                    first[event.logical]       = event.pass;
                }
                if (last[event.logical] == invalid_pass || at > last_order[event.logical])
                {
                    last_order[event.logical] = at;
                    last[event.logical]       = event.pass;
                }
            };
            if (event.kind == resource_kind::image)
                update(plan.lifetimes.image_first_used_pass, plan.lifetimes.image_last_used_pass, image_first_order, image_last_order);
            else
                update(plan.lifetimes.buffer_first_used_pass, plan.lifetimes.buffer_last_used_pass, buffer_first_order, buffer_last_order);
        }

        // --- Physical images: reuse dead transient memory via alias handoffs ---
        auto& physical = plan.physical_resources;
        physical.handle_to_physical_img_id.assign(image_count, invalid_resource);
        physical.handle_to_image_memory_block.assign(image_count, invalid_resource);
        for (resource_handle logical = 0; logical < image_count; ++logical)
        {
            const auto& desc      = plan.resources.image_metas.descs[logical];
            resource_handle reuse = invalid_resource;
            if (desc.lifetime == resource_lifetime_class::transient && desc.aliasing != aliasing_policy::forbidden)
            {
                for (resource_handle candidate = 0; candidate < logical; ++candidate)
                {
                    if (plan.resources.image_metas.descs[candidate] == desc && image_first_order[logical] != std::numeric_limits<uint32_t>::max() &&
                        image_first_order[candidate] != std::numeric_limits<uint32_t>::max() &&
                        image_last_order[candidate] < image_first_order[logical])
                    {
                        reuse = candidate;
                        break;
                    }
                }
            }
            if (reuse != invalid_resource)
            {
                physical.handle_to_physical_img_id[logical]    = physical.handle_to_physical_img_id[reuse];
                physical.handle_to_image_memory_block[logical] = physical.handle_to_image_memory_block[reuse];
                physical.alias_handoffs.push_back({.kind         = resource_kind::image,
                                                   .previous     = reuse,
                                                   .next         = logical,
                                                   .memory_block = physical.handle_to_image_memory_block[reuse],
                                                   .at_pass      = plan.lifetimes.image_first_used_pass[logical]});
            }
            else
            {
                physical.handle_to_physical_img_id[logical] = static_cast<resource_handle>(physical.physical_image_meta.size());
                physical.physical_image_meta.push_back(logical);
                if (!plan.resources.image_metas.is_imported[logical])
                {
                    const auto requirements = (state.request->allocations.image_requirements != nullptr)
                                                  ? state.request->allocations.image_requirements(state.request->allocations.state, desc)
                                                  : default_image_requirements(desc);
                    physical.handle_to_image_memory_block[logical] = static_cast<resource_handle>(physical.image_memory_blocks.size());
                    physical.image_memory_blocks.push_back(requirements);
                }
            }
        }
        // --- Physical buffers: same aliasing pass as images ---
        physical.handle_to_physical_buf_id.assign(buffer_count, invalid_resource);
        physical.handle_to_buffer_memory_block.assign(buffer_count, invalid_resource);
        for (resource_handle logical = 0; logical < buffer_count; ++logical)
        {
            const auto& desc      = plan.resources.buffer_metas.descs[logical];
            resource_handle reuse = invalid_resource;
            if (desc.lifetime == resource_lifetime_class::transient && desc.aliasing != aliasing_policy::forbidden)
            {
                for (resource_handle candidate = 0; candidate < logical; ++candidate)
                {
                    if (plan.resources.buffer_metas.descs[candidate] == desc && buffer_first_order[logical] != std::numeric_limits<uint32_t>::max() &&
                        buffer_first_order[candidate] != std::numeric_limits<uint32_t>::max() &&
                        buffer_last_order[candidate] < buffer_first_order[logical])
                    {
                        reuse = candidate;
                        break;
                    }
                }
            }
            if (reuse != invalid_resource)
            {
                physical.handle_to_physical_buf_id[logical]     = physical.handle_to_physical_buf_id[reuse];
                physical.handle_to_buffer_memory_block[logical] = physical.handle_to_buffer_memory_block[reuse];
                physical.alias_handoffs.push_back({.kind         = resource_kind::buffer,
                                                   .previous     = reuse,
                                                   .next         = logical,
                                                   .memory_block = physical.handle_to_buffer_memory_block[reuse],
                                                   .at_pass      = plan.lifetimes.buffer_first_used_pass[logical]});
            }
            else
            {
                physical.handle_to_physical_buf_id[logical] = static_cast<resource_handle>(physical.physical_buffer_meta.size());
                physical.physical_buffer_meta.push_back(logical);
                if (!plan.resources.buffer_metas.is_imported[logical])
                {
                    const auto requirements = (state.request->allocations.buffer_requirements != nullptr)
                                                  ? state.request->allocations.buffer_requirements(state.request->allocations.state, desc)
                                                  : default_buffer_requirements(desc);
                    physical.handle_to_buffer_memory_block[logical] = static_cast<resource_handle>(physical.buffer_memory_blocks.size());
                    physical.buffer_memory_blocks.push_back(requirements);
                }
            }
        }
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
            for (const auto& event : state.accesses)
            {
                if (event.pass != pass)
                    continue;
                auto& previous   = event.kind == resource_kind::image ? images[event.logical] : buffers[event.logical];
                const auto after = abstract_state(event);
                abstract_resource_state before{};
                bool have_before = previous.valid;
                if (previous.valid)
                    before = previous.state;
                else if (event.kind == resource_kind::image)
                {
                    const auto& contract = state.image_contracts[event.logical];
                    if (contract.has_initial_state)
                    {
                        access_event initial{.kind = resource_kind::image, .access = contract.initial_access, .state = contract.initial_state};
                        before      = abstract_state(initial);
                        have_before = true;
                    }
                }
                else
                {
                    const auto& contract = state.buffer_contracts[event.logical];
                    if (contract.has_initial_state)
                    {
                        access_event initial{.kind = resource_kind::buffer, .access = contract.initial_access, .state = contract.initial_state};
                        before      = abstract_state(initial);
                        have_before = true;
                    }
                }
                if (!have_before)
                {
                    before            = after;
                    before.usage_bits = 0;
                    before.access     = access_type::read;
                }
                const auto intents = transition_intents(before, after, event.kind);
                if (intents != synchronization_intent::none)
                {
                    const auto physical = event.kind == resource_kind::image ? plan.physical_resources.handle_to_physical_img_id[event.logical]
                                                                             : plan.physical_resources.handle_to_physical_buf_id[event.logical];
                    const auto block    = event.kind == resource_kind::image ? plan.physical_resources.handle_to_image_memory_block[event.logical]
                                                                             : plan.physical_resources.handle_to_buffer_memory_block[event.logical];
                    prologues[pass].push_back({
                        .scope        = synchronization_scope::pass_prologue,
                        .phase        = before.queue == after.queue ? synchronization_phase::full : synchronization_phase::acquire,
                        .intents      = intents,
                        .kind         = event.kind,
                        .logical      = event.logical,
                        .physical     = physical,
                        .memory_block = block,
                        .pass         = pass,
                        .source_pass  = previous.pass,
                        .before       = before,
                        .after        = after,
                    });
                }
                previous = {.valid = true, .state = after, .pass = pass};
            }
        }
        // --- Alias handoffs: barrier between the previous and next user ---
        for (const auto& handoff : plan.physical_resources.alias_handoffs)
        {
            auto after = abstract_resource_state{};
            for (const auto& event : state.accesses)
                if (event.kind == handoff.kind && event.logical == handoff.next)
                {
                    after = abstract_state(event);
                    break;
                }
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
            const auto& contract = state.image_contracts[logical];
            if (!contract.has_final_state || !images[logical].valid)
                continue;
            access_event final_event{.kind = resource_kind::image, .access = contract.final_access, .state = contract.final_state};
            const auto after   = abstract_state(final_event);
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
            const auto queue = plan.passes[pass].queue;
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
        for (pass_handle source = 0; source < state.dag.outgoing.size(); ++source)
        {
            for (const auto destination : state.dag.outgoing[source])
            {
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
        for (const auto& pass : plan.passes)
        {
            hash = combine(hash, static_cast<uint64_t>(pass.kind));
            hash = combine(hash, static_cast<uint64_t>(pass.queue));
            for (const auto value : pass.name)
                hash = combine(hash, static_cast<uint8_t>(value));
            hash                       = combine(hash, pass.raster.layer_count);
            hash                       = combine(hash, static_cast<uint64_t>(pass.raster.has_depth_stencil));
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
            for (const auto& attachment : pass.raster.colors)
                hash_attachment(attachment);
            if (pass.raster.has_depth_stencil)
                hash_attachment(pass.raster.depth_stencil);
        }
        for (const auto& event : state.accesses)
        {
            hash             = combine(hash, event.pass.value);
            hash             = combine(hash, event.logical);
            hash             = combine(hash, static_cast<uint64_t>(event.kind));
            hash             = combine(hash, static_cast<uint64_t>(event.access));
            const auto state = abstract_state(event);
            hash             = combine(hash, state.usage_bits);
            hash             = combine(hash, static_cast<uint64_t>(state.domain));
            hash             = combine(hash, static_cast<uint64_t>(state.queue));
            if (event.kind == resource_kind::image)
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
        plan.cache_key  = hash;

        // --- Statistics: counts for one frame of the compiled plan ---
        plan.statistics = {
            .pass_count                = static_cast<uint32_t>(plan.passes.size()),
            .active_pass_count         = static_cast<uint32_t>(plan.scheduled_passes.size()),
            .image_count               = static_cast<uint32_t>(plan.resources.image_metas.descs.size()),
            .buffer_count              = static_cast<uint32_t>(plan.resources.buffer_metas.descs.size()),
            .access_event_count        = static_cast<uint32_t>(state.accesses.size()),
            .synchronization_op_count  = static_cast<uint32_t>(plan.synchronization.ops.size()),
            .submission_batch_count    = static_cast<uint32_t>(plan.submissions.batches.size()),
            .image_memory_block_count  = static_cast<uint32_t>(plan.physical_resources.image_memory_blocks.size()),
            .buffer_memory_block_count = static_cast<uint32_t>(plan.physical_resources.buffer_memory_blocks.size()),
            .culled_pass_count         = static_cast<uint32_t>(plan.passes.size()) - static_cast<uint32_t>(plan.scheduled_passes.size()),
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
