// Compile-scale micro benchmark: synthesizes a frame plan with
// 256 passes and ~2000 access events, then measures compile_graph() time.
// Reports one data line per run — deliberately no pass/fail threshold, not a
// CI gate (仅作重构前后对比数据，不进 CI 门槛).
#include "compile_benchmark.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "render_graph/compiler.h"
#include "test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        // Adjust the scale here; the plan's reference figure is 256 passes.
        constexpr uint32_t pass_count   = 256;
        constexpr uint32_t image_count  = pass_count;      // one owned image per pass
        constexpr uint32_t buffer_count = pass_count / 4;  // shared storage buffers
        constexpr uint32_t iterations   = 20;              // timed compiles (after 1 warmup)

        image_desc transient_color()
        {
            return {.fmt = format::R8G8B8A8_UNORM,
                    .extent = {256, 256, 1},
                    .usage = image_usage::COLOR_ATTACHMENT | image_usage::SAMPLED};
        }

        struct synthetic_plan
        {
            std::vector<std::string> names;  // stable storage for resource string_views
            std::vector<frame_resource_row> resources;
            std::vector<frame_pass_row> passes;
            std::vector<frame_buffer_access_row> buffers;
            std::vector<frame_image_access_row> images;
            frame_plan plan;

            void publish(uint64_t key = 17)
            {
                plan = {
                    .cache_key = key,
                    .resources = resources,
                    .passes = passes,
                    .buffer_accesses = buffers,
                    .image_accesses = images,
                };
            }
        };

        // Chain (pass i reads image i-1) + broadcast reads + buffer traffic.
        // The last pass is the culling root via side_effect; the chain keeps
        // every pass alive, so the benchmark exercises the full pipeline.
        synthetic_plan synthesize()
        {
            synthetic_plan out;
            out.names.reserve(image_count + buffer_count);
            out.resources.reserve(image_count + buffer_count);
            out.images.reserve(pass_count * 6);
            out.buffers.reserve(pass_count * 2);

            for (uint32_t i = 0; i < image_count; ++i)
            {
                out.names.push_back("img" + std::to_string(i));
                out.resources.push_back({.source = frame_resource_source::transient_image,
                                         .name = out.names.back(),
                                         .image_description = transient_color()});
            }
            for (uint32_t b = 0; b < buffer_count; ++b)
            {
                out.names.push_back("buf" + std::to_string(b));
                out.resources.push_back({.source = frame_resource_source::transient_buffer,
                                         .name = out.names.back(),
                                         .buffer_description = {.size = 4096, .usage = buffer_usage::STORAGE_BUFFER}});
            }

            for (uint32_t i = 0; i < pass_count; ++i)
            {
                // Own write target.
                out.images.push_back({.resource = {i}, .usage = image_usage::STORAGE, .access = access_type::write});
                // Chain read: previous pass's write (no-op for pass 0).
                out.images.push_back({.resource = {i > 0 ? i - 1 : 0}, .usage = image_usage::SAMPLED, .access = access_type::read});
                // Broadcast reads on four shared images.
                out.images.push_back({.resource = {(i * 7U + 3U) % image_count}, .usage = image_usage::SAMPLED, .access = access_type::read});
                out.images.push_back({.resource = {(i * 11U + 5U) % image_count}, .usage = image_usage::SAMPLED, .access = access_type::read});
                out.images.push_back({.resource = {(i * 13U + 7U) % image_count}, .usage = image_usage::SAMPLED, .access = access_type::read});
                out.images.push_back({.resource = {(i * 17U + 11U) % image_count}, .usage = image_usage::SAMPLED, .access = access_type::read});
                // Buffer traffic: one read, one write per pass. Buffer rows
                // live after the image resources in the resource table.
                out.buffers.push_back({.resource = {image_count + (i % buffer_count)}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::read});
                out.buffers.push_back({.resource = {image_count + (((i * 3U) + 1U) % buffer_count)}, .usage = buffer_usage::STORAGE_BUFFER, .access = access_type::write});

                const auto first_image = static_cast<uint32_t>(out.images.size() - 6);
                const auto first_buffer = static_cast<uint32_t>(out.buffers.size() - 2);
                out.passes.push_back({.name = "pass" + std::to_string(i),
                                      .kind = pass_kind::compute,
                                      .queue = queue_class::graphics,
                                      .buffer_accesses = {first_buffer, 2},
                                      .image_accesses = {first_image, 4},
                                      .side_effect = (i == pass_count - 1)});
            }
            out.publish();
            return out;
        }
    }

    void compile_benchmark_test()
    {
        auto storage = synthesize();
        const graph_compile_request request{
            .frame = &storage.plan,
            .environment = {.extent = {1280, 720, 1},
                            .color_format = format::B8G8R8A8_UNORM,
                            .swapchain_initialized = true},
        };

        // Sanity: the synthesized plan must compile successfully and keep all
        // passes alive (the culling chain must not shrink the workload).
        {
            const auto check = compile_graph(request);
            if (!check.succeeded())
            {
                for (const auto& diagnostic : check.result.diagnostics)
                    std::cout << "    diagnostic: " << static_cast<uint32_t>(diagnostic.code) << " " << diagnostic.message
                              << " (pass " << diagnostic.pass.index() << ")\n";
            }
            RG_CHECK(check.succeeded());
            RG_CHECK(check.plan.scheduled_passes.size() == pass_count);
        }

        std::vector<double> samples;
        samples.reserve(iterations);
        for (uint32_t run = 0; run <= iterations; ++run)
        {
            const auto start = std::chrono::steady_clock::now();
            const auto output = compile_graph(request);
            const auto end = std::chrono::steady_clock::now();
            RG_CHECK(output.succeeded());
            if (run != 0)  // first run is warmup (page faults, code loading)
                samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        std::sort(samples.begin(), samples.end());

        const auto best = samples.front();
        const auto median = samples[samples.size() / 2];
        const auto events = compile_graph(request).plan.statistics.access_event_count;

        std::cout << "[bench] compile passes=" << pass_count
                  << " events=" << events
                  << " best_us=" << best
                  << " median_us=" << median << '\n';
    }
} // namespace render_graph::unit_test
