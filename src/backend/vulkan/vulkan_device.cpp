#include "vulkan_device.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "render_graph/system.h"
#include "vk_backend.h"
#include "vk_resource_lowering.h"

namespace render_graph::vulkan
{
    static_assert(sizeof(indexed_indirect_command) == sizeof(VkDrawIndexedIndirectCommand));
    namespace
    {
        template <typename Native>
        struct handle_row
        {
            Native native{};
            uint32_t generation = 1;
            bool alive = false;
        };

        struct buffer_native
        {
            vk_buffer_resource_handle handle;
            buffer_desc desc;
            vk_buffer_slice slice;
            bool suballocated = false;
        };
        struct image_native { vk_image_resource_handle handle; image_desc desc; };
        struct sampler_native { vk_bindless_handle slot; };
        struct pipeline_native { vk_pipeline_handle handle; };
        struct bindless_native { vk_bindless_handle handle; };

        struct graph_buffer_row
        {
            device_buffer_handle device;
            buffer_handle logical = invalid_buffer;
            buffer_usage usage = buffer_usage::NONE;
        };

        struct device_state
        {
            using frame_graph = render_graph_system<vk_backend>;
            vk_runtime runtime;
            vk_buffer_resource_handle device_buffer_arena;
            std::vector<handle_row<buffer_native>> buffers;
            std::vector<handle_row<image_native>> images;
            std::vector<handle_row<sampler_native>> samplers;
            std::vector<handle_row<pipeline_native>> pipelines;
            std::vector<handle_row<bindless_native>> bindless;
            std::unique_ptr<frame_graph> graph;
            std::vector<graph_buffer_row> graph_buffers;
            uint64_t graph_cache_key = 0;
            bool graph_cache_key_valid = false;
            buffer_handle upload_buffer{};
            image_handle swapchain_image{};
            image_handle depth_image{};
            std::vector<bool> swapchain_initialized;
            frame_plan* current_plan = nullptr;
            std::vector<vk_indexed_indirect_draw_row> native_draws;
            render_statistics statistics;
            bool resize_requested = false;
            bool shutdown = false;
        };

        template <typename Handle, typename Table, typename Native>
        Handle publish_handle(Table& table, Native native)
        {
            for (uint32_t index = 0; index < table.size(); ++index)
            {
                if (table[index].alive) continue;
                table[index].alive = true;
                table[index].native = std::move(native);
                return {index, table[index].generation};
            }
            table.push_back({.native = std::move(native), .generation = 1, .alive = true});
            return {static_cast<uint32_t>(table.size() - 1), 1};
        }

        template <typename Handle, typename Table>
        auto* find_handle(Table& table, Handle handle)
        {
            if (!handle || handle.index >= table.size()) return static_cast<typename Table::value_type*>(nullptr);
            auto& row = table[handle.index];
            return row.alive && row.generation == handle.generation ? &row : nullptr;
        }

        template <typename Handle, typename Table>
        const auto* find_handle(const Table& table, Handle handle)
        {
            if (!handle || handle.index >= table.size()) return static_cast<const typename Table::value_type*>(nullptr);
            const auto& row = table[handle.index];
            return row.alive && row.generation == handle.generation ? &row : nullptr;
        }

        uint64_t hash_combine(uint64_t seed, uint64_t value) noexcept
        {
            return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
        }

        void include_graph_buffer(std::vector<graph_buffer_row>& rows,
                                  device_buffer_handle handle,
                                  buffer_usage usage)
        {
            const auto found = std::find_if(rows.begin(), rows.end(),
                [handle](const graph_buffer_row& row) { return row.device == handle; });
            if (found != rows.end()) found->usage = found->usage | usage;
            else rows.push_back({.device = handle, .usage = usage});
        }

        VkShaderStageFlagBits lower_stage(shader_stage value)
        {
            if (value == shader_stage::fragment) return VK_SHADER_STAGE_FRAGMENT_BIT;
            if (value == shader_stage::compute) return VK_SHADER_STAGE_COMPUTE_BIT;
            return VK_SHADER_STAGE_VERTEX_BIT;
        }

