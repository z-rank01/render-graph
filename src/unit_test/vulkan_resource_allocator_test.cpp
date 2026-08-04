#include "render_graph/unit_test/vulkan_resource_allocator_test.h"

#include <cstdint>
#include <type_traits>
#include <vector>

#include "render_graph/system.h"
#include "render_graph/unit_test/test_check.h"
#include "render_graph/vk_backend.h"

namespace render_graph::unit_test
{
    namespace
    {
        template <typename Handle>
        Handle fake_handle(uintptr_t value)
        {
            if constexpr (std::is_pointer_v<Handle>)
            {
                return reinterpret_cast<Handle>(value);
            }
            else
            {
                return static_cast<Handle>(value);
            }
        }

        struct allocator_log
        {
            uint32_t allocations = 0;
            uint32_t frees = 0;
            uint32_t image_creates = 0;
            uint32_t buffer_creates = 0;
            uint32_t image_destroys = 0;
            uint32_t buffer_destroys = 0;
            uint32_t view_creates = 0;
            uint32_t view_destroys = 0;
        };

        vk_allocator_dispatch fake_dispatch(allocator_log& log)
        {
            vk_allocator_dispatch dispatch;
            dispatch.image_requirements = [](const VkImageCreateInfo& desc)
            {
                return allocation_requirements{
                    .size = static_cast<uint64_t>(desc.extent.width) * desc.extent.height * 4,
                    .alignment = 256,
                    .memory_type_bits = 1,
                    .supports_aliasing = true,
                };
            };
            dispatch.buffer_requirements = [](const VkBufferCreateInfo& desc)
            {
                return allocation_requirements{
                    .size = desc.size,
                    .alignment = 256,
                    .memory_type_bits = 1,
                    .supports_aliasing = true,
                };
            };
            dispatch.allocate = [&](const allocation_requirements&, vk_allocation_handle& allocation)
            {
                allocation = reinterpret_cast<void*>(static_cast<uintptr_t>(++log.allocations));
                return true;
            };
            dispatch.free = [&](vk_allocation_handle) { ++log.frees; };
            dispatch.create_image = [&](vk_allocation_handle, const VkImageCreateInfo&, VkImage& image)
            {
                image = fake_handle<VkImage>(0x1000 + ++log.image_creates);
                return true;
            };
            dispatch.create_buffer = [&](vk_allocation_handle, const VkBufferCreateInfo&, VkBuffer& buffer)
            {
                buffer = fake_handle<VkBuffer>(0x2000 + ++log.buffer_creates);
                return true;
            };
            dispatch.destroy_image = [&](VkImage) { ++log.image_destroys; };
            dispatch.destroy_buffer = [&](VkBuffer) { ++log.buffer_destroys; };
            dispatch.create_view = [&](VkImage, const VkImageViewCreateInfo&, VkImageView& view)
            {
                view = fake_handle<VkImageView>(0x3000 + ++log.view_creates);
                return true;
            };
            dispatch.destroy_view = [&](VkImageView) { ++log.view_destroys; };
            return dispatch;
        }

        VkImageCreateInfo image_desc(uint32_t width, uint32_t mip_levels = 1, bool imported = false)
        {
            return VkImageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .flags = imported ? 0U : static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_ALIAS_BIT),
                .imageType = VK_IMAGE_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .extent = {.width = width, .height = 32, .depth = 1},
                .mipLevels = mip_levels,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
        }

        using system_t = render_graph_system<vk_backend>;
        using setup_context = system_t::pass_setup_context;
        using execute_context = system_t::pass_execute_context;

        void noop_execute(execute_context&) {}

