// Unit tests for the Vulkan resource allocator / view cache: fake dispatches
// record allocation and lifetime calls so tests can assert reuse, retirement,
// and imported-resource binding without a real device.
#include "vulkan_resource_allocator_test.h"

#include <cstdint>
#include <type_traits>

#include "render_graph/compiler.h"
#include "test_check.h"
#include "vk_backend.h"

namespace render_graph::unit_test
{
    namespace
    {
        // =========================================================================
        // Test helpers
        // =========================================================================

        // Builds a fake native handle from a plain integer.
        template <typename Handle>
        Handle fake_handle(uintptr_t value)
        {
            if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
            else return static_cast<Handle>(value);
        }

        // Counts every allocator callback so tests can assert exact lifetimes.
        struct allocator_log
        {
            uint32_t allocations = 0;
            uint32_t frees = 0;
            uint32_t image_creates = 0;
            uint32_t image_destroys = 0;
            uint32_t view_creates = 0;
            uint32_t view_destroys = 0;
        };

        vk_allocator_dispatch fake_dispatch(allocator_log& log)
        {
            vk_allocator_dispatch dispatch;
            dispatch.image_requirements = [](const VkImageCreateInfo& desc)
            {
                return allocation_requirements{.size = static_cast<uint64_t>(desc.extent.width) * desc.extent.height * 4,
                    .alignment = 256, .memory_type_bits = 1, .supports_aliasing = true};
            };
            dispatch.buffer_requirements = [](const VkBufferCreateInfo& desc)
            { return allocation_requirements{.size = desc.size, .alignment = 256, .memory_type_bits = 1,
                                               .supports_aliasing = true}; };
            dispatch.allocate = [&](const allocation_requirements&, vk_allocation_handle& allocation)
            { allocation = reinterpret_cast<void*>(static_cast<uintptr_t>(++log.allocations)); return true; };
            dispatch.free = [&](vk_allocation_handle) { ++log.frees; };
            dispatch.create_image = [&](vk_allocation_handle, const VkImageCreateInfo&, VkImage& image)
            { image = fake_handle<VkImage>(0x1000 + ++log.image_creates); return true; };
            dispatch.create_buffer = [](vk_allocation_handle, const VkBufferCreateInfo&, VkBuffer& buffer)
            { buffer = fake_handle<VkBuffer>(0x2000); return true; };
            dispatch.destroy_image = [&](VkImage) { ++log.image_destroys; };
            dispatch.destroy_buffer = [](VkBuffer) {};
            dispatch.create_view = [&](VkImage, const VkImageViewCreateInfo&, VkImageView& view)
            { view = fake_handle<VkImageView>(0x3000 + ++log.view_creates); return true; };
            dispatch.destroy_view = [&](VkImageView) { ++log.view_destroys; };
            return dispatch;
        }

        image_desc make_image(uint32_t width)
        {
            return {.fmt = format::R8G8B8A8_UNORM, .extent = {width, 32, 1},
                    .usage = image_usage::TRANSFER_DST | image_usage::SAMPLED};
        }

        // A two-image plan: a stable transient and a resizable transient.
        compiled_graph_plan make_two_image_plan(uint32_t second_width)
        {
            compiled_graph_plan plan;
            const auto first_desc = make_image(32);
            const auto second_desc = make_image(second_width);
            plan.resources.image_metas.add("Stable", first_desc, resource_lifetime_class::transient,
                                           hash_resource_desc(first_desc));
            plan.resources.image_metas.add("Resizable", second_desc, resource_lifetime_class::transient,
                                           hash_resource_desc(second_desc));
            auto& physical = plan.physical_resources;
            physical.physical_image_meta = {image_handle{0}, image_handle{1}};
            physical.handle_to_physical_img_id = {physical_image_id{0}, physical_image_id{1}};
            physical.handle_to_image_memory_block = {memory_block_id{0}, memory_block_id{1}};
            physical.image_memory_blocks = {
                {.size = 32 * 32 * 4, .alignment = 256, .memory_type_bits = 1, .supports_aliasing = true},
                {.size = static_cast<uint64_t>(second_width) * 32 * 4, .alignment = 256,
                 .memory_type_bits = 1, .supports_aliasing = true},
            };
            return plan;
        }

        // =========================================================================
        // Test cases
        // =========================================================================

        // Recompiling the same plan reuses allocations, and matching image views
        // hit the view cache while incompatible ones are rejected.
        void allocation_reuse_and_view_cache()
        {
            allocator_log log;
            vk_graph_executor backend;
            backend.set_context(fake_handle<VkPhysicalDevice>(1), fake_handle<VkDevice>(2),
                                fake_dispatch(log), {}, 2);
            auto plan = make_two_image_plan(64);
            backend.on_compile_resource_allocation(plan.resources, plan.physical_resources);
            RG_CHECK(backend.get_last_error().empty());
            RG_CHECK(log.allocations == 2);
            RG_CHECK(log.image_creates == 2);
            const auto allocations = log.allocations;
            const auto creates = log.image_creates;
            backend.on_compile_resource_allocation(plan.resources, plan.physical_resources);
            RG_CHECK(log.allocations == allocations);
            RG_CHECK(log.image_creates == creates);
            const auto first = backend.get_or_create_image_view(image_handle{1}, {});
            const auto cached = backend.get_or_create_image_view(image_handle{1}, {});
            RG_CHECK(first != VK_NULL_HANDLE && first == cached);
            RG_CHECK(log.view_creates == 1);
            auto incompatible = vk_image_view_desc{};
            incompatible.format = VK_FORMAT_B8G8R8A8_UNORM;
            RG_CHECK(backend.get_or_create_image_view(image_handle{1}, incompatible) == VK_NULL_HANDLE);
        }

        // Resizing replaces only the changed image block; imported images are
        // bound directly and skip allocation entirely.
        void resize_and_import_retirement()
        {
            allocator_log log;
            vk_graph_executor backend;
            backend.set_context(fake_handle<VkPhysicalDevice>(1), fake_handle<VkDevice>(2),
                                fake_dispatch(log), {}, 2);
            auto initial = make_two_image_plan(32);
            backend.on_compile_resource_allocation(initial.resources, initial.physical_resources);
            const auto stable = backend.get_image(image_handle{0});
            backend.begin_frame(10, 0);
            auto resized = make_two_image_plan(64);
            backend.on_compile_resource_allocation(resized.resources, resized.physical_resources);
            RG_CHECK(backend.get_image(image_handle{0}) == stable);
            RG_CHECK(log.allocations == 3);
            RG_CHECK(log.image_creates == 3);
            backend.begin_frame(11, 11);
            RG_CHECK(log.image_destroys == 0);
            backend.begin_frame(12, 12);
            RG_CHECK(log.image_destroys == 1);

            compiled_graph_plan imported;
            auto desc = make_image(128);
            desc.lifetime = resource_lifetime_class::imported;
            imported.resources.image_metas.add("Imported", desc, desc.lifetime, hash_resource_desc(desc), true);
            imported.physical_resources.physical_image_meta = {image_handle{0}};
            imported.physical_resources.handle_to_physical_img_id = {physical_image_id{0}};
            imported.physical_resources.handle_to_image_memory_block = {invalid_memory_block_id};
            const auto native = fake_handle<VkImage>(0x9001);
            backend.bind_imported_image(image_handle{0}, native);
            backend.on_compile_resource_allocation(imported.resources, imported.physical_resources);
            RG_CHECK(backend.get_image(image_handle{0}) == native);
        }
    }

    void vulkan_resource_allocator_test()
    {
        allocation_reuse_and_view_cache();
        resize_and_import_retirement();
    }
}