        VkShaderStageFlags lower_stage_mask(uint32_t value)
        {
            VkShaderStageFlags flags = 0;
            if (value & shader_stage_vertex_bit) flags |= VK_SHADER_STAGE_VERTEX_BIT;
            if (value & shader_stage_fragment_bit) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
            if (value & shader_stage_compute_bit) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
            return flags;
        }

        VkFormat lower_vertex_format(vertex_format value)
        {
            switch (value)
            {
            case vertex_format::float2: return VK_FORMAT_R32G32_SFLOAT;
            case vertex_format::float3: return VK_FORMAT_R32G32B32_SFLOAT;
            case vertex_format::float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case vertex_format::uint4: return VK_FORMAT_R32G32B32A32_UINT;
            }
            return VK_FORMAT_UNDEFINED;
        }

        vk_graphics_pipeline_desc lower_pipeline(const graphics_pipeline_desc& source, const device_state& state)
        {
            vk_graphics_pipeline_desc output;
            for (const auto& shader : source.shaders)
                output.shaders.push_back({.stage = lower_stage(shader.stage), .spirv = shader.binary, .entry = shader.entry});
            for (const auto& binding : source.vertex_bindings)
                output.vertex_layout.bindings.push_back({binding.binding, binding.stride,
                    binding.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
            for (const auto& attribute : source.vertex_attributes)
                output.vertex_layout.attributes.push_back({attribute.location, attribute.binding,
                                                            lower_vertex_format(attribute.format), attribute.offset});
            output.raster.topology = source.topology == primitive_topology::line_list ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                   : source.topology == primitive_topology::point_list ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                                                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            output.raster.cull_mode = source.cull == cull_mode::none ? VK_CULL_MODE_NONE
                                    : source.cull == cull_mode::front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
            output.raster.front_face = source.winding == front_face::clockwise ? VK_FRONT_FACE_CLOCKWISE
                                                                               : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            output.raster.depth_test = source.depth_test;
            output.raster.depth_write = source.depth_write;
            output.raster.blend = source.blend;
            for (const auto color : source.color_formats)
                output.color_formats.push_back(color == format::UNDEFINED ? state.runtime.swapchain_images().format
                                                                          : lower_vk_format(color));
            output.depth_format = lower_vk_format(source.depth_format);
            output.samples = static_cast<VkSampleCountFlagBits>(static_cast<uint32_t>(source.samples));
            for (const auto& range : source.push_constants)
                output.push_constants.push_back({lower_stage_mask(range.stage_mask), range.offset, range.size});
            return output;
        }

        resource_change_result apply_changes(void* value, const resource_change_batch& batch)
        {
            auto& state = *static_cast<device_state*>(value);
            resource_change_result output;
            for (const auto& row : batch.buffer_creates)
            {
                buffer_native native{.desc = row.desc};
                if (row.desc.memory == memory_domain::device_local &&
                    row.desc.allocation == allocation_policy::automatic)
                {
                    if (!state.runtime.allocate_buffer_slice(state.device_buffer_arena, row.desc.size, 256, native.slice))
                    { output.error = state.runtime.last_error(); return output; }
                    native.handle = native.slice.buffer;
                    native.suballocated = true;
                }
                else
                {
                    const auto created = state.runtime.create_buffer(row.desc, native.handle);
                    if (!created) { output.error = created.error; return output; }
                    native.slice = {.buffer = native.handle, .offset = 0, .size = row.desc.size};
                }
                output.buffers.push_back(publish_handle<device_buffer_handle>(state.buffers, std::move(native)));
            }
            for (const auto& row : batch.image_creates)
            {
                vk_image_resource_handle native;
                const auto created = state.runtime.create_image(row.desc, native);
                if (!created) { output.error = created.error; return output; }
                output.images.push_back(publish_handle<device_image_handle>(state.images, image_native{native, row.desc}));
            }
            for (const auto& row : batch.sampler_creates)
            {
                vk_bindless_handle native;
                const auto created = state.runtime.create_sampler({
                    .min_filter = row.desc.min_filter == sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                    .mag_filter = row.desc.mag_filter == sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                    .address_u = row.desc.address_u == sampler_address_mode::clamp_to_edge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                               : row.desc.address_u == sampler_address_mode::mirrored_repeat ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                                                            : VK_SAMPLER_ADDRESS_MODE_REPEAT,
                    .address_v = row.desc.address_v == sampler_address_mode::clamp_to_edge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                               : row.desc.address_v == sampler_address_mode::mirrored_repeat ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                                                            : VK_SAMPLER_ADDRESS_MODE_REPEAT,
                    .max_lod = row.desc.max_lod,
                }, native);
                if (!created) { output.error = created.error; return output; }
                output.samplers.push_back(publish_handle<device_sampler_handle>(state.samplers, sampler_native{native}));
            }
            for (const auto& row : batch.pipeline_creates)
            {
                for (const auto& shader : row.desc.shaders)
                    if (shader.binary_format != shader_binary_format::spirv)
                    { output.error = "Vulkan backend requires SPIR-V shader rows"; return output; }
                vk_pipeline_handle native;
                const auto created = state.runtime.create_graphics_pipeline(lower_pipeline(row.desc, state), native);
                if (!created) { output.error = created.error; return output; }
                output.pipelines.push_back(publish_handle<device_pipeline_handle>(state.pipelines, pipeline_native{native}));
            }
            for (const auto& row : batch.buffer_uploads)
            {
                if (row.bytes.empty()) continue;
                auto* destination = find_handle(state.buffers, row.destination);
                if (!destination) { output.error = "Buffer upload references a stale handle"; return output; }
                if (row.offset + row.bytes.size() > destination->native.desc.size)
                { output.error = "Buffer upload exceeds the logical buffer range"; return output; }
                const auto native = destination->native.handle;
                const bool uploaded = destination->native.desc.memory == memory_domain::device_local
                    ? state.runtime.stage_buffer_upload({native, destination->native.slice.offset + row.offset,
                                                        row.bytes.size()}, row.bytes)
                    : state.runtime.update_buffer(native, destination->native.slice.offset + row.offset, row.bytes);
                if (!uploaded) { output.error = state.runtime.last_error(); return output; }
            }
            for (const auto& row : batch.image_uploads)
            {
                auto* destination = find_handle(state.images, row.destination);
                if (!destination) { output.error = "Image upload references a stale handle"; return output; }
                if (!state.runtime.stage_image_upload(destination->native.handle,
                                                      row.mip_level,
                                                      0,
                                                      extent_3d{row.width, row.height, 1},
                                                      row.bytes))
                { output.error = state.runtime.last_error(); return output; }
            }
            for (const auto& row : batch.bindless_publishes)
            {
                vk_bindless_handle native;
                vk_runtime_result published;
                if (row.table == bindless_table_kind::samplers)
                {
                    auto* sampler = find_handle(state.samplers, row.sampler);
                    if (!sampler) { output.error = "Bindless sampler publish references a stale handle"; return output; }
                    native = sampler->native.slot;
                }
                else if (row.table == bindless_table_kind::sampled_images)
                {
                    auto* image = find_handle(state.images, row.image);
                    if (!image) { output.error = "Bindless image publish references a stale handle"; return output; }
                    published = state.runtime.allocate_sampled_image(image->native.handle, lower_vk_format(image->native.desc.fmt), native);
                }
                else if (row.table == bindless_table_kind::storage_images)
                {
                    auto* image = find_handle(state.images, row.image);
                    if (!image) { output.error = "Bindless storage image publish references a stale handle"; return output; }
                    published = state.runtime.allocate_storage_image(image->native.handle,
                                                                      lower_vk_format(image->native.desc.fmt), native);
                }
                else
                {
                    auto* buffer = find_handle(state.buffers, row.buffer);
                    if (!buffer) { output.error = "Bindless buffer publish references a stale handle"; return output; }
                    published = row.table == bindless_table_kind::uniform_buffers
                        ? state.runtime.allocate_uniform_buffer(buffer->native.handle,
                            buffer->native.slice.offset + row.offset, row.size, native)
                        : state.runtime.allocate_storage_buffer(buffer->native.handle,
                            buffer->native.slice.offset + row.offset, row.size, native);
                }
                if (!published) { output.error = published.error; return output; }
                output.bindless.push_back(publish_handle<device_bindless_handle>(state.bindless, bindless_native{native}));
                output.bindless_slots.push_back(native.index);
            }
            for (const auto& row : batch.retires)
            {
                const uint64_t safe_after = state.runtime.frames().next_submission;
                if (auto* buffer = find_handle(state.buffers, row.buffer))
                {
                    if (buffer->native.suballocated)
                        state.runtime.release_buffer_slice(buffer->native.slice, safe_after);
                    else state.runtime.destroy_buffer(buffer->native.handle, safe_after);
                    buffer->alive = false; ++buffer->generation;
                }
                if (auto* image = find_handle(state.images, row.image))
                { state.runtime.destroy_image(image->native.handle, safe_after); image->alive = false; ++image->generation; }
                if (auto* sampler = find_handle(state.samplers, row.sampler))
                { state.runtime.release_bindless(sampler->native.slot, safe_after); sampler->alive = false; ++sampler->generation; }
                if (auto* bindless = find_handle(state.bindless, row.bindless))
                { state.runtime.release_bindless(bindless->native.handle, safe_after); bindless->alive = false; ++bindless->generation; }
                if (auto* pipeline = find_handle(state.pipelines, row.pipeline))
                { pipeline->alive = false; ++pipeline->generation; }
            }
            state.statistics.pipeline_creations = state.runtime.pipelines().creations;
            state.statistics.descriptor_updates = state.runtime.bindless().statistics.descriptor_updates;
            return output;
        }

        bool rebuild_graph(device_state& state,
                           const frame_environment& environment,
                           uint32_t image_index,
                           uint64_t cache_key)
        {
            using setup_context = device_state::frame_graph::pass_setup_context;
            using execute_context = device_state::frame_graph::pass_execute_context;
            auto& graph = *state.graph;
            graph.begin_frame(environment.submission, environment.completed_submission, cache_key);
            if (!graph.needs_recompile()) return true;
            graph.clear();

            const buffer_desc upload_desc{
                .size = 64ull * 1024ull * 1024ull,
                .usage = buffer_usage::TRANSFER_SRC,
                .memory = memory_domain::upload,
                .mapping = mapping_policy::persistent,
                .aliasing = aliasing_policy::forbidden,
                .lifetime = resource_lifetime_class::imported,
            };
            graph.add_copy_pass("UploadPass", [&state, upload_desc](setup_context& context)
            {
                state.upload_buffer = context.import_buffer("UploadArena", upload_desc);
                const buffer_access_desc source_access{
                    .usage = buffer_usage::TRANSFER_SRC,
                    .domain = pipeline_domain::copy,
                };
                context.set_initial_state(state.upload_buffer, source_access, access_type::read,
                                          contents_policy::preserve);
                context.read_buffer(state.upload_buffer, source_access);
                for (auto& binding : state.graph_buffers)
                {
                    auto desc = find_handle(state.buffers, binding.device)->native.desc;
                    if ((desc.usage & buffer_usage::TRANSFER_DST) == buffer_usage::NONE) continue;
                    desc.lifetime = resource_lifetime_class::imported;
                    binding.logical = context.import_buffer("DeviceBuffer" + std::to_string(binding.device.index), desc);
                    const buffer_access_desc destination_access{
                        .usage = buffer_usage::TRANSFER_DST,
                        .domain = pipeline_domain::copy,
                    };
                    context.set_initial_state(binding.logical, destination_access, access_type::write,
                                              contents_policy::preserve);
                    context.write_buffer(binding.logical, destination_access);
                }
            }, [&state](execute_context& context)
            {
                if (state.runtime.has_pending_uploads())
                {
                    ++state.statistics.upload_pass_executions;
                    if (!state.runtime.record_pending_uploads(context.commands()))
                        throw std::runtime_error(state.runtime.last_error());
                }
            });

            const image_desc swapchain_desc{
                .fmt = environment.color_format,
                .extent = environment.extent,
                .usage = image_usage::COLOR_ATTACHMENT | image_usage::PRESENT,
                .memory = memory_domain::device_local,
                .aliasing = aliasing_policy::forbidden,
                .lifetime = resource_lifetime_class::imported,
            };
            const image_desc depth_desc{
                .fmt = format::D32_SFLOAT,
                .extent = environment.extent,
                .usage = image_usage::DEPTH_STENCIL_ATTACHMENT,
                .memory = memory_domain::device_local,
                .lifetime = resource_lifetime_class::transient,
            };
            const bool initialized = state.swapchain_initialized[image_index];
            graph.add_raster_pass(state.current_plan->pass_name,
                [&state, swapchain_desc, depth_desc, environment, initialized](setup_context& context)
                {
                    for (auto& binding : state.graph_buffers)
                    {
                        if (binding.logical == invalid_buffer)
                        {
                            auto desc = find_handle(state.buffers, binding.device)->native.desc;
                            desc.lifetime = resource_lifetime_class::imported;
                            binding.logical = context.import_buffer(
                                "DeviceBuffer" + std::to_string(binding.device.index), desc);
                            const buffer_access_desc initial{
                                .usage = binding.usage,
                                .domain = pipeline_domain::graphics,
                            };
                            context.set_initial_state(binding.logical, initial, access_type::read,
                                                      contents_policy::preserve);
                        }
                        const buffer_access_desc access{
                            .usage = binding.usage,
                            .domain = pipeline_domain::graphics,
                        };
                        context.read_buffer(binding.logical, access);
                    }
                    state.swapchain_image = context.import_image("Swapchain", swapchain_desc);
                    const image_access_desc present{
                        .usage = image_usage::PRESENT,
                        .domain = pipeline_domain::graphics,
                    };
                    context.set_initial_state(state.swapchain_image,
                        initialized ? present : image_access_desc{.usage = image_usage::NONE,
                                                                  .domain = pipeline_domain::graphics},
                        access_type::read,
                        initialized ? contents_policy::preserve : contents_policy::discard);
                    context.set_final_state(state.swapchain_image, present, access_type::read);
                    context.set_render_area({.width = environment.extent.width, .height = environment.extent.height});
                    context.add_color_attachment(state.swapchain_image,
                        attachment_load_op::clear, attachment_store_op::store,
                        clear_value{.color = state.current_plan->clear_color});
                    if (state.current_plan->depth_attachment)
                    {
                        state.depth_image = context.create_image("Depth", depth_desc);
                        context.set_depth_stencil_attachment(state.depth_image,
                            attachment_load_op::clear, attachment_store_op::dont_care,
                            clear_value{.depth = 1.0F});
                    }
                    context.declare_image_output(state.swapchain_image);
                },
                [&state, environment](execute_context& context)
                {
                    ++state.statistics.draw_pass_executions;
                    if (!state.runtime.record_indexed_indirect({
                        .commands = context.commands(),
                        .extent = {environment.extent.width, environment.extent.height},
                        .push_constants = state.current_plan->push_constants,
                        .push_stages = lower_stage_mask(state.current_plan->push_constant_stage_mask),
                        .rows = state.native_draws,
                    })) throw std::runtime_error(state.runtime.last_error());
                });
            const auto compiled = graph.compile();
            if (!compiled.succeeded())
            {
                graph.abort_frame();
                return false;
            }
            ++state.statistics.graph_compiles;
            return true;
        }

        bool record_graph(void* value, VkCommandBuffer commands, uint32_t image_index)
        {
            auto& state = *static_cast<device_state*>(value);
            state.graph->bind_imported_buffer(state.upload_buffer,
                state.runtime.buffer(state.runtime.resources().upload_arena));
            for (const auto& binding : state.graph_buffers)
            {
                const auto* native = find_handle(state.buffers, binding.device);
                if (!native) return false;
                state.graph->bind_imported_buffer(binding.logical, state.runtime.buffer(native->native.handle));
            }
            state.graph->bind_imported_image(state.swapchain_image,
                state.runtime.swapchain_images().rows[image_index].image);
            return state.graph->execute(commands).succeeded();
        }

        frame_result render_frame(void* value, const frame_recipe& recipe)
        {
            auto& state = *static_cast<device_state*>(value);
            if (state.shutdown) return {.error = "Vulkan render device is shut down"};
            if (recipe.build == nullptr) return {.error = "Frame recipe has no builder"};
            if (state.resize_requested)
            {
                const auto resized = state.runtime.resize();
                if (!resized) return {.error = resized.error};
                state.swapchain_initialized.assign(state.runtime.swapchain_images().rows.size(), false);
                state.resize_requested = false;
            }
            vk_frame_token token;
            const auto acquired = state.runtime.acquire(token);
            if (acquired == vk_frame_status::skipped)
            {
                state.resize_requested = true;
                return {.status = frame_status::skipped};
            }
            if (acquired == vk_frame_status::failed) return {.error = state.runtime.last_error()};

            const auto& swapchain = state.runtime.swapchain_images();
            frame_environment environment{
                .extent = {swapchain.extent.width, swapchain.extent.height, 1},
                .color_format = normalize_vk_format(swapchain.format),
                .frame_index = token.frame_index,
                .submission = state.runtime.frames().next_submission,
                .completed_submission = state.runtime.frames().completed_submission,
            };
            frame_plan plan;
            const auto built = recipe.build(recipe.state, environment, plan);
            if (!built) return {.error = built.error};
            if (!plan.buffer_copies.empty() || !plan.dispatches.empty())
                return {.error = "Vulkan frame recipes do not yet support explicit copy or dispatch rows"};

            state.current_plan = &plan;
            std::vector<graph_buffer_row> graph_buffers;
            state.native_draws.clear();
            uint64_t cache_key = hash_combine(plan.cache_key,
                (static_cast<uint64_t>(swapchain.extent.width) << 32) | swapchain.extent.height);
            cache_key = hash_combine(cache_key, static_cast<uint64_t>(swapchain.format));
            cache_key = hash_combine(cache_key, state.swapchain_initialized[token.image_index]);
            for (const auto& draw : plan.indexed_indirect_draws)
            {
                const auto* pipeline = find_handle(state.pipelines, draw.pipeline);
                const auto* vertex = find_handle(state.buffers, draw.vertex_buffer);
                const auto* index = find_handle(state.buffers, draw.index_buffer);
                const auto* indirect = find_handle(state.buffers, draw.indirect_buffer);
                if (!pipeline || !vertex || !index || !indirect)
                { state.current_plan = nullptr; return {.error = "Frame recipe references a stale draw handle"}; }
                include_graph_buffer(graph_buffers, draw.vertex_buffer, buffer_usage::VERTEX_BUFFER);
                include_graph_buffer(graph_buffers, draw.index_buffer, buffer_usage::INDEX_BUFFER);
                include_graph_buffer(graph_buffers, draw.indirect_buffer, buffer_usage::INDIRECT_BUFFER);
                cache_key = hash_combine(cache_key, (static_cast<uint64_t>(draw.pipeline.index) << 32) |
                                                     draw.pipeline.generation);
                cache_key = hash_combine(cache_key, (static_cast<uint64_t>(draw.vertex_buffer.index) << 32) |
                                                     draw.vertex_buffer.generation);
                cache_key = hash_combine(cache_key, (static_cast<uint64_t>(draw.index_buffer.index) << 32) |
                                                     draw.index_buffer.generation);
                cache_key = hash_combine(cache_key, (static_cast<uint64_t>(draw.indirect_buffer.index) << 32) |
                                                     draw.indirect_buffer.generation);
                state.native_draws.push_back({
                    .pipeline = pipeline->native.handle,
                    .vertex_buffer = state.runtime.buffer(vertex->native.handle),
                    .vertex_offset = vertex->native.slice.offset + draw.vertex_offset,
                    .index_buffer = state.runtime.buffer(index->native.handle),
                    .index_offset = index->native.slice.offset + draw.index_offset,
                    .index_type = draw.indices == index_format::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
                    .indirect_buffer = state.runtime.buffer(indirect->native.handle),
                    .indirect_offset = indirect->native.slice.offset + draw.indirect_offset,
                    .draw_count = draw.draw_count,
                    .stride = draw.stride,
                });
                if (draw.draw_count != 0) ++state.statistics.indirect_groups;
            }
            if (!state.graph_cache_key_valid || state.graph_cache_key != cache_key)
            {
                state.graph_buffers = std::move(graph_buffers);
                state.graph_cache_key = cache_key;
                state.graph_cache_key_valid = true;
            }
            if (!rebuild_graph(state, environment, token.image_index, cache_key) ||
                !state.runtime.realize_resources() ||
                !state.runtime.record_batches(token, &state, &record_graph))
            {
                state.graph->abort_frame();
                state.current_plan = nullptr;
                return {.error = state.runtime.last_error().empty() ? "Render graph execution failed"
                                                                    : state.runtime.last_error()};
            }
            if (!state.runtime.submit(token))
            {
                state.graph->abort_frame();
                state.current_plan = nullptr;
                return {.error = state.runtime.last_error()};
            }
            state.runtime.commit_pending_uploads(state.runtime.frames().next_submission - 1);
            if (!state.graph->commit_frame())
            { state.current_plan = nullptr; return {.error = "Render graph frame commit failed"}; }
            const auto presented = state.runtime.present(token);
            state.current_plan = nullptr;
            if (presented == vk_frame_status::failed) return {.error = state.runtime.last_error()};
            state.swapchain_initialized[token.image_index] = true;
            state.statistics.presented_frames = state.runtime.statistics().presented_frames;
            if (presented == vk_frame_status::skipped)
            {
                state.resize_requested = true;
                return {.status = frame_status::skipped};
            }
            return {.status = frame_status::rendered};
        }

        const render_device_api device_api{
            .apply_resource_changes = &apply_changes,
            .render = &render_frame,
            .request_resize = [](void* value) noexcept
            { static_cast<device_state*>(value)->resize_requested = true; },
            .shutdown = [](void* value) noexcept
            {
                auto& state = *static_cast<device_state*>(value);
                if (!state.shutdown)
                {
                    state.shutdown = true;
                    state.runtime.wait_idle();
                    state.graph.reset();
                    state.runtime.shutdown();
                }
            },
            .statistics = [](const void* value) noexcept { return static_cast<const device_state*>(value)->statistics; },
            .validation_error_count = [](const void* value) noexcept
            { return static_cast<const device_state*>(value)->runtime.validation_error_count(); },
            .destroy = [](void* value) noexcept { delete static_cast<device_state*>(value); },
        };
    } // namespace

    device_create_result create_device(const device_config& config)
    {
        auto state = std::make_unique<device_state>();
        const auto initialized = state->runtime.initialize({
            .application_name = config.application_name,
            .frames_in_flight = config.frames_in_flight,
            .validation = config.validation,
            .surface = config.surface,
        });
        if (!initialized) return {.error = initialized.error};
        const auto arena_created = state->runtime.create_buffer(buffer_desc{
            .size = 256ull * 1024ull * 1024ull,
            .usage = buffer_usage::TRANSFER_DST | buffer_usage::VERTEX_BUFFER |
                     buffer_usage::INDEX_BUFFER | buffer_usage::STORAGE_BUFFER |
                     buffer_usage::INDIRECT_BUFFER | buffer_usage::UNIFORM_BUFFER,
            .memory = memory_domain::device_local,
            .allocation = allocation_policy::dedicated,
            .aliasing = aliasing_policy::forbidden,
            .lifetime = resource_lifetime_class::persistent,
        }, state->device_buffer_arena);
        if (!arena_created) return {.error = arena_created.error};
        const auto family = state->runtime.queues().graphics.family;
        state->graph = std::make_unique<device_state::frame_graph>();
        state->graph->set_queue_availability({.compute = false, .copy = false});
        state->graph->set_backend_context(state->runtime.devices().physical_device,
                                          state->runtime.devices().device,
                                          state->runtime.devices().allocator,
                                          vk_queue_family_indices{
                                              .graphics = family,
                                              .compute = family,
                                              .copy = family,
                                          },
                                          config.frames_in_flight);
        state->swapchain_initialized.assign(state->runtime.swapchain_images().rows.size(), false);
        return {.device = render_device(state.release(), &device_api)};
    }
} // namespace render_graph::vulkan
