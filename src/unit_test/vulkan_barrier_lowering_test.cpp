#include "render_graph/unit_test/vulkan_barrier_lowering_test.h"

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "render_graph/unit_test/test_check.h"
#include "render_graph/backend/vulkan/graph_backend.h"
#include "render_graph/backend/vulkan/barrier_lowering.h"

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

        abstract_resource_state image_state(image_usage usage,
                                            access_type access,
                                            pipeline_domain domain = pipeline_domain::graphics)
        {
            return abstract_resource_state{
                .usage_bits = static_cast<uint32_t>(usage),
                .access = access,
                .domain = domain,
            };
        }

        abstract_resource_state buffer_state(buffer_usage usage,
                                             access_type access,
                                             pipeline_domain domain = pipeline_domain::graphics)
        {
            return abstract_resource_state{
                .usage_bits = static_cast<uint32_t>(usage),
                .access = access,
                .domain = domain,
            };
        }

        void state_mapping_test()
        {
            const auto undefined = lower_vk_image_state(image_state(image_usage::NONE, access_type::read));
            RG_CHECK(undefined.stages == VK_PIPELINE_STAGE_2_NONE);
            RG_CHECK(undefined.access == VK_ACCESS_2_NONE);
            RG_CHECK(undefined.layout == VK_IMAGE_LAYOUT_UNDEFINED);

            const auto sampled = lower_vk_image_state(image_state(image_usage::SAMPLED, access_type::read));
            RG_CHECK(sampled.stages == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
            RG_CHECK(sampled.access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            RG_CHECK(sampled.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            const auto storage = lower_vk_image_state(image_state(image_usage::STORAGE, access_type::read_write, pipeline_domain::compute));
            RG_CHECK(storage.stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            RG_CHECK((storage.access & VK_ACCESS_2_SHADER_STORAGE_READ_BIT) != 0);
            RG_CHECK((storage.access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) != 0);
            RG_CHECK(storage.layout == VK_IMAGE_LAYOUT_GENERAL);

            const auto color = lower_vk_image_state(image_state(image_usage::COLOR_ATTACHMENT, access_type::write));
            RG_CHECK(color.stages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            RG_CHECK(color.access == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            RG_CHECK(color.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            const auto depth = lower_vk_image_state(image_state(image_usage::DEPTH_STENCIL_ATTACHMENT, access_type::read_write));
            RG_CHECK((depth.stages & VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT) != 0);
            RG_CHECK((depth.stages & VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT) != 0);
            RG_CHECK((depth.access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) != 0);
            RG_CHECK((depth.access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);
            RG_CHECK(depth.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

            const auto transfer_source = lower_vk_image_state(image_state(image_usage::TRANSFER_SRC, access_type::read));
            const auto transfer_destination = lower_vk_image_state(image_state(image_usage::TRANSFER_DST, access_type::write));
            RG_CHECK(transfer_source.access == VK_ACCESS_2_TRANSFER_READ_BIT);
            RG_CHECK(transfer_source.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            RG_CHECK(transfer_destination.access == VK_ACCESS_2_TRANSFER_WRITE_BIT);
            RG_CHECK(transfer_destination.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            const auto present = lower_vk_image_state(image_state(image_usage::PRESENT, access_type::read));
            RG_CHECK(present.stages == VK_PIPELINE_STAGE_2_NONE);
            RG_CHECK(present.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

            const auto vertex = lower_vk_buffer_state(buffer_state(buffer_usage::VERTEX_BUFFER, access_type::read));
            const auto index = lower_vk_buffer_state(buffer_state(buffer_usage::INDEX_BUFFER, access_type::read));
            const auto indirect = lower_vk_buffer_state(buffer_state(buffer_usage::INDIRECT_BUFFER, access_type::read));
            const auto uniform = lower_vk_buffer_state(buffer_state(buffer_usage::UNIFORM_BUFFER, access_type::read));
            RG_CHECK(vertex.stages == VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
            RG_CHECK(vertex.access == VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
            RG_CHECK(index.stages == VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);
            RG_CHECK(index.access == VK_ACCESS_2_INDEX_READ_BIT);
            RG_CHECK(indirect.stages == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
            RG_CHECK(indirect.access == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
            RG_CHECK(uniform.stages == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
            RG_CHECK(uniform.access == VK_ACCESS_2_UNIFORM_READ_BIT);
        }

        void range_and_batch_test()
        {
            const auto image = fake_handle<VkImage>(0x101);
            const auto buffer = fake_handle<VkBuffer>(0x202);
            synchronization_op image_op{
                .intents = synchronization_intent::layout_transition | synchronization_intent::memory_dependency,
                .kind = resource_kind::image,
                .logical = 3,
                .before = image_state(image_usage::DEPTH_STENCIL_ATTACHMENT, access_type::write),
                .after = image_state(image_usage::SAMPLED, access_type::read),
            };
            image_op.after.image_range = image_subresource_range{
                .aspects = image_aspect::depth,
                .base_mip_level = 2,
                .mip_level_count = 3,
                .base_array_layer = 4,
                .array_layer_count = 2,
            };

            synchronization_op buffer_op{
                .intents = synchronization_intent::execution_dependency | synchronization_intent::memory_dependency |
                           synchronization_intent::queue_ownership,
                .kind = resource_kind::buffer,
                .logical = 5,
                .before = buffer_state(buffer_usage::TRANSFER_DST, access_type::write, pipeline_domain::copy),
                .after = buffer_state(buffer_usage::VERTEX_BUFFER, access_type::read, pipeline_domain::graphics),
            };
            buffer_op.before.queue = queue_class::copy;
            buffer_op.after.queue = queue_class::graphics;
            buffer_op.after.buffer_range = {.offset = 64, .size = 128};

            synchronization_op alias_op{
                .intents = synchronization_intent::aliasing | synchronization_intent::memory_dependency,
                .kind = resource_kind::image,
                .logical = 7,
                .previous_logical = 1,
            };

            const std::vector operations{image_op, buffer_op, alias_op};
            vk_barrier_batch batch;
            const bool built = build_vk_barrier_batch(
                operations,
                vk_queue_family_indices{.graphics = 2, .compute = 3, .copy = 4},
                [&](image_handle logical) { return logical == image_handle{3} ? image : VK_NULL_HANDLE; },
                [&](buffer_handle logical) { return logical == buffer_handle{5} ? buffer : VK_NULL_HANDLE; },
                batch);
            RG_CHECK(built);
            RG_CHECK(batch.image_barriers.size() == 1);
            RG_CHECK(batch.buffer_barriers.size() == 1);
            RG_CHECK(batch.memory_barriers.size() == 1);

            const auto& image_barrier = batch.image_barriers.front();
            RG_CHECK(image_barrier.image == image);
            RG_CHECK(image_barrier.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
            RG_CHECK(image_barrier.subresourceRange.baseMipLevel == 2);
            RG_CHECK(image_barrier.subresourceRange.levelCount == 3);
            RG_CHECK(image_barrier.subresourceRange.baseArrayLayer == 4);
            RG_CHECK(image_barrier.subresourceRange.layerCount == 2);

            const auto& buffer_barrier = batch.buffer_barriers.front();
            RG_CHECK(buffer_barrier.buffer == buffer);
            RG_CHECK(buffer_barrier.offset == 64);
            RG_CHECK(buffer_barrier.size == 128);
            RG_CHECK(buffer_barrier.srcQueueFamilyIndex == 4);
            RG_CHECK(buffer_barrier.dstQueueFamilyIndex == 2);
            RG_CHECK(buffer_barrier.srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            RG_CHECK(buffer_barrier.dstStageMask == VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
            const auto dependency = batch.dependency_info();
            RG_CHECK(dependency.imageMemoryBarrierCount == 1);
            RG_CHECK(dependency.bufferMemoryBarrierCount == 1);
            RG_CHECK(dependency.memoryBarrierCount == 1);

            auto release_op = buffer_op;
            release_op.phase = synchronization_phase::release;
            RG_CHECK(build_vk_barrier_batch(
                std::span<const synchronization_op>(&release_op, 1),
                vk_queue_family_indices{.graphics = 2, .compute = 3, .copy = 4},
                [](image_handle) { return VK_NULL_HANDLE; },
                [&](buffer_handle) { return buffer; },
                batch));
            RG_CHECK(batch.buffer_barriers.front().srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            RG_CHECK(batch.buffer_barriers.front().dstStageMask == VK_PIPELINE_STAGE_2_NONE);
            RG_CHECK(batch.buffer_barriers.front().dstAccessMask == VK_ACCESS_2_NONE);

            auto acquire_op = buffer_op;
            acquire_op.phase = synchronization_phase::acquire;
            RG_CHECK(build_vk_barrier_batch(
                std::span<const synchronization_op>(&acquire_op, 1),
                vk_queue_family_indices{.graphics = 2, .compute = 3, .copy = 4},
                [](image_handle) { return VK_NULL_HANDLE; },
                [&](buffer_handle) { return buffer; },
                batch));
            RG_CHECK(batch.buffer_barriers.front().srcStageMask == VK_PIPELINE_STAGE_2_NONE);
            RG_CHECK(batch.buffer_barriers.front().srcAccessMask == VK_ACCESS_2_NONE);
            RG_CHECK(batch.buffer_barriers.front().dstStageMask == VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);

        }

        void depth_stencil_and_same_layout_test()
        {
            abstract_resource_state depth = image_state(image_usage::SAMPLED, access_type::read);
            depth.image_range.aspects = image_aspect::depth;
            abstract_resource_state stencil = depth;
            stencil.image_range.aspects = image_aspect::stencil;
            RG_CHECK(lower_vk_subresource_range(depth).aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
            RG_CHECK(lower_vk_subresource_range(stencil).aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT);

            synchronization_op storage_raw{
                .intents = synchronization_intent::execution_dependency | synchronization_intent::memory_dependency,
                .kind = resource_kind::image,
                .logical = 0,
                .before = image_state(image_usage::STORAGE, access_type::write, pipeline_domain::compute),
                .after = image_state(image_usage::STORAGE, access_type::read, pipeline_domain::compute),
            };
            vk_barrier_batch batch;
            RG_CHECK(build_vk_barrier_batch(
                std::span<const synchronization_op>(&storage_raw, 1),
                {},
                [](image_handle) { return fake_handle<VkImage>(0x303); },
                [](buffer_handle) { return VK_NULL_HANDLE; },
                batch));
            RG_CHECK(batch.image_barriers.size() == 1);
            RG_CHECK(batch.image_barriers.front().oldLayout == VK_IMAGE_LAYOUT_GENERAL);
            RG_CHECK(batch.image_barriers.front().newLayout == VK_IMAGE_LAYOUT_GENERAL);
            RG_CHECK(batch.image_barriers.front().srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            RG_CHECK(batch.image_barriers.front().dstAccessMask == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }

        void invalid_binding_test()
        {
            synchronization_op operation{
                .intents = synchronization_intent::layout_transition,
                .kind = resource_kind::image,
                .logical = 42,
                .after = image_state(image_usage::COLOR_ATTACHMENT, access_type::write),
            };
            vk_barrier_batch batch;
            RG_CHECK(!build_vk_barrier_batch(
                std::span<const synchronization_op>(&operation, 1),
                {},
                [](image_handle) { return VK_NULL_HANDLE; },
                [](buffer_handle) { return VK_NULL_HANDLE; },
                batch));

            vk_backend backend;
            VkCommandBuffer command_buffer = VK_NULL_HANDLE;
            RG_CHECK(!backend.emit_barriers(command_buffer, std::span<const synchronization_op>(&operation, 1)));
            RG_CHECK(!backend.get_last_error().empty());
        }
    }

    void vulkan_barrier_lowering_test()
    {
        state_mapping_test();
        range_and_batch_test();
        depth_stencil_and_same_layout_test();
        invalid_binding_test();
    }
}
