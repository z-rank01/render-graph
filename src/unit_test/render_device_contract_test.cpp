#include "render_graph/unit_test/render_device_contract_test.h"

#include "render_graph/render_device.h"
#include "render_graph/unit_test/test_check.h"

namespace render_graph::unit_test
{
    namespace
    {
        struct fake_state
        {
            uint32_t next_buffer = 0;
            uint32_t renders = 0;
            uint32_t destroys = 0;
        };

        frame_build_result build_frame(void* state, const frame_environment& environment, frame_plan& plan)
        {
            auto& built = *static_cast<bool*>(state);
            built = environment.extent.width == 1280 && environment.color_format == format::B8G8R8A8_UNORM;
            plan.cache_key = 7;
            plan.pass_name = "ContractPass";
            return {};
        }
    }

    void render_device_contract_test()
    {
        render_device empty;
        RG_CHECK(!empty);
        RG_CHECK(!empty.apply_resource_changes({}));
        RG_CHECK(empty.render({}).status == frame_status::failed);
        empty.request_resize();
        empty.shutdown();
        RG_CHECK(empty.statistics().presented_frames == 0);
        RG_CHECK(empty.validation_error_count() == 0);

        fake_state state;
        const render_device_api api{
            .apply_resource_changes = [](void* value, const resource_change_batch& batch)
            {
                auto& fake = *static_cast<fake_state*>(value);
                resource_change_result result;
                for ([[maybe_unused]] const auto& row : batch.buffer_creates)
                    result.buffers.push_back({fake.next_buffer++, 1});
                return result;
            },
            .render = [](void* value, const frame_recipe& recipe)
            {
                auto& fake = *static_cast<fake_state*>(value);
                frame_plan plan;
                const frame_environment environment{
                    .extent = {1280, 720, 1},
                    .color_format = format::B8G8R8A8_UNORM,
                };
                const auto built = recipe.build(recipe.state, environment, plan);
                if (!built) return frame_result{.status = frame_status::failed, .error = built.error};
                ++fake.renders;
                return frame_result{.status = frame_status::rendered};
            },
            .request_resize = [](void*) noexcept {},
            .shutdown = [](void*) noexcept {},
            .statistics = [](const void* value) noexcept
            {
                return render_statistics{.presented_frames = static_cast<const fake_state*>(value)->renders};
            },
            .validation_error_count = [](const void*) noexcept { return 0u; },
            .destroy = nullptr,
        };
        render_device device(&state, &api);
        const buffer_create_row creates[]{{.desc = {.size = 256, .usage = buffer_usage::STORAGE_BUFFER}}};
        const auto changed = device.apply_resource_changes({.buffer_creates = creates});
        RG_CHECK(changed);
        RG_CHECK(changed.buffers.size() == 1);
        RG_CHECK(changed.buffers.front().index == 0 && changed.buffers.front().generation == 1);

        bool built = false;
        const auto rendered = device.render({.state = &built, .build = &build_frame});
        RG_CHECK(rendered.status == frame_status::rendered);
        RG_CHECK(built);
        RG_CHECK(device.statistics().presented_frames == 1);
    }
} // namespace render_graph::unit_test