        void alias_reuse_and_view_test()
        {
            allocator_log log;
            system_t rg;
            rg.set_backend_context(fake_handle<VkPhysicalDevice>(1),
                                   fake_handle<VkDevice>(2),
                                   fake_dispatch(log),
                                   vk_queue_family_indices{},
                                   2);
            image_handle first{};
            image_handle bridge{};
            image_handle second{};
            rg.add_pass("first", [&](setup_context& ctx)
            {
                first = ctx.create_image("First", image_desc(32));
                ctx.write_image(first, image_usage::TRANSFER_DST);
            }, noop_execute);
            rg.add_pass("bridge", [&](setup_context& ctx)
            {
                ctx.read_image(first, image_usage::TRANSFER_SRC);
                bridge = ctx.create_image("Bridge", image_desc(16));
                ctx.write_image(bridge, image_usage::TRANSFER_DST);
            }, noop_execute);
            rg.add_pass("second", [&](setup_context& ctx)
            {
                ctx.read_image(bridge, image_usage::TRANSFER_SRC);
                second = ctx.create_image("Second", image_desc(64, 2));
                ctx.write_image(second, image_usage::TRANSFER_DST);
                ctx.declare_image_output(second);
            }, noop_execute);

            RG_CHECK(rg.compile().succeeded());
            RG_CHECK(rg.get_physical_image_id(first) != rg.get_physical_image_id(second));
            RG_CHECK(rg.get_image_memory_block(first) == rg.get_image_memory_block(second));
            RG_CHECK(log.allocations == 2);
            RG_CHECK(log.image_creates == 3);

            const auto allocations = log.allocations;
            const auto creates = log.image_creates;
            RG_CHECK(rg.compile().succeeded());
            RG_CHECK(log.allocations == allocations);
            RG_CHECK(log.image_creates == creates);

            auto& backend = rg.get_backend_context();
            const vk_image_view_desc base_view{};
            const auto first_view = backend.get_or_create_image_view(second, base_view);
            const auto cached_view = backend.get_or_create_image_view(second, base_view);
            RG_CHECK(first_view != VK_NULL_HANDLE);
            RG_CHECK(first_view == cached_view);
            RG_CHECK(log.view_creates == 1);
            auto mip_view = base_view;
            mip_view.base_mip_level = 1;
            mip_view.mip_level_count = 1;
            RG_CHECK(backend.get_or_create_image_view(second, mip_view) != first_view);
            RG_CHECK(log.view_creates == 2);
            auto incompatible_view = base_view;
            incompatible_view.format = VK_FORMAT_B8G8R8A8_UNORM;
            RG_CHECK(backend.get_or_create_image_view(second, incompatible_view) == VK_NULL_HANDLE);
        }

        void imported_rebind_and_deferred_destroy_test()
        {
            allocator_log log;
            system_t rg;
            rg.set_backend_context(fake_handle<VkPhysicalDevice>(1),
                                   fake_handle<VkDevice>(2),
                                   fake_dispatch(log),
                                   vk_queue_family_indices{},
                                   2);
            image_handle imported{};
            rg.add_pass("imported", [&](setup_context& ctx)
            {
                imported = ctx.create_image("Imported", image_desc(128, 1, true), resource_lifetime_class::imported);
                ctx.write_image(imported, image_usage::TRANSFER_DST);
                ctx.declare_image_output(imported);
            }, noop_execute);
            const auto native0 = fake_handle<VkImage>(0x9001);
            const auto native1 = fake_handle<VkImage>(0x9002);
            rg.bind_imported_image(image_handle{0}, native0);
            RG_CHECK(rg.compile().succeeded());
            RG_CHECK(rg.get_backend_context().get_image(imported) == native0);
            RG_CHECK(rg.get_backend_context().get_or_create_image_view(imported, {}) != VK_NULL_HANDLE);
            rg.bind_imported_image(imported, native1);
            RG_CHECK(rg.get_backend_context().get_image(imported) == native1);
            RG_CHECK(rg.get_backend_context().get_or_create_image_view(imported, {}) != VK_NULL_HANDLE);
            RG_CHECK(log.view_creates == 2);
            RG_CHECK(log.view_destroys == 0);
            rg.get_backend_context().begin_frame(1, 1);
            RG_CHECK(log.view_destroys == 0);
            rg.get_backend_context().begin_frame(2, 2);
            RG_CHECK(log.view_destroys == 1);
        }

