#pragma once

#include <algorithm>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "barrier.h"
#include "compile_result.h"
#include "graph.h"
#include "resource.h"
#include "rg_function.h"

namespace render_graph
{
    namespace unit_test
    {
        struct system_test_access;
    }

    template <typename BackendT>
    class render_graph_system
    {
    public:
        using backend_type = BackendT;
        using image_desc   = typename BackendT::image_desc;
        using buffer_desc  = typename BackendT::buffer_desc;
        using meta_table_t = resource_meta_table<image_desc, buffer_desc>;

        struct pass_setup_context
        {
        private:
            friend class render_graph_system<BackendT>;

            meta_table_t* meta_table;
            ordered_pass_accesses* ordered_accesses;
            output_table* output_table;
            pass_handle current_pass;

            pass_setup_context(meta_table_t* meta_table_in,
                               ordered_pass_accesses* ordered_accesses_in,
                               render_graph::output_table* output_table_in,
                               pass_handle current_pass_in)
                : meta_table(meta_table_in),
                  ordered_accesses(ordered_accesses_in),
                  output_table(output_table_in),
                  current_pass(current_pass_in)
            {
            }

        public:
            image_handle create_image(const image_desc& desc, bool imported = false, const std::string& name = {}) const
            {
                return create_image(desc,
                                    imported ? resource_lifetime_class::imported : resource_lifetime_class::transient,
                                    name);
            }

            image_handle create_image(const image_desc& desc,
                                      resource_lifetime_class lifetime,
                                      const std::string& name = {}) const
            {
                return meta_table->image_metas.add(name, desc, lifetime);
            }

            image_handle create_image(const std::string& name, const image_desc& desc, bool imported = false) const
            {
                return create_image(desc, imported, name);
            }

            image_handle create_image(const std::string& name,
                                      const image_desc& desc,
                                      resource_lifetime_class lifetime) const
            {
                return create_image(desc, lifetime, name);
            }

            buffer_handle create_buffer(const buffer_desc& desc, bool imported = false, const std::string& name = {}) const
            {
                return create_buffer(desc,
                                     imported ? resource_lifetime_class::imported : resource_lifetime_class::transient,
                                     name);
            }

            buffer_handle create_buffer(const buffer_desc& desc,
                                        resource_lifetime_class lifetime,
                                        const std::string& name = {}) const
            {
                return meta_table->buffer_metas.add(name, desc, lifetime);
            }

            buffer_handle create_buffer(const std::string& name, const buffer_desc& desc, bool imported = false) const
            {
                return create_buffer(desc, imported, name);
            }

            buffer_handle create_buffer(const std::string& name,
                                        const buffer_desc& desc,
                                        resource_lifetime_class lifetime) const
            {
                return create_buffer(desc, lifetime, name);
            }

            image_handle create_and_write_image(const image_desc& desc,
                                                image_usage usage,
                                                bool imported           = false,
                                                const std::string& name = {}) const
            {
                const auto resource = create_image(desc, imported, name);
                write_image(resource, usage);
                return resource;
            }

            image_handle create_and_write_image(const std::string& name, const image_desc& desc, image_usage usage, bool imported = false) const
            {
                return create_and_write_image(desc, usage, imported, name);
            }

            buffer_handle create_and_write_buffer(const buffer_desc& desc,
                                                  buffer_usage usage,
                                                  bool imported           = false,
                                                  const std::string& name = {}) const
            {
                const auto resource = create_buffer(desc, imported, name);
                write_buffer(resource, usage);
                return resource;
            }

            buffer_handle create_and_write_buffer(const std::string& name, const buffer_desc& desc, buffer_usage usage, bool imported = false) const
            {
                return create_and_write_buffer(desc, usage, imported, name);
            }

            void declare_image_output(image_handle resource) const
            {
                output_table->image_outputs.push_back(resource);
            }

            void declare_buffer_output(buffer_handle resource) const
            {
                output_table->buffer_outputs.push_back(resource);
            }

            void read_image(image_handle resource, const image_access_desc& state) const
            {
                ordered_accesses->events.push_back(pass_access_event{.resource = resource, .access = access_type::read, .state = state});
                ordered_accesses->lengths[current_pass]++;
            }

            void read_image(image_handle resource, image_usage usage) const
            {
                read_image(resource, image_access_desc{.usage = usage});
            }

            void read_buffer(buffer_handle resource, const buffer_access_desc& state) const
            {
                ordered_accesses->events.push_back(pass_access_event{.resource = resource, .access = access_type::read, .state = state});
                ordered_accesses->lengths[current_pass]++;
            }

            void read_buffer(buffer_handle resource, buffer_usage usage) const
            {
                read_buffer(resource, buffer_access_desc{.usage = usage});
            }

            void write_image(image_handle resource, const image_access_desc& state) const
            {
                ordered_accesses->events.push_back(pass_access_event{.resource = resource, .access = access_type::write, .state = state});
                ordered_accesses->lengths[current_pass]++;
            }

            void write_image(image_handle resource, image_usage usage) const
            {
                write_image(resource, image_access_desc{.usage = usage});
            }

            void write_buffer(buffer_handle resource, const buffer_access_desc& state) const
            {
                ordered_accesses->events.push_back(pass_access_event{.resource = resource, .access = access_type::write, .state = state});
                ordered_accesses->lengths[current_pass]++;
            }

            void write_buffer(buffer_handle resource, buffer_usage usage) const
            {
                write_buffer(resource, buffer_access_desc{.usage = usage});
            }
        };

        struct resource_access
        {
            const BackendT* backend = nullptr;

            [[nodiscard]] typename BackendT::native_image_handle image(image_handle logical) const { return backend->get_image(logical); }

            [[nodiscard]] typename BackendT::native_buffer_handle buffer(buffer_handle logical) const { return backend->get_buffer(logical); }
        };

        struct pass_execute_context
        {
            resource_access resources;
        };

        using pass_execute_func = rg_function<void(pass_execute_context&)>;
        using pass_setup_func   = rg_function<void(pass_setup_context&)>;

        struct graph_topology
        {
            std::vector<pass_handle> passes;
            std::vector<std::string> pass_names;
            std::vector<pass_setup_func> setup_funcs;
            std::vector<pass_execute_func> execute_funcs;
        };

        [[nodiscard]] const directed_acyclic_graph& get_dag() const { return dag; }
        [[nodiscard]] const std::vector<bool>& get_active_pass_flags() const { return active_pass_flags; }
        [[nodiscard]] const std::vector<pass_handle>& get_sorted_passes() const { return sorted_passes; }

