// Unit tests for barrier lowering: logical abstract states map to Vulkan stage
// / access / layout combinations, and synchronization operations produce the
// expected native barrier batches.
#include "vulkan_barrier_lowering_test.h"

#include <cstdint>
#include <type_traits>
#include <vector>

#include "test_check.h"
#include "vk_backend.h"
#include "vk_barrier_lowering.h"

namespace render_graph::unit_test
{
    namespace
    {
        // =========================================================================
        // Test helpers
        // =========================================================================

        // Builds a fake native handle from a plain integer so tests never touch
        // a real Vulkan device.
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

        // Appends one op row to a kind-split table (test-local mirror of the
        // compiler's row writer); the matching before/after range column is
        // selected by the table type at compile time.
        template <typename OpTable>
        void push_op(OpTable& ops, synchronization_phase phase, synchronization_intent intents,
                     resource_handle logical, const abstract_resource_state& before,
                     const abstract_resource_state& after)
        {
            ops.phases.push_back(phase);
            ops.intents.push_back(intents);
            ops.logicals.push_back(logical);
            ops.physicals.push_back(invalid_resource);
            ops.memory_blocks.push_back(invalid_resource);
            ops.previous_logicals.push_back(invalid_resource);
            ops.passes.push_back(invalid_pass);
            ops.source_passes.push_back(invalid_pass);
            ops.before_usage_bits.push_back(before.usage_bits);
            ops.before_accesses.push_back(before.access);
            ops.before_domains.push_back(before.domain);
            ops.before_queues.push_back(before.queue);
            ops.after_usage_bits.push_back(after.usage_bits);
            ops.after_accesses.push_back(after.access);
            ops.after_domains.push_back(after.domain);
            ops.after_queues.push_back(after.queue);
            if constexpr (std::is_same_v<OpTable, image_sync_op_table>)
            {
                ops.before_ranges.push_back(before.image_range);
                ops.after_ranges.push_back(after.image_range);
            }
            else
            {
                ops.before_ranges.push_back(before.buffer_range);
                ops.after_ranges.push_back(after.buffer_range);
            }
        }

        // =========================================================================
        // Test cases
        // =========================================================================

        // Every abstract usage/access pair lowers to the expected native bits.
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