        void resize_invalidation_test()
        {
            allocator_log log;
            system_t rg;
            rg.set_backend_context(fake_handle<VkPhysicalDevice>(1),
                                   fake_handle<VkDevice>(2),
                                   fake_dispatch(log),
                                   vk_queue_family_indices{},
                                   2);
            uint32_t width = 32;
            image_handle stable{};
            image_handle output{};
            rg.add_pass("resizable", [&](setup_context& ctx)
            {
                stable = ctx.create_image("Stable", image_desc(16));
                ctx.write_image(stable, image_usage::TRANSFER_DST);
                output = ctx.create_image("Resizable", image_desc(width));
                ctx.write_image(output, image_usage::TRANSFER_DST);
                ctx.declare_image_output(stable);
                ctx.declare_image_output(output);
            }, noop_execute);
            RG_CHECK(rg.compile().succeeded());
            const auto stable_native = rg.get_backend_context().get_image(stable);
            const auto resized_native = rg.get_backend_context().get_image(output);
            const auto stable_view = rg.get_backend_context().get_or_create_image_view(stable, {});
            const auto resized_view = rg.get_backend_context().get_or_create_image_view(output, {});
            RG_CHECK(log.allocations == 2);
            RG_CHECK(log.image_creates == 2);
            rg.get_backend_context().begin_frame(10, 0);
            width = 64;
            RG_CHECK(rg.compile().succeeded());
            RG_CHECK(log.allocations == 3);
            RG_CHECK(log.image_creates == 3);
            RG_CHECK(rg.get_backend_context().get_image(stable) == stable_native);
            RG_CHECK(rg.get_backend_context().get_image(output) != resized_native);
            RG_CHECK(rg.get_backend_context().get_or_create_image_view(stable, {}) == stable_view);
            RG_CHECK(rg.get_backend_context().get_or_create_image_view(output, {}) != resized_view);
            RG_CHECK(log.image_destroys == 0);
            RG_CHECK(log.frees == 0);
            rg.get_backend_context().begin_frame(11, 11);
            RG_CHECK(log.image_destroys == 0);
            rg.get_backend_context().begin_frame(12, 12);
            RG_CHECK(log.image_destroys == 1);
            RG_CHECK(log.frees == 1);
            RG_CHECK(log.view_destroys == 1);
        }

        void repeated_rebuild_cleanup_test()
        {
            allocator_log log;
            {
                system_t rg;
                rg.set_backend_context(fake_handle<VkPhysicalDevice>(1),
                                       fake_handle<VkDevice>(2),
                                       fake_dispatch(log),
                                       vk_queue_family_indices{},
                                       2);
                for (uint32_t iteration = 0; iteration < 4; iteration++)
                {
                    rg.clear();
                    rg.add_pass("persistent shape", [iteration](setup_context& ctx)
                    {
                        const auto output = ctx.create_image("Output", image_desc(iteration % 2 == 0 ? 32 : 64));
                        ctx.write_image(output, image_usage::TRANSFER_DST);
                        ctx.declare_image_output(output);
                    }, noop_execute);
                    rg.begin_frame(iteration + 1, iteration, iteration % 2);
                    RG_CHECK(rg.compile().succeeded());
                    rg.abort_frame();
                }
            }
            RG_CHECK(log.allocations == log.frees);
            RG_CHECK(log.image_creates == log.image_destroys);
            RG_CHECK(log.buffer_creates == log.buffer_destroys);
            RG_CHECK(log.view_creates == log.view_destroys);
        }
    }

    void vulkan_resource_allocator_test()
    {
        alias_reuse_and_view_test();
        imported_rebind_and_deferred_destroy_test();
        resize_invalidation_test();
        repeated_rebuild_cleanup_test();
    }
}