        [[nodiscard]] const per_pass_barrier& get_per_pass_barriers() const { return per_pass_barriers; }
        [[nodiscard]] const physical_resource_meta& get_physical_resource_plan() const { return physical_resource_metas; }

        [[nodiscard]] resource_handle get_physical_image_id(image_handle logical) const
        {
            if (logical >= physical_resource_metas.handle_to_physical_img_id.size())
            {
                return invalid_resource;
            }
            return physical_resource_metas.handle_to_physical_img_id[logical];
        }

        [[nodiscard]] resource_handle get_physical_buffer_id(buffer_handle logical) const
        {
            if (logical >= physical_resource_metas.handle_to_physical_buf_id.size())
            {
                return invalid_resource;
            }
            return physical_resource_metas.handle_to_physical_buf_id[logical];
        }

        [[nodiscard]] resource_handle get_image_memory_block(image_handle logical) const
        {
            if (logical >= physical_resource_metas.handle_to_image_memory_block.size())
            {
                return invalid_resource;
            }
            return physical_resource_metas.handle_to_image_memory_block[logical];
        }

        [[nodiscard]] resource_handle get_buffer_memory_block(buffer_handle logical) const
        {
            if (logical >= physical_resource_metas.handle_to_buffer_memory_block.size())
            {
                return invalid_resource;
            }
            return physical_resource_metas.handle_to_buffer_memory_block[logical];
        }

        render_graph_system() = default;

        template <typename... Args>
            requires requires(BackendT& b, Args&&... args) { b.set_context(std::forward<Args>(args)...); }
        void set_backend_context(Args&&... args)
        {
            backend.set_context(std::forward<Args>(args)...);
        }

        void bind_imported_image(image_handle logical, typename BackendT::native_image_handle native)
        {
            backend.bind_imported_image(logical, native);
        }

        void bind_imported_buffer(buffer_handle logical, typename BackendT::native_buffer_handle native)
        {
            backend.bind_imported_buffer(logical, native);
        }

        // 1. Add Pass System
        // Separates resource definition (setup) from execution logic.

        template <typename SetupFn = pass_setup_func, typename ExecuteFn = pass_execute_func>
        pass_handle add_pass(SetupFn&& setup, ExecuteFn&& execute)
        {
            return add_pass("pass_" + std::to_string(graph.passes.size()), std::forward<SetupFn>(setup), std::forward<ExecuteFn>(execute));
        }

        template <typename SetupFn = pass_setup_func, typename ExecuteFn = pass_execute_func>
        pass_handle add_pass(const std::string& name, SetupFn&& setup, ExecuteFn&& execute)
        {
            auto handle = static_cast<pass_handle>(graph.passes.size());
            graph.passes.push_back(handle);
            graph.pass_names.push_back(name);
            graph.setup_funcs.push_back(std::forward<SetupFn>(setup));
            graph.execute_funcs.push_back(std::forward<ExecuteFn>(execute));
            return handle;
        }

        // 2. Compile System

        [[nodiscard]] compile_result compile()
        {
            reset_compiled_state();

            compile_result result;
            const auto pass_count = graph.passes.size();

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
            ordered_accesses.events.clear();
            ordered_accesses.begins.assign(pass_count, 0);
            ordered_accesses.lengths.assign(pass_count, 0);
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

            pass_setup_context setup_ctx(&meta_table, &ordered_accesses, &output_table, 0);
            for (size_t i = 0; i < pass_count; i++)
            {
                setup_ctx.current_pass = graph.passes[i];

                ordered_accesses.begins[setup_ctx.current_pass]   = static_cast<uint32_t>(ordered_accesses.events.size());

                auto setup_func = graph.setup_funcs[i];
                setup_func(setup_ctx);
            }

            // Derive the legacy read/write CSR views from the ordered source stream.
            // All compile logic that needs ordering consumes ordered_accesses directly.
            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                image_read_deps.begins[pass]   = static_cast<uint32_t>(image_read_deps.read_list.size());
                image_write_deps.begins[pass]  = static_cast<uint32_t>(image_write_deps.write_list.size());
                buffer_read_deps.begins[pass]  = static_cast<uint32_t>(buffer_read_deps.read_list.size());
                buffer_write_deps.begins[pass] = static_cast<uint32_t>(buffer_write_deps.write_list.size());

                const auto begin = ordered_accesses.begins[pass];
                const auto end   = begin + ordered_accesses.lengths[pass];
                for (auto event_index = begin; event_index < end; event_index++)
                {
                    const auto& event = ordered_accesses.events[event_index];
                    if (const auto* image = std::get_if<image_handle>(&event.resource))
                    {
                        const auto& state = std::get<image_access_desc>(event.state);
                        if (event.access == access_type::read)
                        {
                            image_read_deps.read_list.push_back(*image);
                            image_read_deps.usage_bits.push_back(static_cast<uint32_t>(state.usage));
                            image_read_deps.lengthes[pass]++;
                        }
                        else
                        {
                            image_write_deps.write_list.push_back(*image);
                            image_write_deps.usage_bits.push_back(static_cast<uint32_t>(state.usage));
                            image_write_deps.lengthes[pass]++;
                        }
                    }
                    else
                    {
                        const auto buffer = std::get<buffer_handle>(event.resource);
                        const auto& state = std::get<buffer_access_desc>(event.state);
                        if (event.access == access_type::read)
                        {
                            buffer_read_deps.read_list.push_back(buffer);
                            buffer_read_deps.usage_bits.push_back(static_cast<uint32_t>(state.usage));
                            buffer_read_deps.lengthes[pass]++;
                        }
                        else
                        {
                            buffer_write_deps.write_list.push_back(buffer);
                            buffer_write_deps.usage_bits.push_back(static_cast<uint32_t>(state.usage));
                            buffer_write_deps.lengthes[pass]++;
                        }
                    }
                }
            }

            const auto image_count  = meta_table.image_metas.names.size();
            const auto buffer_count = meta_table.buffer_metas.names.size();

