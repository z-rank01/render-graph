// debug_dump implementation: deterministic JSON serialization of a compiled
// graph plan. Hand-rolled writer keeps Core dependency-free; every iteration
// follows schedule/CSR order and edges are sorted + deduplicated, so output
// is byte-stable for a given plan.
#include "render_graph/debug_dump.h"

#include <cstdint>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace render_graph
{
    namespace
    {
        constexpr uint32_t invalid_schedule = 0xffffffffU;

        // Compact JSON writer: no whitespace, integers and strings only — no
        // locale-dependent float formatting anywhere in the dump.
        struct json_writer
        {
            std::string out;

            void raw(std::string_view text) { out += text; }
            void character(char value) { out += value; }
            void null() { raw("null"); }
            void boolean(bool value) { raw(value ? "true" : "false"); }
            void number(uint64_t value) { out += std::to_string(value); }

            void string(std::string_view value)
            {
                out += '"';
                for (const char c : value)
                {
                    switch (c)
                    {
                    case '"': raw("\\\""); break;
                    case '\\': raw("\\\\"); break;
                    case '\b': raw("\\b"); break;
                    case '\f': raw("\\f"); break;
                    case '\n': raw("\\n"); break;
                    case '\r': raw("\\r"); break;
                    case '\t': raw("\\t"); break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20U)
                        {
                            constexpr char digits[] = "0123456789abcdef";
                            const auto byte = static_cast<unsigned char>(c);
                            raw("\\u00");
                            character(digits[(byte >> 4U) & 0x0FU]);
                            character(digits[byte & 0x0FU]);
                        }
                        else
                        {
                            character(c);
                        }
                    }
                }
                out += '"';
            }

            // {"key": written by caller} helpers: key with colon prefix.
            void key(std::string_view name)
            {
                string(name);
                character(':');
            }

            // Comma separator driven by an open section cursor.
            void separate(bool& first)
            {
                if (!first) character(',');
                first = false;
            }
        };

        std::string_view pass_kind_name(pass_kind kind)
        {
            switch (kind)
            {
            case pass_kind::raster: return "raster";
            case pass_kind::compute: return "compute";
            case pass_kind::copy: return "copy";
            }
            return "unknown";
        }

        std::string_view queue_name(queue_class value)
        {
            switch (value)
            {
            case queue_class::graphics: return "graphics";
            case queue_class::compute: return "compute";
            case queue_class::copy: return "copy";
            }
            return "unknown";
        }

        std::string_view access_name(access_type value)
        {
            switch (value)
            {
            case access_type::read: return "read";
            case access_type::write: return "write";
            case access_type::read_write: return "read_write";
            }
            return "unknown";
        }

        std::string_view domain_name(pipeline_domain value)
        {
            switch (value)
            {
            case pipeline_domain::any: return "any";
            case pipeline_domain::graphics: return "graphics";
            case pipeline_domain::compute: return "compute";
            case pipeline_domain::copy: return "copy";
            }
            return "unknown";
        }

        std::string_view phase_name(synchronization_phase value)
        {
            switch (value)
            {
            case synchronization_phase::full: return "full";
            case synchronization_phase::release: return "release";
            case synchronization_phase::acquire: return "acquire";
            }
            return "unknown";
        }

        std::string_view load_op_name(attachment_load_op value)
        {
            switch (value)
            {
            case attachment_load_op::load: return "load";
            case attachment_load_op::clear: return "clear";
            case attachment_load_op::dont_care: return "dont_care";
            }
            return "unknown";
        }

        std::string_view store_op_name(attachment_store_op value)
        {
            switch (value)
            {
            case attachment_store_op::store: return "store";
            case attachment_store_op::dont_care: return "dont_care";
            }
            return "unknown";
        }

        void image_usage_bits(json_writer& writer, uint32_t bits)
        {
            static constexpr std::pair<uint32_t, std::string_view> names[]{
                {static_cast<uint32_t>(image_usage::TRANSFER_SRC), "TRANSFER_SRC"},
                {static_cast<uint32_t>(image_usage::TRANSFER_DST), "TRANSFER_DST"},
                {static_cast<uint32_t>(image_usage::SAMPLED), "SAMPLED"},
                {static_cast<uint32_t>(image_usage::STORAGE), "STORAGE"},
                {static_cast<uint32_t>(image_usage::COLOR_ATTACHMENT), "COLOR_ATTACHMENT"},
                {static_cast<uint32_t>(image_usage::DEPTH_STENCIL_ATTACHMENT), "DEPTH_STENCIL_ATTACHMENT"},
                {static_cast<uint32_t>(image_usage::PRESENT), "PRESENT"},
            };
            writer.character('[');
            bool first = true;
            for (const auto& [bit, name] : names)
            {
                if ((bits & bit) == 0) continue;
                writer.separate(first);
                writer.string(name);
            }
            writer.character(']');
        }

        void buffer_usage_bits(json_writer& writer, uint32_t bits)
        {
            static constexpr std::pair<uint32_t, std::string_view> names[]{
                {static_cast<uint32_t>(buffer_usage::TRANSFER_SRC), "TRANSFER_SRC"},
                {static_cast<uint32_t>(buffer_usage::TRANSFER_DST), "TRANSFER_DST"},
                {static_cast<uint32_t>(buffer_usage::UNIFORM_BUFFER), "UNIFORM_BUFFER"},
                {static_cast<uint32_t>(buffer_usage::STORAGE_BUFFER), "STORAGE_BUFFER"},
                {static_cast<uint32_t>(buffer_usage::INDEX_BUFFER), "INDEX_BUFFER"},
                {static_cast<uint32_t>(buffer_usage::VERTEX_BUFFER), "VERTEX_BUFFER"},
                {static_cast<uint32_t>(buffer_usage::INDIRECT_BUFFER), "INDIRECT_BUFFER"},
            };
            writer.character('[');
            bool first = true;
            for (const auto& [bit, name] : names)
            {
                if ((bits & bit) == 0) continue;
                writer.separate(first);
                writer.string(name);
            }
            writer.character(']');
        }

        std::string_view resource_name(const std::vector<std::string>& names, uint32_t index)
        {
            return index < names.size() ? std::string_view(names[index]) : std::string_view("?");
        }

        // Pass handle -> schedule index; invalid/unscheduled -> JSON null.
        void pass_reference(json_writer& writer, pass_handle handle, const std::vector<uint32_t>& schedule_of)
        {
            if (handle != invalid_pass && handle.index() < schedule_of.size() &&
                schedule_of[handle.index()] != invalid_schedule)
            {
                writer.number(schedule_of[handle.index()]);
                return;
            }
            writer.null();
        }

        void intent_flags(json_writer& writer, synchronization_intent intents)
        {
            static constexpr std::pair<synchronization_intent, std::string_view> names[]{
                {synchronization_intent::layout_transition, "layout_transition"},
                {synchronization_intent::execution_dependency, "execution_dependency"},
                {synchronization_intent::memory_dependency, "memory_dependency"},
                {synchronization_intent::queue_ownership, "queue_ownership"},
                {synchronization_intent::aliasing, "aliasing"},
            };
            writer.character('[');
            bool first = true;
            for (const auto& [flag, name] : names)
            {
                if (!has_intent(intents, flag)) continue;
                writer.separate(first);
                writer.string(name);
            }
            writer.character(']');
        }

        void state_object(json_writer& writer, uint32_t usage_bits, access_type access,
                          pipeline_domain domain, queue_class queue, bool is_image)
        {
            writer.character('{');
            writer.key("usage");
            if (is_image) image_usage_bits(writer, usage_bits);
            else buffer_usage_bits(writer, usage_bits);
            writer.character(',');
            writer.key("access");
            writer.string(access_name(access));
            writer.character(',');
            writer.key("domain");
            writer.string(domain_name(domain));
            writer.character(',');
            writer.key("queue");
            writer.string(queue_name(queue));
            writer.character('}');
        }

        // One barrier object per op row, in CSR row order (per-pass prologue
        // segments followed by the graph epilogue segment).
        template <typename RangeDesc, typename Handle, typename PhysicalId>
        void dump_barrier_rows(json_writer& writer,
                               const synchronization_op_table<RangeDesc, Handle, PhysicalId>& table,
                               const std::vector<std::string>& names,
                               std::string_view kind_name,
                               bool is_image,
                               const std::vector<uint32_t>& schedule_of,
                               bool& first)
        {
            const auto& segments = table.segments;
            for (uint32_t row = 0; row < table.size(); ++row)
            {
                writer.separate(first);
                writer.character('{');
                writer.key("pass");
                pass_reference(writer, table.passes[row], schedule_of);
                writer.character(',');
                writer.key("scope");
                const bool epilogue = row >= segments.epilogue_begin &&
                                      row < segments.epilogue_begin + segments.epilogue_length;
                writer.string(epilogue ? "epilogue" : "prologue");
                writer.character(',');
                writer.key("kind");
                writer.string(kind_name);
                writer.character(',');
                writer.key("resource");
                writer.string(resource_name(names, table.logicals[row].index()));
                writer.character(',');
                writer.key("phase");
                writer.string(phase_name(table.phases[row]));
                writer.character(',');
                writer.key("intents");
                intent_flags(writer, table.intents[row]);
                writer.character(',');
                writer.key("producer");
                pass_reference(writer, table.source_passes[row], schedule_of);
                writer.character(',');
                writer.key("before");
                state_object(writer, table.before_usage_bits[row], table.before_accesses[row],
                             table.before_domains[row], table.before_queues[row], is_image);
                writer.character(',');
                writer.key("after");
                state_object(writer, table.after_usage_bits[row], table.after_accesses[row],
                             table.after_domains[row], table.after_queues[row], is_image);
                writer.character('}');
            }
        }

        template <typename Handle>
        void collect_sync_edges(const std::vector<Handle>& /*logicals*/,
                                const std::vector<pass_handle>& passes,
                                const std::vector<pass_handle>& source_passes,
                                const std::vector<uint32_t>& schedule_of,
                                std::set<std::pair<uint32_t, uint32_t>>& edges)
        {
            for (uint32_t row = 0; row < passes.size(); ++row)
            {
                const pass_handle source = source_passes[row];
                const pass_handle destination = passes[row];
                if (source == invalid_pass || destination == invalid_pass || source == destination) continue;
                if (source.index() >= schedule_of.size() || destination.index() >= schedule_of.size()) continue;
                if (schedule_of[source.index()] == invalid_schedule ||
                    schedule_of[destination.index()] == invalid_schedule) continue;
                edges.emplace(schedule_of[source.index()], schedule_of[destination.index()]);
            }
        }

        template <typename Handle>
        void collect_queue_edges(const std::vector<cross_queue_dependency<Handle>>& dependencies,
                                 const std::vector<uint32_t>& schedule_of,
                                 std::set<std::pair<uint32_t, uint32_t>>& queue_edges)
        {
            for (const auto& dependency : dependencies)
            {
                const pass_handle source = dependency.source_pass;
                const pass_handle destination = dependency.destination_pass;
                if (source == invalid_pass || destination == invalid_pass || source == destination) continue;
                if (source.index() >= schedule_of.size() || destination.index() >= schedule_of.size()) continue;
                if (schedule_of[source.index()] == invalid_schedule ||
                    schedule_of[destination.index()] == invalid_schedule) continue;
                queue_edges.emplace(schedule_of[source.index()], schedule_of[destination.index()]);
            }
        }

        template <typename Handle>
        void dump_alias_rows(json_writer& writer,
                             const std::vector<physical_resource_meta::alias_handoff<Handle>>& handoffs,
                             const std::vector<std::string>& names,
                             std::string_view kind_name,
                             const std::vector<uint32_t>& schedule_of,
                             bool& first)
        {
            for (const auto& handoff : handoffs)
            {
                writer.separate(first);
                writer.character('{');
                writer.key("kind");
                writer.string(kind_name);
                writer.character(',');
                writer.key("previous");
                writer.string(resource_name(names, handoff.previous.index()));
                writer.character(',');
                writer.key("next");
                writer.string(resource_name(names, handoff.next.index()));
                writer.character(',');
                writer.key("memory_block");
                if (handoff.memory_block != invalid_memory_block_id) writer.number(handoff.memory_block.index());
                else writer.null();
                writer.character(',');
                writer.key("at_pass");
                pass_reference(writer, handoff.at_pass, schedule_of);
                writer.character('}');
            }
        }
    } // namespace

    std::string debug_dump(const compiled_graph_plan& plan)
    {
        // Pass handle -> schedule position for every reference remapping below.
        std::vector<uint32_t> schedule_of(plan.passes.size(), invalid_schedule);
        for (uint32_t index = 0; index < plan.scheduled_passes.size(); ++index)
        {
            const pass_handle handle = plan.scheduled_passes[index];
            if (handle.index() < schedule_of.size()) schedule_of[handle.index()] = index;
        }

        json_writer writer;
        writer.character('{');

        // --- passes: schedule order, raster attachments from the pass CSR ---
        writer.key("passes");
        writer.character('[');
        {
            bool first = true;
            for (uint32_t index = 0; index < plan.scheduled_passes.size(); ++index)
            {
                const uint32_t pass = plan.scheduled_passes[index].index();
                if (pass >= plan.passes.size()) continue;
                writer.separate(first);
                writer.character('{');
                writer.key("index");
                writer.number(index);
                writer.character(',');
                writer.key("name");
                writer.string(plan.passes.names[pass]);
                writer.character(',');
                writer.key("kind");
                writer.string(pass_kind_name(plan.passes.kinds[pass]));
                writer.character(',');
                writer.key("queue");
                writer.string(queue_name(plan.passes.queues[pass]));
                writer.character(',');
                writer.key("flags");
                writer.character('[');
                {
                    bool first_flag = true;
                    if (plan.passes.is_backend_upload(pass))
                    {
                        writer.separate(first_flag);
                        writer.string("backend_upload");
                    }
                    if (plan.passes.is_side_effect(pass))
                    {
                        writer.separate(first_flag);
                        writer.string("side_effect");
                    }
                }
                writer.character(']');
                writer.character(',');
                writer.key("colors");
                writer.character('[');
                {
                    bool first_color = true;
                    const uint32_t begin = plan.passes.color_begins[pass];
                    const uint32_t end = begin + plan.passes.color_counts[pass];
                    for (uint32_t row = begin; row < end; ++row)
                    {
                        const raster_attachment& attachment = plan.passes.colors[row];
                        writer.separate(first_color);
                        writer.character('{');
                        writer.key("resource");
                        writer.string(resource_name(plan.resources.image_metas.names, attachment.image.index()));
                        writer.character(',');
                        writer.key("load");
                        writer.string(load_op_name(attachment.load));
                        writer.character(',');
                        writer.key("store");
                        writer.string(store_op_name(attachment.store));
                        writer.character('}');
                    }
                }
                writer.character(']');
                writer.character(',');
                writer.key("depth");
                if (plan.passes.depth_indices[pass] != invalid_depth_index)
                {
                    const raster_attachment& attachment = plan.passes.depths[plan.passes.depth_indices[pass]];
                    writer.character('{');
                    writer.key("resource");
                    writer.string(resource_name(plan.resources.image_metas.names, attachment.image.index()));
                    writer.character(',');
                    writer.key("load");
                    writer.string(load_op_name(attachment.load));
                    writer.character(',');
                    writer.key("store");
                    writer.string(store_op_name(attachment.store));
                    writer.character('}');
                }
                else
                {
                    writer.null();
                }
                writer.character('}');
            }
        }
        writer.character(']');
        writer.character(',');

        // --- edges: derived from sync ops (producer -> consumer); pairs that
        //     also appear as cross-queue dependencies are tagged cross_queue ---
        std::set<std::pair<uint32_t, uint32_t>> sync_edges;
        collect_sync_edges(plan.synchronization.image.logicals, plan.synchronization.image.passes,
                           plan.synchronization.image.source_passes, schedule_of, sync_edges);
        collect_sync_edges(plan.synchronization.buffer.logicals, plan.synchronization.buffer.passes,
                           plan.synchronization.buffer.source_passes, schedule_of, sync_edges);
        std::set<std::pair<uint32_t, uint32_t>> queue_edges;
        collect_queue_edges(plan.submissions.image_cross_queue_dependencies, schedule_of, queue_edges);
        collect_queue_edges(plan.submissions.buffer_cross_queue_dependencies, schedule_of, queue_edges);
        writer.key("edges");
        writer.character('[');
        {
            bool first = true;
            for (const auto& [from, to] : sync_edges)
            {
                writer.separate(first);
                writer.character('{');
                writer.key("from");
                writer.number(from);
                writer.character(',');
                writer.key("to");
                writer.number(to);
                writer.character(',');
                writer.key("kind");
                writer.string(queue_edges.count({from, to}) != 0 ? "cross_queue" : "sync");
                writer.character('}');
            }
        }
        writer.character(']');
        writer.character(',');

        // --- barriers: image table then buffer table, CSR row order ---
        writer.key("barriers");
        writer.character('[');
        {
            bool first = true;
            dump_barrier_rows(writer, plan.synchronization.image, plan.resources.image_metas.names,
                              "image", true, schedule_of, first);
            dump_barrier_rows(writer, plan.synchronization.buffer, plan.resources.buffer_metas.names,
                              "buffer", false, schedule_of, first);
        }
        writer.character(']');
        writer.character(',');

        // --- aliases: memory aliasing handoffs, image then buffer ---
        writer.key("aliases");
        writer.character('[');
        {
            bool first = true;
            dump_alias_rows(writer, plan.physical_resources.image_alias_handoffs,
                            plan.resources.image_metas.names, "image", schedule_of, first);
            dump_alias_rows(writer, plan.physical_resources.buffer_alias_handoffs,
                            plan.resources.buffer_metas.names, "buffer", schedule_of, first);
        }
        writer.character(']');
        writer.character(',');

        // --- resources: images then buffers, lifetimes/physical remapped ---
        writer.key("resources");
        writer.character('[');
        {
            bool first = true;
            const auto dump_resource = [&](std::string_view kind_name, const std::string& name, bool imported,
                                           pass_handle first_used, pass_handle last_used, uint32_t physical,
                                           uint32_t memory_block)
            {
                writer.separate(first);
                writer.character('{');
                writer.key("name");
                writer.string(name);
                writer.character(',');
                writer.key("kind");
                writer.string(kind_name);
                writer.character(',');
                writer.key("imported");
                writer.boolean(imported);
                writer.character(',');
                writer.key("first_pass");
                pass_reference(writer, first_used, schedule_of);
                writer.character(',');
                writer.key("last_pass");
                pass_reference(writer, last_used, schedule_of);
                writer.character(',');
                writer.key("physical");
                if (physical != invalid_schedule) writer.number(physical);
                else writer.null();
                writer.character(',');
                writer.key("memory_block");
                if (memory_block != invalid_schedule) writer.number(memory_block);
                else writer.null();
                writer.character('}');
            };
            const auto physical_index = [](const auto& column, uint32_t handle)
            {
                return handle < column.size() && column[handle].index() != std::numeric_limits<uint32_t>::max()
                           ? column[handle].index()
                           : invalid_schedule;
            };
            for (uint32_t handle = 0; handle < plan.resources.image_metas.names.size(); ++handle)
            {
                dump_resource("image", plan.resources.image_metas.names[handle],
                              plan.resources.image_metas.is_imported[handle] != 0,
                              handle < plan.lifetimes.image_first_used_pass.size()
                                  ? plan.lifetimes.image_first_used_pass[handle]
                                  : invalid_pass,
                              handle < plan.lifetimes.image_last_used_pass.size()
                                  ? plan.lifetimes.image_last_used_pass[handle]
                                  : invalid_pass,
                              physical_index(plan.physical_resources.handle_to_physical_img_id, handle),
                              physical_index(plan.physical_resources.handle_to_image_memory_block, handle));
            }
            for (uint32_t handle = 0; handle < plan.resources.buffer_metas.names.size(); ++handle)
            {
                dump_resource("buffer", plan.resources.buffer_metas.names[handle],
                              plan.resources.buffer_metas.is_imported[handle] != 0,
                              handle < plan.lifetimes.buffer_first_used_pass.size()
                                  ? plan.lifetimes.buffer_first_used_pass[handle]
                                  : invalid_pass,
                              handle < plan.lifetimes.buffer_last_used_pass.size()
                                  ? plan.lifetimes.buffer_last_used_pass[handle]
                                  : invalid_pass,
                              physical_index(plan.physical_resources.handle_to_physical_buf_id, handle),
                              physical_index(plan.physical_resources.handle_to_buffer_memory_block, handle));
            }
        }
        writer.character(']');
        writer.character(',');

        // --- statistics: compile counters verbatim ---
        writer.key("statistics");
        writer.character('{');
        {
            const render_graph_statistics& stats = plan.statistics;
            bool first = true;
            const auto field = [&](std::string_view name, uint32_t value)
            {
                writer.separate(first);
                writer.key(name);
                writer.number(value);
            };
            field("pass_count", stats.pass_count);
            field("active_pass_count", stats.active_pass_count);
            field("image_count", stats.image_count);
            field("buffer_count", stats.buffer_count);
            field("access_event_count", stats.access_event_count);
            field("synchronization_op_count", stats.synchronization_op_count);
            field("submission_batch_count", stats.submission_batch_count);
            field("image_memory_block_count", stats.image_memory_block_count);
            field("buffer_memory_block_count", stats.buffer_memory_block_count);
            field("culled_pass_count", stats.culled_pass_count);
        }
        writer.character('}');

        writer.character('}');
        return writer.out;
    }
} // namespace render_graph