        // Subresource ranges, queue-family transfers, and the release/acquire
        // phases must survive lowering into native barriers.
        void range_and_batch_test()
        {
            const auto image = fake_handle<VkImage>(0x101);
            const auto buffer = fake_handle<VkBuffer>(0x202);
            const vk_queue_family_indices families{.graphics = 2, .compute = 3, .copy = 4};

            // --- Image table: one layout-transition op and one aliasing op ---
            image_sync_op_table image_ops;
            push_op(image_ops, synchronization_phase::full,
                    synchronization_intent::layout_transition | synchronization_intent::memory_dependency,
                    resource_handle{3},
                    image_state(image_usage::DEPTH_STENCIL_ATTACHMENT, access_type::write),
                    image_state(image_usage::SAMPLED, access_type::read));
            image_ops.after_ranges.back() = image_subresource_range{
                .aspects = image_aspect::depth,
                .base_mip_level = 2,
                .mip_level_count = 3,
                .base_array_layer = 4,
                .array_layer_count = 2,
            };
            push_op(image_ops, synchronization_phase::full,
                    synchronization_intent::aliasing | synchronization_intent::memory_dependency,
                    resource_handle{7}, abstract_resource_state{}, abstract_resource_state{});
            image_ops.previous_logicals.back() = resource_handle{1};

            // --- Buffer table: one cross-queue ownership op ---
            buffer_sync_op_table buffer_ops;
            auto buffer_before = buffer_state(buffer_usage::TRANSFER_DST, access_type::write, pipeline_domain::copy);
            buffer_before.queue = queue_class::copy;
            auto buffer_after = buffer_state(buffer_usage::VERTEX_BUFFER, access_type::read, pipeline_domain::graphics);
            buffer_after.queue = queue_class::graphics;
            buffer_after.buffer_range = {.offset = 64, .size = 128};
            push_op(buffer_ops, synchronization_phase::full,
                    synchronization_intent::execution_dependency | synchronization_intent::memory_dependency |
                        synchronization_intent::queue_ownership,
                    resource_handle{5}, buffer_before, buffer_after);

            const auto resolve_image = [&](image_handle logical)
            {
                return logical == image_handle{3} ? image : VK_NULL_HANDLE;
            };
            const auto resolve_buffer = [&](buffer_handle logical)
            {
                return logical == buffer_handle{5} ? vk_native_buffer_range{buffer} : vk_native_buffer_range{};
            };

            // --- Image table lowers to one image barrier + one memory barrier ---
            vk_barrier_batch batch;
            RG_CHECK(build_vk_barrier_batch(image_ops, 0, static_cast<uint32_t>(image_ops.size()), families,
                                            resolve_image, resolve_buffer, batch));
            RG_CHECK(batch.image_barriers.size() == 1);
            RG_CHECK(batch.memory_barriers.size() == 1);
            RG_CHECK(batch.buffer_barriers.empty());

            const auto& image_barrier = batch.image_barriers.front();
            RG_CHECK(image_barrier.image == image);
            RG_CHECK(image_barrier.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
            RG_CHECK(image_barrier.subresourceRange.baseMipLevel == 2);
            RG_CHECK(image_barrier.subresourceRange.levelCount == 3);
            RG_CHECK(image_barrier.subresourceRange.baseArrayLayer == 4);
            RG_CHECK(image_barrier.subresourceRange.layerCount == 2);
            const auto image_dependency = batch.dependency_info();
            RG_CHECK(image_dependency.imageMemoryBarrierCount == 1);
            RG_CHECK(image_dependency.memoryBarrierCount == 1);

            // --- Buffer table lowers to one buffer barrier ---
            RG_CHECK(build_vk_barrier_batch(buffer_ops, 0, static_cast<uint32_t>(buffer_ops.size()), families,
                                            resolve_image, resolve_buffer, batch));
            RG_CHECK(batch.buffer_barriers.size() == 1);
            RG_CHECK(batch.image_barriers.empty());
            RG_CHECK(batch.memory_barriers.empty());

            const auto& buffer_barrier = batch.buffer_barriers.front();
            RG_CHECK(buffer_barrier.buffer == buffer);
            RG_CHECK(buffer_barrier.offset == 64);
            RG_CHECK(buffer_barrier.size == 128);
            RG_CHECK(buffer_barrier.srcQueueFamilyIndex == 4);
            RG_CHECK(buffer_barrier.dstQueueFamilyIndex == 2);
            RG_CHECK(buffer_barrier.srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            RG_CHECK(buffer_barrier.dstStageMask == VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
            const auto buffer_dependency = batch.dependency_info();
            RG_CHECK(buffer_dependency.bufferMemoryBarrierCount == 1);

            // --- The native range offset applies to the logical range ---
            RG_CHECK(build_vk_barrier_batch(buffer_ops, 0, 1, families, resolve_image,
                [&](buffer_handle logical)
                {
                    return logical == buffer_handle{5}
                        ? vk_native_buffer_range{buffer, 4096, 1024}
                        : vk_native_buffer_range{};
                },
                batch));
            RG_CHECK(batch.buffer_barriers.front().buffer == buffer);
            RG_CHECK(batch.buffer_barriers.front().offset == 4096 + 64);
            RG_CHECK(batch.buffer_barriers.front().size == 128);

            // --- Release phase keeps only the source half of the dependency ---
            buffer_ops.phases[0] = synchronization_phase::release;
            RG_CHECK(build_vk_barrier_batch(buffer_ops, 0, 1, families, resolve_image,
                                            [&](buffer_handle) { return buffer; }, batch));
            RG_CHECK(batch.buffer_barriers.front().srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            RG_CHECK(batch.buffer_barriers.front().dstStageMask == VK_PIPELINE_STAGE_2_NONE);
            RG_CHECK(batch.buffer_barriers.front().dstAccessMask == VK_ACCESS_2_NONE);

            // --- Acquire phase keeps only the destination half ---
            buffer_ops.phases[0] = synchronization_phase::acquire;
            RG_CHECK(build_vk_barrier_batch(buffer_ops, 0, 1, families, resolve_image,
                                            [&](buffer_handle) { return buffer; }, batch));
            RG_CHECK(batch.buffer_barriers.front().srcStageMask == VK_PIPELINE_STAGE_2_NONE);
            RG_CHECK(batch.buffer_barriers.front().srcAccessMask == VK_ACCESS_2_NONE);
            RG_CHECK(batch.buffer_barriers.front().dstStageMask == VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
        }

        // Depth/stencil aspects split into separate barriers, and same-layout
        // transitions still carry the memory dependency.
        void depth_stencil_and_same_layout_test()
        {
            abstract_resource_state depth = image_state(image_usage::SAMPLED, access_type::read);
            depth.image_range.aspects = image_aspect::depth;
            abstract_resource_state stencil = depth;
            stencil.image_range.aspects = image_aspect::stencil;
            RG_CHECK(lower_vk_subresource_range(depth).aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
            RG_CHECK(lower_vk_subresource_range(stencil).aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT);

            image_sync_op_table storage_raw;
            push_op(storage_raw, synchronization_phase::full,
                    synchronization_intent::execution_dependency | synchronization_intent::memory_dependency,
                    resource_handle{0},
                    image_state(image_usage::STORAGE, access_type::write, pipeline_domain::compute),
                    image_state(image_usage::STORAGE, access_type::read, pipeline_domain::compute));
            vk_barrier_batch batch;
            RG_CHECK(build_vk_barrier_batch(
                storage_raw, 0, 1,
                {},
                [](image_handle) { return fake_handle<VkImage>(0x303); },
                [](buffer_handle) { return vk_native_buffer_range{}; },
                batch));
            RG_CHECK(batch.image_barriers.size() == 1);
            RG_CHECK(batch.image_barriers.front().oldLayout == VK_IMAGE_LAYOUT_GENERAL);
            RG_CHECK(batch.image_barriers.front().newLayout == VK_IMAGE_LAYOUT_GENERAL);
            RG_CHECK(batch.image_barriers.front().srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            RG_CHECK(batch.image_barriers.front().dstAccessMask == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }

        // Unresolvable bindings fail loudly instead of emitting bad barriers.
        void invalid_binding_test()
        {
            image_sync_op_table operation;
            push_op(operation, synchronization_phase::full,
                    synchronization_intent::layout_transition,
                    resource_handle{42}, abstract_resource_state{},
                    image_state(image_usage::COLOR_ATTACHMENT, access_type::write));
            vk_barrier_batch batch;
            RG_CHECK(!build_vk_barrier_batch(
                operation, 0, 1,
                {},
                [](image_handle) { return VK_NULL_HANDLE; },
                [](buffer_handle) { return vk_native_buffer_range{}; },
                batch));

            vk_graph_executor backend;
            VkCommandBuffer command_buffer = VK_NULL_HANDLE;
            RG_CHECK(!backend.emit_barriers(command_buffer, operation, 0, 1));
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