            auto add_diagnostic = [&](compile_error_code code,
                                      pass_handle pass,
                                      resource_kind kind,
                                      resource_handle resource,
                                      const std::string& message)
            {
                compile_diagnostic diagnostic;
                diagnostic.code     = code;
                diagnostic.pass     = pass;
                diagnostic.kind     = kind;
                diagnostic.resource = resource;
                diagnostic.message  = message;

                if (pass < graph.pass_names.size())
                {
                    diagnostic.pass_name = graph.pass_names[pass];
                }
                if (kind == resource_kind::image && resource < image_count)
                {
                    diagnostic.resource_name = meta_table.image_metas.names[resource];
                }
                else if (kind == resource_kind::buffer && resource < buffer_count)
                {
                    diagnostic.resource_name = meta_table.buffer_metas.names[resource];
                }

                result.diagnostics.push_back(std::move(diagnostic));
            };

            // Validate every handle before versioning, culling, and physical planning index
            // resource metadata with it. These are user input errors, not internal invariants.
            if (output_table.image_outputs.empty() && output_table.buffer_outputs.empty())
            {
                add_diagnostic(compile_error_code::no_output,
                               invalid_pass,
                               resource_kind::image,
                               invalid_resource,
                               "render graph has no declared output");
            }

            for (const auto image : output_table.image_outputs)
            {
                if (image >= image_count)
                {
                    add_diagnostic(compile_error_code::image_output_out_of_range,
                                   invalid_pass,
                                   resource_kind::image,
                                   image,
                                   "declared image output handle is out of range");
                }
            }
            for (const auto buffer : output_table.buffer_outputs)
            {
                if (buffer >= buffer_count)
                {
                    add_diagnostic(compile_error_code::buffer_output_out_of_range,
                                   invalid_pass,
                                   resource_kind::buffer,
                                   buffer,
                                   "declared buffer output handle is out of range");
                }
            }

            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                const auto image_read_begin = image_read_deps.begins[pass];
                const auto image_read_end   = image_read_begin + image_read_deps.lengthes[pass];
                for (auto index = image_read_begin; index < image_read_end; index++)
                {
                    const auto image = image_read_deps.read_list[index];
                    if (image >= image_count)
                    {
                        add_diagnostic(compile_error_code::image_read_out_of_range,
                                       pass,
                                       resource_kind::image,
                                       image,
                                       "image read handle is out of range");
                    }
                }

                const auto image_write_begin = image_write_deps.begins[pass];
                const auto image_write_end   = image_write_begin + image_write_deps.lengthes[pass];
                for (auto index = image_write_begin; index < image_write_end; index++)
                {
                    const auto image = image_write_deps.write_list[index];
                    if (image >= image_count)
                    {
                        add_diagnostic(compile_error_code::image_write_out_of_range,
                                       pass,
                                       resource_kind::image,
                                       image,
                                       "image write handle is out of range");
                    }
                }

                const auto buffer_read_begin = buffer_read_deps.begins[pass];
                const auto buffer_read_end   = buffer_read_begin + buffer_read_deps.lengthes[pass];
                for (auto index = buffer_read_begin; index < buffer_read_end; index++)
                {
                    const auto buffer = buffer_read_deps.read_list[index];
                    if (buffer >= buffer_count)
                    {
                        add_diagnostic(compile_error_code::buffer_read_out_of_range,
                                       pass,
                                       resource_kind::buffer,
                                       buffer,
                                       "buffer read handle is out of range");
                    }
                }

