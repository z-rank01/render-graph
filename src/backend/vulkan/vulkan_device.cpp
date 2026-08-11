#include "render_graph/backend/vulkan/device.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "render_graph/system.h"
#include "vk_backend.h"
#include "vk_resource_lowering.h"
#include "vk_runtime.h"

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
        struct bindless_native { vk_bindless_handle handle; bool owns_slot = true; };

        struct vulkan_device_state
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
            buffer_handle upload_buffer{};
            std::vector<bool> swapchain_initialized;
            frame_plan* current_plan = nullptr;
            std::vector<vk_indexed_indirect_draw_row> native_draws;
            std::vector<vk_buffer_copy_command_row> native_copies;
            std::vector<vk_dispatch_command_row> native_dispatches;
            std::vector<buffer_handle> frame_buffers;
            std::vector<image_handle> frame_images;
            render_statistics statistics;
            bool resize_requested = false;
            bool shutdown = false;
        };

        using device_state = vulkan_device_state;

        struct vulkan_frame_phase_table
        {
            vk_frame_status (*acquire)(device_state&, vk_frame_token&);
            bool (*realize_resources)(device_state&);
            bool (*record_batches)(device_state&, vk_frame_token&);
            bool (*submit)(device_state&, const vk_frame_token&);
            vk_frame_status (*present)(device_state&, const vk_frame_token&);
            void (*collect_retired)(device_state&);
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

        vk_compute_pipeline_desc lower_pipeline(const compute_pipeline_desc& source)
        {
            vk_compute_pipeline_desc output;
            output.shader = {.stage = VK_SHADER_STAGE_COMPUTE_BIT,
                             .spirv = source.shader.binary,
                             .entry = source.shader.entry};
            for (const auto& range : source.push_constants)
                output.push_constants.push_back({lower_stage_mask(range.stage_mask), range.offset, range.size});
            return output;
        }

        resource_change_result apply_changes(void* value, const resource_change_batch& batch)
        {
            auto& state = *static_cast<device_state*>(value);
            const auto fail = [](resource_change_phase phase, resource_change_row_kind kind,
                                 uint32_t index, std::string message)
            {
                resource_change_result result;
                result.error = message;
                result.diagnostic = {phase, kind, index, std::move(message)};
                return result;
            };

            for (uint32_t index = 0; index < batch.buffer_creates.size(); ++index)
            {
                const auto validated = vk_backend::validate_buffer_desc(batch.buffer_creates[index].desc);
                if (!validated.supported)
                    return fail(resource_change_phase::validate, resource_change_row_kind::buffer_create,
                                index, validated.message);
            }
            for (uint32_t index = 0; index < batch.image_creates.size(); ++index)
            {
                const auto validated = vk_backend::validate_image_desc(batch.image_creates[index].desc);
                if (!validated.supported)
                    return fail(resource_change_phase::validate, resource_change_row_kind::image_create,
                                index, validated.message);
            }
            for (uint32_t index = 0; index < batch.graphics_pipeline_creates.size(); ++index)
                for (const auto& shader : batch.graphics_pipeline_creates[index].desc.shaders)
                    if (shader.binary_format != shader_binary_format::spirv)
                        return fail(resource_change_phase::validate, resource_change_row_kind::pipeline_create,
                                    index, "Vulkan backend requires SPIR-V shader rows");
            for (uint32_t index = 0; index < batch.compute_pipeline_creates.size(); ++index)
            {
                const auto& shader = batch.compute_pipeline_creates[index].desc.shader;
                if (shader.stage != shader_stage::compute || shader.binary_format != shader_binary_format::spirv)
                    return fail(resource_change_phase::validate, resource_change_row_kind::pipeline_create,
                                index, "Vulkan compute pipeline requires one compute SPIR-V shader row");
            }
            for (uint32_t index = 0; index < batch.buffer_uploads.size(); ++index)
            {
                const auto& row = batch.buffer_uploads[index];
                const auto* destination = find_handle(state.buffers, row.destination);
                if (!destination)
                    return fail(resource_change_phase::validate, resource_change_row_kind::buffer_upload,
                                index, "Buffer upload references a stale handle");
                if (row.offset > destination->native.desc.size ||
                    row.bytes.size() > destination->native.desc.size - row.offset)
                    return fail(resource_change_phase::validate, resource_change_row_kind::buffer_upload,
                                index, "Buffer upload exceeds the logical buffer range");
            }
            for (uint32_t index = 0; index < batch.image_uploads.size(); ++index)
                if (!find_handle(state.images, batch.image_uploads[index].destination))
                    return fail(resource_change_phase::validate, resource_change_row_kind::image_upload,
                                index, "Image upload references a stale handle");
            for (uint32_t index = 0; index < batch.bindless_publishes.size(); ++index)
            {
                const auto& row = batch.bindless_publishes[index];
                const bool valid = row.table == bindless_table_kind::samplers
                    ? find_handle(state.samplers, row.sampler) != nullptr
                    : (row.table == bindless_table_kind::sampled_images || row.table == bindless_table_kind::storage_images)
                        ? find_handle(state.images, row.image) != nullptr
                        : find_handle(state.buffers, row.buffer) != nullptr;
                if (!valid)
                    return fail(resource_change_phase::validate, resource_change_row_kind::bindless_publish,
                                index, "Bindless publish references a stale handle");
                if (row.table == bindless_table_kind::uniform_buffers || row.table == bindless_table_kind::storage_buffers)
                {
                    const auto* buffer = find_handle(state.buffers, row.buffer);
                    if (row.offset > buffer->native.desc.size || row.size == 0 ||
                        row.size > buffer->native.desc.size - row.offset)
                        return fail(resource_change_phase::validate, resource_change_row_kind::bindless_publish,
                                    index, "Bindless buffer range exceeds the logical buffer");
                }
            }

            std::vector<buffer_native> prepared_buffers;
            std::vector<image_native> prepared_images;
            std::vector<sampler_native> prepared_samplers;
            std::vector<pipeline_native> prepared_graphics_pipelines;
            std::vector<pipeline_native> prepared_compute_pipelines;
            std::vector<bindless_native> prepared_bindless;
            const auto upload_checkpoint = state.runtime.upload_checkpoint();
            const auto rollback = [&]
            {
                state.runtime.rollback_pending_uploads(upload_checkpoint);
                const uint64_t completed = state.runtime.frames().completed_submission;
                for (const auto& native : prepared_bindless)
                    if (native.owns_slot) state.runtime.release_bindless(native.handle, completed);
                for (const auto& native : prepared_samplers)
                    state.runtime.release_bindless(native.slot, completed);
                for (const auto& native : prepared_images)
                    state.runtime.destroy_image(native.handle, completed);
                for (const auto& native : prepared_buffers)
                    if (native.suballocated) state.runtime.release_buffer_slice(native.slice, completed);
                    else state.runtime.destroy_buffer(native.handle, completed);
                state.runtime.collect_retired();
            };

            for (uint32_t index = 0; index < batch.graphics_pipeline_creates.size(); ++index)
            {
                vk_pipeline_handle native;
                const auto created = state.runtime.create_graphics_pipeline(
                    lower_pipeline(batch.graphics_pipeline_creates[index].desc, state), native);
                if (!created)
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::pipeline_create,
                                index, created.error);
                }
                prepared_graphics_pipelines.push_back({native});
            }
            for (uint32_t index = 0; index < batch.compute_pipeline_creates.size(); ++index)
            {
                vk_pipeline_handle native;
                const auto created = state.runtime.create_compute_pipeline(
                    lower_pipeline(batch.compute_pipeline_creates[index].desc), native);
                if (!created)
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::pipeline_create,
                                index, created.error);
                }
                prepared_compute_pipelines.push_back({native});
            }
            for (uint32_t index = 0; index < batch.buffer_creates.size(); ++index)
            {
                const auto& row = batch.buffer_creates[index];
                buffer_native native{.desc = row.desc};
                if (row.desc.memory == memory_domain::device_local &&
                    row.desc.allocation == allocation_policy::automatic)
                {
                    if (!state.runtime.allocate_buffer_slice(state.device_buffer_arena, row.desc.size, 256, native.slice))
                    {
                        rollback();
                        return fail(resource_change_phase::prepare, resource_change_row_kind::buffer_create,
                                    index, state.runtime.last_error());
                    }
                    native.handle = native.slice.buffer;
                    native.suballocated = true;
                }
                else
                {
                    const auto created = state.runtime.create_buffer(row.desc, native.handle);
                    if (!created)
                    {
                        rollback();
                        return fail(resource_change_phase::prepare, resource_change_row_kind::buffer_create,
                                    index, created.error);
                    }
                    native.slice = {.buffer = native.handle, .offset = 0, .size = row.desc.size};
                }
                prepared_buffers.push_back(native);
            }
            for (uint32_t index = 0; index < batch.image_creates.size(); ++index)
            {
                vk_image_resource_handle native;
                const auto created = state.runtime.create_image(batch.image_creates[index].desc, native);
                if (!created)
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::image_create,
                                index, created.error);
                }
                prepared_images.push_back({native, batch.image_creates[index].desc});
            }
            for (uint32_t index = 0; index < batch.sampler_creates.size(); ++index)
            {
                const auto& desc = batch.sampler_creates[index].desc;
                vk_bindless_handle native;
                const auto created = state.runtime.create_sampler({
                    .min_filter = desc.min_filter == sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                    .mag_filter = desc.mag_filter == sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                    .address_u = desc.address_u == sampler_address_mode::clamp_to_edge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                               : desc.address_u == sampler_address_mode::mirrored_repeat ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                                                        : VK_SAMPLER_ADDRESS_MODE_REPEAT,
                    .address_v = desc.address_v == sampler_address_mode::clamp_to_edge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                               : desc.address_v == sampler_address_mode::mirrored_repeat ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                                                        : VK_SAMPLER_ADDRESS_MODE_REPEAT,
                    .max_lod = desc.max_lod,
                }, native);
                if (!created)
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::sampler_create,
                                index, created.error);
                }
                prepared_samplers.push_back({native});
            }

            for (uint32_t index = 0; index < batch.bindless_publishes.size(); ++index)
            {
                const auto& row = batch.bindless_publishes[index];
                vk_bindless_handle native;
                vk_runtime_result published;
                bool owns_slot = true;
                if (row.table == bindless_table_kind::samplers)
                {
                    native = find_handle(state.samplers, row.sampler)->native.slot;
                    owns_slot = false;
                }
                else if (row.table == bindless_table_kind::sampled_images)
                {
                    const auto* image = find_handle(state.images, row.image);
                    published = state.runtime.allocate_sampled_image(
                        image->native.handle, lower_vk_format(image->native.desc.fmt), native);
                }
                else if (row.table == bindless_table_kind::storage_images)
                {
                    const auto* image = find_handle(state.images, row.image);
                    published = state.runtime.allocate_storage_image(
                        image->native.handle, lower_vk_format(image->native.desc.fmt), native);
                }
                else
                {
                    const auto* buffer = find_handle(state.buffers, row.buffer);
                    published = row.table == bindless_table_kind::uniform_buffers
                        ? state.runtime.allocate_uniform_buffer(buffer->native.handle,
                            buffer->native.slice.offset + row.offset, row.size, native)
                        : state.runtime.allocate_storage_buffer(buffer->native.handle,
                            buffer->native.slice.offset + row.offset, row.size, native);
                }
                if (!published)
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::bindless_publish,
                                index, published.error);
                }
                prepared_bindless.push_back({native, owns_slot});
            }

            for (uint32_t index = 0; index < batch.buffer_uploads.size(); ++index)
            {
                const auto& row = batch.buffer_uploads[index];
                if (row.bytes.empty()) continue;
                auto* destination = find_handle(state.buffers, row.destination);
                if (destination->native.desc.memory != memory_domain::device_local) continue;
                const auto native = destination->native.handle;
                const bool uploaded = state.runtime.stage_buffer_upload(
                    {native, destination->native.slice.offset + row.offset, row.bytes.size()}, row.bytes);
                if (!uploaded)
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::buffer_upload,
                                index, state.runtime.last_error());
                }
            }
            for (uint32_t index = 0; index < batch.image_uploads.size(); ++index)
            {
                const auto& row = batch.image_uploads[index];
                auto* destination = find_handle(state.images, row.destination);
                if (!state.runtime.stage_image_upload(destination->native.handle, row.mip_level, 0,
                        extent_3d{row.width, row.height, 1}, row.bytes))
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::image_upload,
                                index, state.runtime.last_error());
                }
            }
            for (uint32_t index = 0; index < batch.buffer_uploads.size(); ++index)
            {
                const auto& row = batch.buffer_uploads[index];
                if (row.bytes.empty()) continue;
                auto* destination = find_handle(state.buffers, row.destination);
                if (destination->native.desc.memory == memory_domain::device_local) continue;
                if (!state.runtime.update_buffer(destination->native.handle,
                        destination->native.slice.offset + row.offset, row.bytes))
                {
                    rollback();
                    return fail(resource_change_phase::prepare, resource_change_row_kind::buffer_upload,
                                index, state.runtime.last_error());
                }
            }

            resource_change_result output;
            for (auto& native : prepared_buffers)
                output.buffers.push_back(publish_handle<device_buffer_handle>(state.buffers, std::move(native)));
            for (auto& native : prepared_images)
                output.images.push_back(publish_handle<device_image_handle>(state.images, std::move(native)));
            for (auto& native : prepared_samplers)
                output.samplers.push_back(publish_handle<device_sampler_handle>(state.samplers, std::move(native)));
            for (auto& native : prepared_graphics_pipelines)
                output.graphics_pipelines.push_back(
                    publish_handle<device_pipeline_handle>(state.pipelines, std::move(native)));
            for (auto& native : prepared_compute_pipelines)
                output.compute_pipelines.push_back(
                    publish_handle<device_pipeline_handle>(state.pipelines, std::move(native)));
            for (auto& native : prepared_bindless)
            {
                output.bindless_slots.push_back(native.handle.index);
                output.bindless.push_back(publish_handle<device_bindless_handle>(state.bindless, std::move(native)));
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
                {
                    if (bindless->native.owns_slot)
                        state.runtime.release_bindless(bindless->native.handle, safe_after);
                    bindless->alive = false; ++bindless->generation;
                }
                if (auto* pipeline = find_handle(state.pipelines, row.pipeline))
                { pipeline->alive = false; ++pipeline->generation; }
            }
            state.statistics.pipeline_creations = state.runtime.pipelines().creations;
            state.statistics.descriptor_updates = state.runtime.bindless().statistics.descriptor_updates;
            return output;
        }

        bool rebuild_row_graph(device_state& state,
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
            state.frame_buffers.assign(state.current_plan->resources.size(), invalid_buffer);
            state.frame_images.assign(state.current_plan->resources.size(), invalid_image);

            const auto domain = [](pass_kind kind)
            {
                if (kind == pass_kind::copy) return pipeline_domain::copy;
                if (kind == pass_kind::compute) return pipeline_domain::compute;
                return pipeline_domain::graphics;
            };
            const auto ensure_buffer = [&](setup_context& context, uint32_t resource_index)
            {
                if (resource_index >= state.current_plan->resources.size())
                    throw std::runtime_error("Frame buffer resource index is out of range");
                auto& logical = state.frame_buffers[resource_index];
                if (logical != invalid_buffer) return std::pair{logical, false};
                const auto& resource = state.current_plan->resources[resource_index];
                if (resource.source == frame_resource_source::persistent_buffer)
                {
                    const auto* native = find_handle(state.buffers, resource.buffer);
                    if (!native) throw std::runtime_error("Frame buffer resource is stale");
                    auto desc = native->native.desc;
                    desc.lifetime = resource_lifetime_class::imported;
                    logical = context.import_buffer(std::string(resource.name), desc);
                }
                else if (resource.source == frame_resource_source::transient_buffer)
                {
                    logical = context.create_buffer(std::string(resource.name), resource.buffer_description);
                }
                else throw std::runtime_error("Frame resource is not a buffer");
                return std::pair{logical, true};
            };
            const auto ensure_image = [&](setup_context& context, uint32_t resource_index)
            {
                if (resource_index >= state.current_plan->resources.size())
                    throw std::runtime_error("Frame image resource index is out of range");
                auto& logical = state.frame_images[resource_index];
                if (logical != invalid_image) return std::pair{logical, false};
                const auto& resource = state.current_plan->resources[resource_index];
                if (resource.source == frame_resource_source::persistent_image)
                {
                    const auto* native = find_handle(state.images, resource.image);
                    if (!native) throw std::runtime_error("Frame image resource is stale");
                    auto desc = native->native.desc;
                    desc.lifetime = resource_lifetime_class::imported;
                    logical = context.import_image(std::string(resource.name), desc);
                }
                else if (resource.source == frame_resource_source::transient_image)
                {
                    logical = context.create_image(std::string(resource.name), resource.image_description);
                }
                else if (resource.source == frame_resource_source::swapchain_image)
                {
                    const image_desc desc{
                        .fmt = environment.color_format,
                        .extent = environment.extent,
                        .usage = image_usage::COLOR_ATTACHMENT | image_usage::PRESENT,
                        .memory = memory_domain::device_local,
                        .aliasing = aliasing_policy::forbidden,
                        .lifetime = resource_lifetime_class::imported,
                    };
                    logical = context.import_image(std::string(resource.name), desc);
                    const image_access_desc present{.usage = image_usage::PRESENT,
                                                    .domain = pipeline_domain::graphics};
                    const bool initialized = state.swapchain_initialized[image_index];
                    context.set_initial_state(logical,
                        initialized ? present : image_access_desc{.usage = image_usage::NONE,
                                                                   .domain = pipeline_domain::graphics},
                        access_type::read,
                        initialized ? contents_policy::preserve : contents_policy::discard);
                    context.set_final_state(logical, present, access_type::read);
                }
                else throw std::runtime_error("Frame resource is not an image");
                return std::pair{logical, true};
            };

            const buffer_desc upload_desc{
                .size = 64ull * 1024ull * 1024ull,
                .usage = buffer_usage::TRANSFER_SRC,
                .memory = memory_domain::upload,
                .mapping = mapping_policy::persistent,
                .aliasing = aliasing_policy::forbidden,
                .lifetime = resource_lifetime_class::imported,
            };
            graph.add_copy_pass("UploadPass", [&, upload_desc](setup_context& context)
            {
                state.upload_buffer = context.import_buffer("UploadArena", upload_desc);
                const buffer_access_desc source{.usage = buffer_usage::TRANSFER_SRC,
                                                .domain = pipeline_domain::copy};
                context.set_initial_state(state.upload_buffer, source, access_type::read,
                                          contents_policy::preserve);
                context.read_buffer(state.upload_buffer, source);
                for (uint32_t index = 0; index < state.current_plan->resources.size(); ++index)
                {
                    const auto& resource = state.current_plan->resources[index];
                    if (resource.source != frame_resource_source::persistent_buffer) continue;
                    const auto* native = find_handle(state.buffers, resource.buffer);
                    if (!native || (native->native.desc.usage & buffer_usage::TRANSFER_DST) == buffer_usage::NONE)
                        continue;
                    const auto [logical, created] = ensure_buffer(context, index);
                    const buffer_access_desc destination{.usage = buffer_usage::TRANSFER_DST,
                                                         .domain = pipeline_domain::copy};
                    if (created)
                        context.set_initial_state(logical, destination, access_type::write,
                                                  contents_policy::preserve);
                    context.write_buffer(logical, destination);
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

            for (uint32_t pass_index = 0; pass_index < state.current_plan->passes.size(); ++pass_index)
            {
                const auto make_setup = [&, pass_index]
                {
                    return [&, pass_index](setup_context& context)
                    {
                        const auto& pass = state.current_plan->passes[pass_index];
                        context.set_queue(pass.queue);
                        const auto pass_domain = domain(pass.kind);
                        for (uint32_t row_index = pass.buffer_accesses.begin;
                             row_index < pass.buffer_accesses.begin + pass.buffer_accesses.count; ++row_index)
                        {
                            const auto& row = state.current_plan->buffer_accesses[row_index];
                            const auto [logical, created] = ensure_buffer(context, row.resource.index);
                            const buffer_access_desc access{.usage = row.usage, .domain = pass_domain,
                                                            .queue = pass.queue, .bytes = row.range};
                            const auto& resource = state.current_plan->resources[row.resource.index];
                            if (created && resource.source == frame_resource_source::persistent_buffer)
                                context.set_initial_state(logical, access, row.access, contents_policy::preserve);
                            if (row.access == access_type::read) context.read_buffer(logical, access);
                            else if (row.access == access_type::write) context.write_buffer(logical, access);
                            else context.read_write_buffer(logical, access);
                        }
                        for (uint32_t row_index = pass.image_accesses.begin;
                             row_index < pass.image_accesses.begin + pass.image_accesses.count; ++row_index)
                        {
                            const auto& row = state.current_plan->image_accesses[row_index];
                            const auto [logical, created] = ensure_image(context, row.resource.index);
                            const image_access_desc access{.usage = row.usage, .domain = pass_domain,
                                                           .queue = pass.queue, .subresource = row.range};
                            const auto& resource = state.current_plan->resources[row.resource.index];
                            if (created && resource.source == frame_resource_source::persistent_image)
                                context.set_initial_state(logical, access, row.access, contents_policy::preserve);
                            if (row.access == access_type::read) context.read_image(logical, access);
                            else if (row.access == access_type::write) context.write_image(logical, access);
                            else context.read_write_image(logical, access);
                        }
                        if (pass.kind == pass_kind::raster)
                        {
                            context.set_render_area({.width = environment.extent.width,
                                                     .height = environment.extent.height});
                            for (uint32_t row_index = pass.attachments.begin;
                                 row_index < pass.attachments.begin + pass.attachments.count; ++row_index)
                            {
                                const auto& row = state.current_plan->attachments[row_index];
                                const auto [logical, created] = ensure_image(context, row.resource.index);
                                (void)created;
                                if (row.kind == frame_attachment_kind::color)
                                    context.add_color_attachment(logical, row.load, row.store, row.clear);
                                else context.set_depth_stencil_attachment(logical, row.load, row.store, row.clear);
                                context.declare_image_output(logical);
                            }
                        }
                    };
                };
                const auto make_execute = [&state, pass_index, extent = environment.extent]
                {
                    return [&state, pass_index, extent](execute_context& context)
                    {
                        const auto& pass = state.current_plan->passes[pass_index];
                        if (pass.kind == pass_kind::copy)
                        {
                            const auto rows = std::span(state.native_copies).subspan(
                                pass.buffer_copies.begin, pass.buffer_copies.count);
                            if (!state.runtime.record_buffer_copies(context.commands(), rows))
                                throw std::runtime_error(state.runtime.last_error());
                        }
                        else if (pass.kind == pass_kind::compute)
                        {
                            const auto rows = std::span(state.native_dispatches).subspan(
                                pass.dispatches.begin, pass.dispatches.count);
                            if (!state.runtime.record_dispatches({.commands = context.commands(),
                                    .push_constants = state.current_plan->push_constants, .rows = rows}))
                                throw std::runtime_error(state.runtime.last_error());
                        }
                        else
                        {
                            ++state.statistics.draw_pass_executions;
                            const auto rows = std::span(state.native_draws).subspan(
                                pass.indexed_indirect_draws.begin, pass.indexed_indirect_draws.count);
                            const auto push = state.current_plan->push_constants.subspan(
                                pass.push_constant_offset, pass.push_constant_size);
                            if (!state.runtime.record_indexed_indirect({
                                .commands = context.commands(),
                                .extent = {extent.width, extent.height},
                                .push_constants = push,
                                .push_stages = lower_stage_mask(pass.push_constant_stage_mask),
                                .rows = rows,
                            })) throw std::runtime_error(state.runtime.last_error());
                        }
                    };
                };
                const auto& pass = state.current_plan->passes[pass_index];
                if (pass.kind == pass_kind::raster)
                    graph.add_raster_pass(std::string(pass.name), make_setup(), make_execute());
                else if (pass.kind == pass_kind::copy)
                    graph.add_copy_pass(std::string(pass.name), make_setup(), make_execute());
                else graph.add_pass(std::string(pass.name), make_setup(), make_execute());
            }
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
            for (uint32_t index = 0; index < state.current_plan->resources.size(); ++index)
            {
                const auto& resource = state.current_plan->resources[index];
                if (resource.source == frame_resource_source::persistent_buffer)
                {
                    const auto* native = find_handle(state.buffers, resource.buffer);
                    if (!native) return false;
                    if (index >= state.frame_buffers.size() || state.frame_buffers[index] == invalid_buffer) continue;
                    state.graph->bind_imported_buffer(state.frame_buffers[index],
                        vk_native_buffer_range{state.runtime.buffer(native->native.handle),
                                               native->native.slice.offset,
                                               native->native.slice.size});
                }
                else if (resource.source == frame_resource_source::persistent_image)
                {
                    const auto* native = find_handle(state.images, resource.image);
                    if (!native) return false;
                    if (index >= state.frame_images.size() || state.frame_images[index] == invalid_image) continue;
                    state.graph->bind_imported_image(state.frame_images[index],
                        state.runtime.image(native->native.handle));
                }
                else if (resource.source == frame_resource_source::swapchain_image)
                {
                    if (index >= state.frame_images.size() || state.frame_images[index] == invalid_image) return false;
                    state.graph->bind_imported_image(state.frame_images[index],
                        state.runtime.swapchain_images().rows[image_index].image);
                }
            }
            return state.graph->execute(commands).succeeded();
        }

        vk_frame_status acquire_phase(device_state& state, vk_frame_token& token)
        {
            return state.runtime.acquire(token);
        }

        bool realize_resources_phase(device_state& state)
        {
            return state.runtime.realize_resources();
        }

        bool record_batches_phase(device_state& state, vk_frame_token& token)
        {
            return state.runtime.record_batches(token, &state, &record_graph);
        }

        bool submit_phase(device_state& state, const vk_frame_token& token)
        {
            return state.runtime.submit(token);
        }

        vk_frame_status present_phase(device_state& state, const vk_frame_token& token)
        {
            return state.runtime.present(token);
        }

        void collect_retired_phase(device_state& state)
        {
            state.runtime.collect_retired();
        }

        constexpr vulkan_frame_phase_table frame_phases{
            .acquire = &acquire_phase,
            .realize_resources = &realize_resources_phase,
            .record_batches = &record_batches_phase,
            .submit = &submit_phase,
            .present = &present_phase,
            .collect_retired = &collect_retired_phase,
        };

        frame_result render_frame(void* value, const frame_recipe& recipe)
        {
            auto& state = *static_cast<device_state*>(value);
            if (state.shutdown) return {.error = "Vulkan render device is shut down"};
            if (recipe.build == nullptr) return {.error = "Frame recipe has no builder"};
            if (state.resize_requested)
            {
                const auto resized = state.runtime.resize();
                if (!resized) return {.error = resized.error};
                if (resized.status == vk_resize_status::skipped)
                    return {.status = frame_status::skipped};
                state.swapchain_initialized.assign(state.runtime.swapchain_images().rows.size(), false);
                state.resize_requested = false;
            }
            vk_frame_token token;
            const auto acquired = frame_phases.acquire(state, token);
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
            if (plan.passes.empty()) return {.error = "Frame recipe contains no passes"};
            for (const auto& pass : plan.passes)
            {
                const auto valid_range = [](frame_row_range range, std::size_t size)
                { return range.begin <= size && range.count <= size - range.begin; };
                if (!valid_range(pass.buffer_accesses, plan.buffer_accesses.size()) ||
                    !valid_range(pass.image_accesses, plan.image_accesses.size()) ||
                    !valid_range(pass.attachments, plan.attachments.size()) ||
                    !valid_range(pass.buffer_copies, plan.buffer_copies.size()) ||
                    !valid_range(pass.dispatches, plan.dispatches.size()) ||
                    !valid_range(pass.indexed_indirect_draws, plan.indexed_indirect_draws.size()) ||
                    pass.push_constant_offset > plan.push_constants.size() ||
                    pass.push_constant_size > plan.push_constants.size() - pass.push_constant_offset)
                    return {.error = "Frame recipe contains an out-of-range pass row"};
                if ((pass.kind == pass_kind::raster &&
                     (pass.buffer_copies.count != 0 || pass.dispatches.count != 0)) ||
                    (pass.kind == pass_kind::compute &&
                     (pass.attachments.count != 0 || pass.buffer_copies.count != 0 ||
                      pass.indexed_indirect_draws.count != 0)) ||
                    (pass.kind == pass_kind::copy &&
                     (pass.attachments.count != 0 || pass.dispatches.count != 0 ||
                      pass.indexed_indirect_draws.count != 0)))
                    return {.error = "Frame pass contains commands incompatible with its pass kind"};
            }
            for (uint32_t index = 0; index < plan.resources.size(); ++index)
            {
                const auto& resource = plan.resources[index];
                if (resource.name.empty()) return {.error = "Frame resource name must not be empty"};
                if (resource.source == frame_resource_source::persistent_buffer &&
                    !find_handle(state.buffers, resource.buffer))
                    return {.error = "Frame recipe contains a stale persistent buffer resource"};
                if (resource.source == frame_resource_source::persistent_image &&
                    !find_handle(state.images, resource.image))
                    return {.error = "Frame recipe contains a stale persistent image resource"};
                if (resource.source == frame_resource_source::transient_buffer &&
                    !vk_backend::validate_buffer_desc(resource.buffer_description))
                    return {.error = "Frame recipe contains an invalid transient buffer description"};
                if (resource.source == frame_resource_source::transient_image &&
                    !vk_backend::validate_image_desc(resource.image_description))
                    return {.error = "Frame recipe contains an invalid transient image description"};
            }
            for (const auto& access : plan.image_accesses)
                if (access.resource.index >= plan.resources.size() ||
                    (plan.resources[access.resource.index].source != frame_resource_source::persistent_image &&
                     plan.resources[access.resource.index].source != frame_resource_source::transient_image &&
                     plan.resources[access.resource.index].source != frame_resource_source::swapchain_image))
                    return {.error = "Frame recipe contains an invalid image access resource"};
            for (const auto& attachment : plan.attachments)
                if (attachment.resource.index >= plan.resources.size() ||
                    (plan.resources[attachment.resource.index].source != frame_resource_source::persistent_image &&
                     plan.resources[attachment.resource.index].source != frame_resource_source::transient_image &&
                     plan.resources[attachment.resource.index].source != frame_resource_source::swapchain_image))
                    return {.error = "Frame recipe contains an invalid attachment resource"};
            state.current_plan = &plan;
            state.native_draws.clear();
            state.native_copies.clear();
            state.native_dispatches.clear();
            uint64_t cache_key = hash_combine(plan.cache_key,
                (static_cast<uint64_t>(swapchain.extent.width) << 32) | swapchain.extent.height);
            cache_key = hash_combine(cache_key, static_cast<uint64_t>(swapchain.format));
            cache_key = hash_combine(cache_key, state.swapchain_initialized[token.image_index]);
            for (const auto& resource : plan.resources)
            {
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(resource.source));
                for (const char value : resource.name)
                    cache_key = hash_combine(cache_key, static_cast<uint8_t>(value));
                if (resource.source == frame_resource_source::persistent_buffer)
                    cache_key = hash_combine(cache_key,
                        (static_cast<uint64_t>(resource.buffer.index) << 32) | resource.buffer.generation);
                else if (resource.source == frame_resource_source::persistent_image)
                    cache_key = hash_combine(cache_key,
                        (static_cast<uint64_t>(resource.image.index) << 32) | resource.image.generation);
                else if (resource.source == frame_resource_source::transient_buffer)
                {
                    cache_key = hash_combine(cache_key, resource.buffer_description.size);
                    cache_key = hash_combine(cache_key, static_cast<uint64_t>(resource.buffer_description.usage));
                    cache_key = hash_combine(cache_key, static_cast<uint64_t>(resource.buffer_description.memory));
                }
                else if (resource.source == frame_resource_source::transient_image)
                {
                    cache_key = hash_combine(cache_key, static_cast<uint64_t>(resource.image_description.fmt));
                    cache_key = hash_combine(cache_key,
                        (static_cast<uint64_t>(resource.image_description.extent.width) << 32) |
                        resource.image_description.extent.height);
                    cache_key = hash_combine(cache_key, static_cast<uint64_t>(resource.image_description.usage));
                }
            }
            for (const auto& pass : plan.passes)
            {
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(pass.kind));
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(pass.queue));
                for (const char value : pass.name)
                    cache_key = hash_combine(cache_key, static_cast<uint8_t>(value));
                cache_key = hash_combine(cache_key,
                    (static_cast<uint64_t>(pass.buffer_accesses.begin) << 32) | pass.buffer_accesses.count);
                cache_key = hash_combine(cache_key,
                    (static_cast<uint64_t>(pass.image_accesses.begin) << 32) | pass.image_accesses.count);
                cache_key = hash_combine(cache_key,
                    (static_cast<uint64_t>(pass.attachments.begin) << 32) | pass.attachments.count);
            }
            for (const auto& attachment : plan.attachments)
            {
                cache_key = hash_combine(cache_key, attachment.resource.index);
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(attachment.kind));
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(attachment.load));
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(attachment.store));
            }
            for (const auto& draw : plan.indexed_indirect_draws)
            {
                const auto* pipeline = find_handle(state.pipelines, draw.pipeline);
                const auto* vertex = find_handle(state.buffers, draw.vertex_buffer);
                const auto* index = find_handle(state.buffers, draw.index_buffer);
                const auto* indirect = find_handle(state.buffers, draw.indirect_buffer);
                if (!pipeline || !vertex || !index || !indirect)
                { state.current_plan = nullptr; return {.error = "Frame recipe references a stale draw handle"}; }
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
            for (const auto& copy : plan.buffer_copies)
            {
                const auto* source = find_handle(state.buffers, copy.source);
                const auto* destination = find_handle(state.buffers, copy.destination);
                if (!source || !destination || copy.size == 0 ||
                    copy.source_offset > source->native.desc.size ||
                    copy.size > source->native.desc.size - copy.source_offset ||
                    copy.destination_offset > destination->native.desc.size ||
                    copy.size > destination->native.desc.size - copy.destination_offset)
                {
                    state.current_plan = nullptr;
                    return {.error = "Frame recipe contains an invalid buffer copy range"};
                }
                cache_key = hash_combine(cache_key, (static_cast<uint64_t>(copy.source.index) << 32) |
                                                     copy.source.generation);
                cache_key = hash_combine(cache_key, (static_cast<uint64_t>(copy.destination.index) << 32) |
                                                     copy.destination.generation);
                state.native_copies.push_back({
                    .source = state.runtime.buffer(source->native.handle),
                    .destination = state.runtime.buffer(destination->native.handle),
                    .source_offset = source->native.slice.offset + copy.source_offset,
                    .destination_offset = destination->native.slice.offset + copy.destination_offset,
                    .size = copy.size,
                });
            }
            for (const auto& access : plan.buffer_accesses)
            {
                if (access.resource.index >= plan.resources.size())
                {
                    state.current_plan = nullptr;
                    return {.error = "Frame recipe contains an invalid buffer resource index"};
                }
                const auto& resource = plan.resources[access.resource.index];
                if (resource.source != frame_resource_source::persistent_buffer &&
                    resource.source != frame_resource_source::transient_buffer)
                {
                    state.current_plan = nullptr;
                    return {.error = "Frame recipe contains a non-buffer access resource"};
                }
                if (resource.source == frame_resource_source::persistent_buffer)
                    cache_key = hash_combine(cache_key, (static_cast<uint64_t>(resource.buffer.index) << 32) |
                                                         resource.buffer.generation);
                else cache_key = hash_combine(cache_key, access.resource.index);
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(access.usage));
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(access.access));
                cache_key = hash_combine(cache_key, access.range.offset);
                cache_key = hash_combine(cache_key, access.range.size);
            }
            for (const auto& access : plan.image_accesses)
            {
                cache_key = hash_combine(cache_key, access.resource.index);
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(access.usage));
                cache_key = hash_combine(cache_key, static_cast<uint64_t>(access.access));
                cache_key = hash_combine(cache_key, access.range.base_mip_level);
                cache_key = hash_combine(cache_key, access.range.mip_level_count);
                cache_key = hash_combine(cache_key, access.range.base_array_layer);
                cache_key = hash_combine(cache_key, access.range.array_layer_count);
            }
            for (const auto& dispatch : plan.dispatches)
            {
                const auto* pipeline = find_handle(state.pipelines, dispatch.pipeline);
                if (!pipeline || dispatch.x == 0 || dispatch.y == 0 || dispatch.z == 0 ||
                    dispatch.push_constant_offset > plan.push_constants.size() ||
                    dispatch.push_constant_size > plan.push_constants.size() - dispatch.push_constant_offset)
                {
                    state.current_plan = nullptr;
                    return {.error = "Frame recipe contains an invalid compute dispatch"};
                }
                state.native_dispatches.push_back({
                    .pipeline = pipeline->native.handle,
                    .x = dispatch.x,
                    .y = dispatch.y,
                    .z = dispatch.z,
                    .push_constant_offset = dispatch.push_constant_offset,
                    .push_constant_size = dispatch.push_constant_size,
                    .push_stages = lower_stage_mask(dispatch.push_constant_stage_mask),
                });
            }
            if (!rebuild_row_graph(state, environment, token.image_index, cache_key) ||
                !frame_phases.realize_resources(state) ||
                !frame_phases.record_batches(state, token))
            {
                state.graph->abort_frame();
                state.current_plan = nullptr;
                return {.error = state.runtime.last_error().empty() ? "Render graph execution failed"
                                                                    : state.runtime.last_error()};
            }
            if (!frame_phases.submit(state, token))
            {
                state.graph->abort_frame();
                state.current_plan = nullptr;
                return {.error = state.runtime.last_error()};
            }
            state.runtime.commit_pending_uploads(state.runtime.frames().next_submission - 1);
            if (!state.graph->commit_frame())
            { state.current_plan = nullptr; return {.error = "Render graph frame commit failed"}; }
            const auto presented = frame_phases.present(state, token);
            frame_phases.collect_retired(state);
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
            .diagnostics = config.diagnostics,
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
        state->graph->set_resource_validation({
            .validate_image = [](void*, const image_desc& desc) { return vk_backend::validate_image_desc(desc); },
            .validate_buffer = [](void*, const buffer_desc& desc) { return vk_backend::validate_buffer_desc(desc); },
        });
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