                const auto buffer_write_begin = buffer_write_deps.begins[pass];
                const auto buffer_write_end   = buffer_write_begin + buffer_write_deps.lengthes[pass];
                for (auto index = buffer_write_begin; index < buffer_write_end; index++)
                {
                    const auto buffer = buffer_write_deps.write_list[index];
                    if (buffer >= buffer_count)
                    {
                        add_diagnostic(compile_error_code::buffer_write_out_of_range,
                                       pass,
                                       resource_kind::buffer,
                                       buffer,
                                       "buffer write handle is out of range");
                    }
                }
            }

            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                const auto begin = ordered_accesses.begins[pass];
                const auto end   = begin + ordered_accesses.lengths[pass];
                for (auto event_index = begin; event_index < end; event_index++)
                {
                    const auto& event = ordered_accesses.events[event_index];
                    if (const auto* image = std::get_if<image_handle>(&event.resource))
                    {
                        if (*image >= image_count)
                        {
                            continue;
                        }
                        const auto& range = std::get<image_access_desc>(event.state).subresource;
                        const auto mip_levels = BackendT::image_mip_levels(meta_table.image_metas.descs[*image]);
                        const auto array_layers = BackendT::image_array_layers(meta_table.image_metas.descs[*image]);
                        const bool valid_mips = range.mip_level_count != 0 && range.base_mip_level < mip_levels &&
                                                (range.mip_level_count == remaining_subresources ||
                                                 range.mip_level_count <= mip_levels - range.base_mip_level);
                        const bool valid_layers = range.array_layer_count != 0 && range.base_array_layer < array_layers &&
                                                  (range.array_layer_count == remaining_subresources ||
                                                   range.array_layer_count <= array_layers - range.base_array_layer);
                        if (range.aspects == image_aspect::none || !valid_mips || !valid_layers)
                        {
                            add_diagnostic(compile_error_code::invalid_image_subresource_range,
                                           pass,
                                           resource_kind::image,
                                           *image,
                                           "image access subresource range is empty or outside the image description");
                        }
                    }
                    else
                    {
                        const auto buffer = std::get<buffer_handle>(event.resource);
                        if (buffer >= buffer_count)
                        {
                            continue;
                        }
                        const auto& range = std::get<buffer_access_desc>(event.state).bytes;
                        const auto size = BackendT::buffer_size(meta_table.buffer_metas.descs[buffer]);
                        const bool valid = range.size != 0 && range.offset < size &&
                                           (range.size == whole_buffer_size || range.size <= size - range.offset);
                        if (!valid)
                        {
                            add_diagnostic(compile_error_code::invalid_buffer_byte_range,
                                           pass,
                                           resource_kind::buffer,
                                           buffer,
                                           "buffer access byte range is empty or outside the buffer description");
                        }
                    }
                }
            }

            if (!result)
            {
                return result;
            }

            // Compute desc hashes (backend-defined). Used by aliasing/reuse grouping.
            // NOTE: collisions are allowed; we always confirm via is_compatible_*.
            for (resource_handle img = 0; img < image_count; img++)
            {
                meta_table.image_metas.desc_hashes[img] = BackendT::hash_image_desc(meta_table.image_metas.descs[img]);
            }
            for (resource_handle buf = 0; buf < buffer_count; buf++)
            {
                meta_table.buffer_metas.desc_hashes[buf] = BackendT::hash_buffer_desc(meta_table.buffer_metas.descs[buf]);
            }

            // Step B: Compute Resource Versions from the ordered access stream.
            // The legacy read/write CSR tables remain derived inspection views; they
            // are not allowed to erase write->read ordering within a pass.

            img_ver_read_handles.resize(image_read_deps.read_list.size());     // index: read dependency index, value: packed(resouce, version)
            img_ver_write_handles.resize(image_write_deps.write_list.size());  // index: write dependency index, value: packed(resouce, version)
            buf_ver_read_handles.resize(buffer_read_deps.read_list.size());    // index: read dependency index, value: packed(resouce, version)
            buf_ver_write_handles.resize(buffer_write_deps.write_list.size()); // index: write dependency index, value: packed(resouce, version)

            std::vector<version_handle> image_next_versions(image_count, 0);   // index: resource, value: next version(finally the latest version)
            std::vector<version_handle> buffer_next_versions(buffer_count, 0); // index: resource, value: next version(finally the latest version)

            for (pass_handle current_pass = 0; current_pass < pass_count; current_pass++)
            {
                auto image_read_index  = image_read_deps.begins[current_pass];
                auto image_write_index = image_write_deps.begins[current_pass];
                auto buffer_read_index = buffer_read_deps.begins[current_pass];
                auto buffer_write_index = buffer_write_deps.begins[current_pass];

                const auto event_begin = ordered_accesses.begins[current_pass];
                const auto event_end   = event_begin + ordered_accesses.lengths[current_pass];
                for (auto event_index = event_begin; event_index < event_end; event_index++)
                {
                    const auto& event = ordered_accesses.events[event_index];
                    if (const auto* image = std::get_if<image_handle>(&event.resource))
                    {
                        if (event.access == access_type::read)
                        {
                            const auto next_version = image_next_versions[*image];
                            img_ver_read_handles[image_read_index++] = next_version == 0
                                                                           ? invalid_resource_version
                                                                           : pack(*image, static_cast<version_handle>(next_version - 1));
                        }
                        else
                        {
                            const auto next_version = image_next_versions[*image];
                            img_ver_write_handles[image_write_index++] = pack(*image, next_version);
                            image_next_versions[*image] = static_cast<version_handle>(next_version + 1);
                        }
                    }
                    else
                    {
                        const auto buffer = std::get<buffer_handle>(event.resource);
                        if (event.access == access_type::read)
                        {
                            const auto next_version = buffer_next_versions[buffer];
                            buf_ver_read_handles[buffer_read_index++] = next_version == 0
                                                                           ? invalid_resource_version
                                                                           : pack(buffer, static_cast<version_handle>(next_version - 1));
                        }
                        else
                        {
                            const auto next_version = buffer_next_versions[buffer];
                            buf_ver_write_handles[buffer_write_index++] = pack(buffer, next_version);
                            buffer_next_versions[buffer] = static_cast<version_handle>(next_version + 1);
                        }
                    }
                }
            }

            // Step C: Build resource-producer map (+ latest version per handle)
            // Build resource_version -> producer lookup in a flat array (DOD/SoA friendly):
            // - offsets are indexed by resource_handle
            // - producers are indexed by (offset + version)

            // In order to know the producer of each resource, we build resource_version - producer
            // dictionary:
            // - key = <resource, version>
            // - value = producer
            // from which we can acknowledge that a resource of 'x' version is produced by pass 'y';

            // Here we build resource_version pair：
            // - 1 resource with n versions
            // but in a flatten 1d array in order to make best performance
            producer_lookup_table.img_version_offsets.assign(static_cast<size_t>(image_count) + 1, 0);
            producer_lookup_table.latest_img.assign(image_count, invalid_resource_version);
            {
                uint32_t offset = 0;
                for (resource_handle image = 0; image < image_count; image++)
                {
                    producer_lookup_table.img_version_offsets[image] = offset;
                    const auto version                               = image_next_versions[image];
                    if (version > 0)
                    {
                        producer_lookup_table.latest_img[image] = pack(image, static_cast<version_handle>(version - 1));
                    }
                    offset = (offset + static_cast<uint32_t>(version));
                }
                producer_lookup_table.img_version_offsets[image_count] = offset;
                producer_lookup_table.img_version_producers.assign(offset, invalid_pass);
            }

            // Here we build resource_version pair：
            // - 1 resource with n versions
            // but in a flatten 1d array in order to make best performance
            producer_lookup_table.buf_version_offsets.assign(static_cast<size_t>(buffer_count) + 1, 0);
            producer_lookup_table.latest_buf.assign(buffer_count, invalid_resource_version);
            {
                uint32_t offset = 0;
                for (resource_handle buffer = 0; buffer < buffer_count; buffer++)
                {
                    producer_lookup_table.buf_version_offsets[buffer] = offset;
                    const auto version                                = buffer_next_versions[buffer];
                    if (version > 0)
                    {
                        producer_lookup_table.latest_buf[buffer] = pack(buffer, static_cast<version_handle>(version - 1));
                    }
                    offset = (offset + static_cast<uint32_t>(version));
                }
                producer_lookup_table.buf_version_offsets[buffer_count] = offset;
                producer_lookup_table.buf_version_producers.assign(offset, invalid_pass);
            }

            // Traverse every write dependency and fill the producer table.
            // Each write already owns a packed (resource, version) handle generated in Step B.
            // If the unpacked version falls inside that resource's version range(which means valid),
            // record:
            //   producer(resource, version) = current_pass

            // a helper lambda function used in producer map creation step and latter culling step.
            constexpr uint32_t invalid_index = std::numeric_limits<uint32_t>::max();
            auto resource_version_index = [](const std::vector<uint32_t>& table, size_t resource_count, resource_version_handle handle) -> uint32_t
            {
                if (handle == invalid_resource_version)
                    return invalid_index;

                const auto resource = unpack_to_resource(handle);
                const auto ver      = unpack_to_version(handle);
                if (resource >= resource_count)
                    return invalid_index;

                const auto base = table[resource];
                const auto end  = table[resource + 1];
                if (ver >= end - base)
                    return invalid_index;

                return base + ver;
            };

            // Fill image producers for each (image, version)
            for (size_t i = 0; i < pass_count; i++)
            {
                const auto current_pass = graph.passes[i];
                const auto begin        = image_write_deps.begins[current_pass];
                const auto length       = image_write_deps.lengthes[current_pass];
                for (auto j = begin; j < begin + length; j++)
                {
                    const auto image_version_handle = img_ver_write_handles[j];
                    const auto idx = resource_version_index(producer_lookup_table.img_version_offsets, image_count, image_version_handle);
                    if (idx != invalid_index)
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
                    const auto idx = resource_version_index(producer_lookup_table.buf_version_offsets, buffer_count, buffer_version_handle);
                    if (idx != invalid_index)
                    {
                        producer_lookup_table.buf_version_producers[idx] = current_pass;
                    }
                }
            }

            // Step D/E: Build overlap-aware access relationships and validate reads.
            // data_predecessors drive culling (RAW only); hazard_predecessors drive
            // scheduling (RAW/WAR/WAW). Same-pass relationships remain in event order.
            std::vector<std::vector<pass_handle>> data_predecessors(pass_count);
            std::vector<std::vector<pass_handle>> hazard_predecessors(pass_count);
            std::vector<std::vector<pass_handle>> image_writer_passes(image_count);
            std::vector<std::vector<pass_handle>> buffer_writer_passes(buffer_count);
            std::vector<std::vector<uint32_t>> image_history(image_count);
            std::vector<std::vector<uint32_t>> buffer_history(buffer_count);
            std::vector<pass_handle> event_passes(ordered_accesses.events.size(), invalid_pass);

            for (pass_handle current_pass = 0; current_pass < pass_count; current_pass++)
            {
                const auto event_begin = ordered_accesses.begins[current_pass];
                const auto event_end   = event_begin + ordered_accesses.lengths[current_pass];
                for (auto event_index = event_begin; event_index < event_end; event_index++)
                {
                    const auto& event = ordered_accesses.events[event_index];
                    event_passes[event_index] = current_pass;

                    if (const auto* image = std::get_if<image_handle>(&event.resource))
                    {
                        const auto& state = std::get<image_access_desc>(event.state);
                        std::vector<image_subresource_range> preceding_writes;
                        for (const auto previous_index : image_history[*image])
                        {
                            const auto& previous = ordered_accesses.events[previous_index];
                            const auto& previous_state = std::get<image_access_desc>(previous.state);
                            if (!overlaps(previous_state.subresource, state.subresource))
                            {
                                continue;
                            }

                            const bool previous_writes = previous.access != access_type::read;
                            const bool current_writes  = event.access != access_type::read;
                            const auto previous_pass   = event_passes[previous_index];
                            if (!current_writes && previous_writes)
                            {
                                preceding_writes.push_back(previous_state.subresource);
                                if (previous_pass != current_pass)
                                {
                                    data_predecessors[current_pass].push_back(previous_pass);
                                }
                            }
                            if ((current_writes || previous_writes) && previous_pass != current_pass)
                            {
                                hazard_predecessors[current_pass].push_back(previous_pass);
                            }
                        }

                        if (event.access == access_type::read && !meta_table.image_metas.is_imported[*image] &&
                            !fully_covers(preceding_writes, state.subresource))
                        {
                            add_diagnostic(compile_error_code::image_read_before_write,
                                           current_pass,
                                           resource_kind::image,
                                           *image,
                                           "non-imported image subresource is read before an overlapping write");
                        }
                        if (event.access != access_type::read)
                        {
                            image_writer_passes[*image].push_back(current_pass);
                        }
                        image_history[*image].push_back(event_index);
                    }
                    else
                    {
                        const auto buffer = std::get<buffer_handle>(event.resource);
                        const auto& state = std::get<buffer_access_desc>(event.state);
                        std::vector<buffer_byte_range> preceding_writes;
                        for (const auto previous_index : buffer_history[buffer])
                        {
                            const auto& previous = ordered_accesses.events[previous_index];
                            const auto& previous_state = std::get<buffer_access_desc>(previous.state);
                            if (!overlaps(previous_state.bytes, state.bytes))
                            {
                                continue;
                            }

                            const bool previous_writes = previous.access != access_type::read;
                            const bool current_writes  = event.access != access_type::read;
                            const auto previous_pass   = event_passes[previous_index];
                            if (!current_writes && previous_writes)
                            {
                                preceding_writes.push_back(previous_state.bytes);
                                if (previous_pass != current_pass)
                                {
                                    data_predecessors[current_pass].push_back(previous_pass);
                                }
                            }
                            if ((current_writes || previous_writes) && previous_pass != current_pass)
                            {
                                hazard_predecessors[current_pass].push_back(previous_pass);
                            }
                        }

                        if (event.access == access_type::read && !meta_table.buffer_metas.is_imported[buffer] &&
                            !fully_covers(preceding_writes, state.bytes))
                        {
                            add_diagnostic(compile_error_code::buffer_read_before_write,
                                           current_pass,
                                           resource_kind::buffer,
                                           buffer,
                                           "non-imported buffer range is read before an overlapping write");
                        }
                        if (event.access != access_type::read)
                        {
                            buffer_writer_passes[buffer].push_back(current_pass);
                        }
                        buffer_history[buffer].push_back(event_index);
                    }
                }
            }

            if (!result)
            {
                return result;
            }

            auto sort_unique = [](std::vector<pass_handle>& passes)
            {
                std::sort(passes.begin(), passes.end());
                passes.erase(std::unique(passes.begin(), passes.end()), passes.end());
            };
            for (pass_handle pass = 0; pass < pass_count; pass++)
            {
                sort_unique(data_predecessors[pass]);
                sort_unique(hazard_predecessors[pass]);
            }

            // Step D: Cull from output writers through data-flow predecessors.
            active_pass_flags.assign(pass_count, false);
            std::queue<pass_handle> culling_worklist;
            auto enqueue_pass = [&](pass_handle pass)
            {
                if (pass != invalid_pass && pass < pass_count && !active_pass_flags[pass])
                {
                    active_pass_flags[pass] = true;
                    culling_worklist.push(pass);
                }
            };

            for (const auto output : output_table.image_outputs)
            {
                for (const auto writer : image_writer_passes[output])
                {
                    enqueue_pass(writer);
                }
            }
            for (const auto output : output_table.buffer_outputs)
            {
                for (const auto writer : buffer_writer_passes[output])
                {
                    enqueue_pass(writer);
                }
            }
            while (!culling_worklist.empty())
            {
                const auto pass = culling_worklist.front();
                culling_worklist.pop();
                for (const auto predecessor : data_predecessors[pass])
                {
                    enqueue_pass(predecessor);
                }
            }

            // Step F: Build the active pass DAG from overlap-aware hazards.
            std::vector<std::vector<pass_handle>> outgoing(pass_count);
            for (pass_handle consumer = 0; consumer < pass_count; consumer++)
            {
                if (!active_pass_flags[consumer])
                {
                    continue;
                }
                for (const auto producer : hazard_predecessors[consumer])
                {
                    if (active_pass_flags[producer])
                    {
                        outgoing[producer].push_back(consumer);
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
            if (sorted_passes.size() != active_pass_count)
            {
                add_diagnostic(compile_error_code::cycle_detected,
                               invalid_pass,
                               resource_kind::image,
                               invalid_resource,
                               "render graph pass dependencies contain a cycle");
                sorted_passes.clear();
                return result;
            }

            // Step H: Lifetime Analysis & Aliasing
            // For each resource version, compute first/last use across the scheduled pass order.
            // Use this to:
            // - allocate transient resources from pools
            // - alias memory for non-overlapping lifetimes

            // 1. Build Pass Index Map (Handle -> Execution Order Index)
            // We need strictly monotonic indices to compare lifetimes correctly.
            std::vector<pass_handle> sorted_pass_indices(pass_count, 0);
            for (pass_handle i = 0; i < sorted_passes.size(); i++)
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
                const pass_handle actual_pass_index = sorted_pass_indices[pass];

                auto update_lifetime = [&](std::vector<pass_handle>& firsts, std::vector<pass_handle>& lasts, resource_handle res, size_t count)
                {
                    if (res >= count)
                        return;
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
                        update_lifetime(resource_lifetimes.image_first_used_pass,
                                        resource_lifetimes.image_last_used_pass,
                                        image_read_deps.read_list[j],
                                        image_count);
                    }
                }
                // image writes
                {
                    const auto write_begin  = image_write_deps.begins[pass];
                    const auto write_length = image_write_deps.lengthes[pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        update_lifetime(resource_lifetimes.image_first_used_pass,
                                        resource_lifetimes.image_last_used_pass,
                                        image_write_deps.write_list[j],
                                        image_count);
                    }
                }
                // buffer reads
                {
                    const auto read_begin  = buffer_read_deps.begins[pass];
                    const auto read_length = buffer_read_deps.lengthes[pass];
                    for (auto j = read_begin; j < read_begin + read_length; j++)
                    {
                        update_lifetime(resource_lifetimes.buffer_first_used_pass,
                                        resource_lifetimes.buffer_last_used_pass,
                                        buffer_read_deps.read_list[j],
                                        buffer_count);
                    }
                }
                // buffer writes
                {
                    const auto write_begin  = buffer_write_deps.begins[pass];
                    const auto write_length = buffer_write_deps.lengthes[pass];
                    for (auto j = write_begin; j < write_begin + write_length; j++)
                    {
                        update_lifetime(resource_lifetimes.buffer_first_used_pass,
                                        resource_lifetimes.buffer_last_used_pass,
                                        buffer_write_deps.write_list[j],
                                        buffer_count);
                    }
                }
            }

            // 3. Aliasing (Greedy First-Fit)
            // Group resources that can share memory (transient & non-overlapping).
            physical_resource_metas.clear();

            auto is_overlapping = [](pass_handle start_a, pass_handle end_a, pass_handle start_b, pass_handle end_b)
            {
                return std::max(start_a, start_b) <= std::min(end_a, end_b);
            };

            // Images
            {
                // Stores intervals for each unique resource: unique_id -> vector<{start, end}>
                std::vector<std::vector<std::pair<pass_handle, pass_handle>>> life_intervals;

                // Resize mapping table
                physical_resource_metas.handle_to_physical_img_id.assign(image_count, invalid_resource);

                for (resource_handle img = 0; img < image_count; img++)
                {
                    const auto first = resource_lifetimes.image_first_used_pass[img];
                    const auto last  = resource_lifetimes.image_last_used_pass[img];

                    // Skip unused
                    if (first == invalid_pass)
                        continue;

                    // Only transient logical resources participate in native object reuse.
                    // Imported objects are external; persistent/history objects retain identity.
                    if (meta_table.image_metas.lifetime_classes[img] != resource_lifetime_class::transient)
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
                        if (life_intervals[u].empty())
                            continue;

                        // Check 1: Compatibility (backend-defined)
                        const auto rep_img = physical_resource_metas.physical_image_meta[u];
                        if (meta_table.image_metas.desc_hashes[rep_img] != meta_table.image_metas.desc_hashes[img])
                        {
                            continue;
                        }
                        if (!BackendT::is_compatible_image(meta_table.image_metas.descs[rep_img], meta_table.image_metas.descs[img]))
                        {
                            continue;
                        }
                        if (BackendT::get_image_allocation_requirements(meta_table.image_metas.descs[rep_img]) !=
                            BackendT::get_image_allocation_requirements(meta_table.image_metas.descs[img]))
                        {
                            continue;
                        }

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
                            assigned                                               = true;
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
                std::vector<std::vector<std::pair<pass_handle, pass_handle>>> life_intervals;
                physical_resource_metas.handle_to_physical_buf_id.assign(buffer_count, invalid_resource);

                for (resource_handle buf = 0; buf < buffer_count; buf++)
                {
                    const auto first = resource_lifetimes.buffer_first_used_pass[buf];
                    const auto last  = resource_lifetimes.buffer_last_used_pass[buf];

                    if (first == invalid_pass)
                        continue;

                    if (meta_table.buffer_metas.lifetime_classes[buf] != resource_lifetime_class::transient)
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
                        if (life_intervals[u].empty())
                            continue;

                        const auto rep_buf = physical_resource_metas.physical_buffer_meta[u];
                        if (meta_table.buffer_metas.desc_hashes[rep_buf] != meta_table.buffer_metas.desc_hashes[buf])
                        {
                            continue;
                        }
                        if (!BackendT::is_compatible_buffer(meta_table.buffer_metas.descs[rep_buf], meta_table.buffer_metas.descs[buf]))
                        {
                            continue;
                        }
                        if (BackendT::get_buffer_allocation_requirements(meta_table.buffer_metas.descs[rep_buf]) !=
                            BackendT::get_buffer_allocation_requirements(meta_table.buffer_metas.descs[buf]))
                        {
                            continue;
                        }

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
                            assigned                                               = true;
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

            // 4. Memory alias plan. This is intentionally separate from native object
            // reuse above: incompatible object descriptions may still share an allocation
            // if their memory requirements are compatible and lifetimes do not overlap.
            struct memory_block_builder
            {
                allocation_requirements requirements;
                bool accepts_aliases = false;
                std::vector<std::tuple<resource_handle, pass_handle, pass_handle>> members;
            };

            auto build_memory_alias_plan = [&](resource_kind kind,
                                               size_t resource_count,
                                               const std::vector<pass_handle>& first_uses,
                                               const std::vector<pass_handle>& last_uses,
                                               const auto& lifetime_classes,
                                               std::vector<resource_handle>& handle_to_block,
                                               std::vector<allocation_requirements>& output_blocks,
                                               auto&& get_requirements)
            {
                handle_to_block.assign(resource_count, invalid_resource);
                std::vector<memory_block_builder> builders;

                for (resource_handle logical = 0; logical < resource_count; logical++)
                {
                    const auto first = first_uses[logical];
                    const auto last  = last_uses[logical];
                    if (first == invalid_pass)
                    {
                        continue;
                    }

                    const auto lifetime = lifetime_classes[logical];
                    if (lifetime == resource_lifetime_class::imported)
                    {
                        continue;
                    }

                    const auto requirements = get_requirements(logical);
                    const bool can_alias = lifetime == resource_lifetime_class::transient &&
                                           requirements.supports_aliasing &&
                                           !requirements.requires_dedicated;
                    resource_handle selected_block = invalid_resource;
                    if (can_alias)
                    {
                        for (resource_handle block_index = 0; block_index < builders.size(); block_index++)
                        {
                            auto& candidate = builders[block_index];
                            if (!candidate.accepts_aliases ||
                                (candidate.requirements.memory_type_bits & requirements.memory_type_bits) == 0)
                            {
                                continue;
                            }

                            const bool lifetime_overlap = std::ranges::any_of(
                                candidate.members,
                                [&](const auto& member)
                                {
                                    return is_overlapping(first, last, std::get<1>(member), std::get<2>(member));
                                });
                            if (!lifetime_overlap)
                            {
                                selected_block = block_index;
                                candidate.requirements.size = std::max(candidate.requirements.size, requirements.size);
                                candidate.requirements.alignment = std::max(candidate.requirements.alignment, requirements.alignment);
                                candidate.requirements.memory_type_bits &= requirements.memory_type_bits;
                                candidate.members.emplace_back(logical, first, last);
                                break;
                            }
                        }
                    }

                    if (selected_block == invalid_resource)
                    {
                        selected_block = static_cast<resource_handle>(builders.size());
                        builders.push_back(memory_block_builder{
                            .requirements = requirements,
                            .accepts_aliases = can_alias,
                            .members = {{logical, first, last}},
                        });
                    }
                    handle_to_block[logical] = selected_block;
                }

                output_blocks.clear();
                output_blocks.reserve(builders.size());
                for (resource_handle block_index = 0; block_index < builders.size(); block_index++)
                {
                    auto& builder = builders[block_index];
                    output_blocks.push_back(builder.requirements);
                    std::sort(builder.members.begin(), builder.members.end(), [](const auto& left, const auto& right)
                    {
                        return std::get<1>(left) < std::get<1>(right);
                    });
                    for (size_t member_index = 1; member_index < builder.members.size(); member_index++)
                    {
                        const auto& previous = builder.members[member_index - 1];
                        const auto& next = builder.members[member_index];
                        physical_resource_metas.alias_handoffs.push_back(physical_resource_meta::alias_handoff{
                            .kind = kind,
                            .previous = std::get<0>(previous),
                            .next = std::get<0>(next),
                            .memory_block = block_index,
                            .at_pass = sorted_passes[std::get<1>(next)],
                        });
                    }
                }
            };

            build_memory_alias_plan(
                resource_kind::image,
                image_count,
                resource_lifetimes.image_first_used_pass,
                resource_lifetimes.image_last_used_pass,
                meta_table.image_metas.lifetime_classes,
                physical_resource_metas.handle_to_image_memory_block,
                physical_resource_metas.image_memory_blocks,
                [&](resource_handle logical)
                {
                    return BackendT::get_image_allocation_requirements(meta_table.image_metas.descs[logical]);
                });

            build_memory_alias_plan(
                resource_kind::buffer,
                buffer_count,
                resource_lifetimes.buffer_first_used_pass,
                resource_lifetimes.buffer_last_used_pass,
                meta_table.buffer_metas.lifetime_classes,
                physical_resource_metas.handle_to_buffer_memory_block,
                physical_resource_metas.buffer_memory_blocks,
                [&](resource_handle logical)
                {
                    return BackendT::get_buffer_allocation_requirements(meta_table.buffer_metas.descs[logical]);
                });

            // Step I: Build Synchronization Plan  (Barriers)
            // Build an API-agnostic per-pass barrier list based on scheduled order.

            per_pass_barriers.clear();
            per_pass_barriers.resize_passes(pass_count);

            // Scratch per-pass AoS; we will flatten into per_pass_barrier (CSR + SoA) afterwards.
            std::vector<std::vector<barrier_op>> scratch(pass_count);

            struct last_use
            {
                resource_handle logical = 0;
                uint32_t usage_bits     = 0;
                pipeline_domain domain  = pipeline_domain::any;
                access_type access      = access_type::read;
                bool valid              = false;
            };

            const auto invalid_physical = invalid_resource;
            std::vector<last_use> last_img_use(physical_resource_metas.physical_image_meta.size());
            std::vector<last_use> last_buf_use(physical_resource_metas.physical_buffer_meta.size());

            auto to_access = [](bool has_read, bool has_write) -> access_type
            {
                if (has_read && has_write)
                    return access_type::read_write;
                if (has_write)
                    return access_type::write;
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
                if (physical == invalid_physical)
                    return;

                // get last use record
                auto& last_vec = (kind == resource_kind::image) ? last_img_use : last_buf_use;
                if (physical >= last_vec.size())
                    return;
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
                    const bool changed =
                        (last.usage_bits != desired_usage_bits) || (last.access != desired_access) || (last.domain != pipeline_domain::any);
                    if (changed)
                    {
                        barrier_op op;
                        op.type           = barrier_op_type::transition;
                        op.kind           = kind;
                        op.logical        = logical;
                        op.physical       = physical;
                        op.src_domain     = last.domain;
                        op.dst_domain     = pipeline_domain::any;
                        op.src_access     = last.access;
                        op.dst_access     = desired_access;
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
                    // bit 0 = read, bit 1 = write. Iterate handles in ascending order
                    // so an identical graph always produces an identical flattened plan.
                    std::vector<uint8_t> access_flags(image_count, 0);
                    std::vector<uint32_t> usage_bits(image_count, 0);

                    const auto r_begin = image_read_deps.begins[pass];
                    const auto r_len   = image_read_deps.lengthes[pass];
                    for (auto j = r_begin; j < r_begin + r_len; j++)
                    {
                        const auto logical = image_read_deps.read_list[j];
                        access_flags[logical] |= uint8_t{1};
                        usage_bits[logical] |= image_read_deps.usage_bits[j];
                    }

                    const auto w_begin = image_write_deps.begins[pass];
                    const auto w_len   = image_write_deps.lengthes[pass];
                    for (auto j = w_begin; j < w_begin + w_len; j++)
                    {
                        const auto logical = image_write_deps.write_list[j];
                        access_flags[logical] |= uint8_t{2};
                        usage_bits[logical] |= image_write_deps.usage_bits[j];
                    }

                    for (resource_handle logical = 0; logical < image_count; logical++)
                    {
                        const auto flags = access_flags[logical];
                        if (flags == 0)
                        {
                            continue;
                        }
                        const auto physical = (logical < physical_resource_metas.handle_to_physical_img_id.size())
                                                  ? physical_resource_metas.handle_to_physical_img_id[logical]
                                                  : invalid_physical;
                        insert_barrier(pass,
                                       resource_kind::image,
                                       logical,
                                       physical,
                                       to_access((flags & uint8_t{1}) != 0, (flags & uint8_t{2}) != 0),
                                       usage_bits[logical]);
                    }
                }

                // Buffers used by this pass
                {
                    std::vector<uint8_t> access_flags(buffer_count, 0);
                    std::vector<uint32_t> usage_bits(buffer_count, 0);

                    const auto r_begin = buffer_read_deps.begins[pass];
                    const auto r_len   = buffer_read_deps.lengthes[pass];
                    for (auto j = r_begin; j < r_begin + r_len; j++)
                    {
                        const auto logical = buffer_read_deps.read_list[j];
                        access_flags[logical] |= uint8_t{1};
                        usage_bits[logical] |= buffer_read_deps.usage_bits[j];
                    }

                    const auto w_begin = buffer_write_deps.begins[pass];
                    const auto w_len   = buffer_write_deps.lengthes[pass];
                    for (auto j = w_begin; j < w_begin + w_len; j++)
                    {
                        const auto logical = buffer_write_deps.write_list[j];
                        access_flags[logical] |= uint8_t{2};
                        usage_bits[logical] |= buffer_write_deps.usage_bits[j];
                    }

                    for (resource_handle logical = 0; logical < buffer_count; logical++)
                    {
                        const auto flags = access_flags[logical];
                        if (flags == 0)
                        {
                            continue;
                        }
                        const auto physical = (logical < physical_resource_metas.handle_to_physical_buf_id.size())
                                                  ? physical_resource_metas.handle_to_physical_buf_id[logical]
                                                  : invalid_physical;
                        insert_barrier(pass,
                                       resource_kind::buffer,
                                       logical,
                                       physical,
                                       to_access((flags & uint8_t{1}) != 0, (flags & uint8_t{2}) != 0),
                                       usage_bits[logical]);
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

                    per_pass_barriers.types[idx]          = op.type;
                    per_pass_barriers.kinds[idx]          = op.kind;
                    per_pass_barriers.logicals[idx]       = op.logical;
                    per_pass_barriers.physicals[idx]      = op.physical;
                    per_pass_barriers.src_domains[idx]    = op.src_domain;
                    per_pass_barriers.dst_domains[idx]    = op.dst_domain;
                    per_pass_barriers.src_accesses[idx]   = op.src_access;
                    per_pass_barriers.dst_accesses[idx]   = op.dst_access;
                    per_pass_barriers.src_usage_bits[idx] = op.src_usage_bits;
                    per_pass_barriers.dst_usage_bits[idx] = op.dst_usage_bits;
                    per_pass_barriers.prev_logicals[idx]  = op.prev_logical;
                }
            }

            // Step J: Physical Resource Allocation
            // Create actual GPU resources for live, non-imported resources.
            // - Imported resources: do not create; expect bind_imported_* later (frame loop)
            // - Call backend to create/realize resources (possibly from pools)

            backend.on_compile_resource_allocation(meta_table, physical_resource_metas);
            return result;
        }

        // 3. Execution System
        void execute()
        {
            pass_execute_context exec_ctx{.resources = resource_access{.backend = &backend}};

            for (const auto pass : sorted_passes)
            {
                backend.apply_barriers(pass, per_pass_barriers);

                if (pass < graph.execute_funcs.size() && graph.execute_funcs[pass])
                {
                    graph.execute_funcs[pass](exec_ctx);
                }
            }
        }

        void clear()
        {
            reset_compiled_state();
            graph.passes.clear();
            graph.pass_names.clear();
            graph.setup_funcs.clear();
            graph.execute_funcs.clear();
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

    private:
        friend struct unit_test::system_test_access;

        void reset_compiled_state()
        {
            meta_table.clear();
            image_read_deps = {};
            image_write_deps = {};
            buffer_read_deps = {};
            buffer_write_deps = {};
            ordered_accesses.clear();
            img_ver_read_handles.clear();
            img_ver_write_handles.clear();
            buf_ver_read_handles.clear();
            buf_ver_write_handles.clear();
            producer_lookup_table.clear();
            output_table = {};
            resource_lifetimes.clear();
            physical_resource_metas.clear();
            dag = {};
            active_pass_flags.clear();
            sorted_passes.clear();
            per_pass_barriers.clear();
        }

        // Debug/inspection storage (kept private; accessed via const getters or unit_test::system_test_access).
        directed_acyclic_graph dag;
        std::vector<bool> active_pass_flags;
        std::vector<pass_handle> sorted_passes;

        // resource related
        meta_table_t meta_table;
        read_dependency image_read_deps;
        write_dependency image_write_deps;
        read_dependency buffer_read_deps;
        write_dependency buffer_write_deps;
        ordered_pass_accesses ordered_accesses;

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

        // backend related (owned by RG)
        BackendT backend{};

        // Barrier plan generated during compile().
        // Indexed by pass_handle; only active passes are consumed by execute().
        per_pass_barrier per_pass_barriers;
    };

} // namespace render_graph
