#include "vk_runtime.h"

#include <array>
#include <cstring>
#include <limits>

namespace render_graph
{
    namespace
    {
        constexpr uint32_t sampled_image_capacity = 2048;
        constexpr uint32_t sampler_capacity = 128;
        constexpr uint32_t storage_image_capacity = 512;
        constexpr uint32_t uniform_buffer_capacity = 1024;
        constexpr uint32_t storage_buffer_capacity = 4096;

        std::vector<vk_bindless_slot_row>& slots(vk_bindless_state& state, vk_bindless_table_kind table)
        {
            switch (table)
            {
            case vk_bindless_table_kind::sampled_images: return state.sampled_images;
            case vk_bindless_table_kind::samplers: return state.samplers;
            case vk_bindless_table_kind::storage_images: return state.storage_images;
            case vk_bindless_table_kind::uniform_buffers: return state.uniform_buffers;
            case vk_bindless_table_kind::storage_buffers: return state.storage_buffers;
            }
            return state.sampled_images;
        }

        const std::vector<vk_bindless_slot_row>& slots(const vk_bindless_state& state, vk_bindless_table_kind table)
        {
            return slots(const_cast<vk_bindless_state&>(state), table);
        }

        uint32_t binding(vk_bindless_table_kind table)
        {
            return static_cast<uint32_t>(table);
        }

        VkDescriptorType descriptor_type(vk_bindless_table_kind table)
        {
            switch (table)
            {
            case vk_bindless_table_kind::sampled_images: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case vk_bindless_table_kind::samplers: return VK_DESCRIPTOR_TYPE_SAMPLER;
            case vk_bindless_table_kind::storage_images: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case vk_bindless_table_kind::uniform_buffers: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case vk_bindless_table_kind::storage_buffers: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            return VK_DESCRIPTOR_TYPE_MAX_ENUM;
        }
    } // namespace

    bool vk_runtime::initialize_bindless()
    {
        bindless_state_.sampled_images.resize(sampled_image_capacity);
        bindless_state_.samplers.resize(sampler_capacity);
        bindless_state_.storage_images.resize(storage_image_capacity);
        bindless_state_.uniform_buffers.resize(uniform_buffer_capacity);
        bindless_state_.storage_buffers.resize(storage_buffer_capacity);

        const std::array bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled_image_capacity,
                                         VK_SHADER_STAGE_ALL, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity,
                                         VK_SHADER_STAGE_ALL, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_image_capacity,
                                         VK_SHADER_STAGE_ALL, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniform_buffer_capacity,
                                         VK_SHADER_STAGE_ALL, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storage_buffer_capacity,
                                         VK_SHADER_STAGE_ALL, nullptr},
        };
        constexpr VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                   VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                   VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        const std::array binding_flags{flags, flags, flags, flags, flags};
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(binding_flags.size()),
            .pBindingFlags = binding_flags.data(),
        };
        const VkDescriptorSetLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flag_info,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        if (vkCreateDescriptorSetLayout(device_table_.device, &layout_info, nullptr, &bindless_state_.layout) != VK_SUCCESS)
        {
            set_error("vkCreateDescriptorSetLayout failed for the global bindless ABI");
            return false;
        }
        const std::array pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled_image_capacity},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_image_capacity},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniform_buffer_capacity},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storage_buffer_capacity},
        };
        const VkDescriptorPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets = 1,
            .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
            .pPoolSizes = pool_sizes.data(),
        };
        if (vkCreateDescriptorPool(device_table_.device, &pool_info, nullptr, &bindless_state_.pool) != VK_SUCCESS)
        {
            set_error("vkCreateDescriptorPool failed for the global bindless ABI");
            return false;
        }
        const VkDescriptorSetAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = bindless_state_.pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &bindless_state_.layout,
        };
        if (vkAllocateDescriptorSets(device_table_.device, &allocate_info, &bindless_state_.set) != VK_SUCCESS)
        {
            set_error("vkAllocateDescriptorSets failed for the global bindless ABI");
            return false;
        }
        return initialize_default_bindless_resources();
    }

    bool vk_runtime::initialize_default_bindless_resources()
    {
        const image_desc sampled_desc{
            .fmt = format::R8G8B8A8_UNORM,
            .extent = {1, 1, 1},
            .usage = image_usage::TRANSFER_DST | image_usage::SAMPLED,
            .memory = memory_domain::device_local,
            .aliasing = aliasing_policy::forbidden,
            .lifetime = resource_lifetime_class::persistent,
        };
        const image_desc storage_desc{
            .fmt = format::R8G8B8A8_UNORM,
            .extent = {1, 1, 1},
            .usage = image_usage::TRANSFER_DST | image_usage::STORAGE,
            .memory = memory_domain::device_local,
            .aliasing = aliasing_policy::forbidden,
            .lifetime = resource_lifetime_class::persistent,
        };
        auto white = create_image(sampled_desc, bindless_state_.default_white_image);
        auto normal = create_image(sampled_desc, bindless_state_.default_normal_image);
        auto storage = create_image(storage_desc, bindless_state_.default_storage_image);
        auto empty = create_buffer(buffer_desc{
            .size = 256,
            .usage = buffer_usage::UNIFORM_BUFFER | buffer_usage::STORAGE_BUFFER,
            .memory = memory_domain::upload,
            .mapping = mapping_policy::persistent,
            .aliasing = aliasing_policy::forbidden,
            .lifetime = resource_lifetime_class::persistent,
        }, bindless_state_.default_buffer);
        if (!white || !normal || !storage || !empty)
        {
            set_error("Failed to create bindless default resources");
            return false;
        }
        std::array<std::byte, 256> zero{};
        if (!update_buffer(bindless_state_.default_buffer, 0, zero)) return false;

        const auto make_view = [this](vk_image_resource_handle handle, VkImageView& view)
        {
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image(handle),
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            return vkCreateImageView(device_table_.device, &info, nullptr, &view) == VK_SUCCESS;
        };
        if (!make_view(bindless_state_.default_white_image, bindless_state_.default_white_view) ||
            !make_view(bindless_state_.default_normal_image, bindless_state_.default_normal_view) ||
            !make_view(bindless_state_.default_storage_image, bindless_state_.default_storage_view))
        {
            set_error("Failed to create bindless default image views");
            return false;
        }
        const VkSamplerCreateInfo sampler_info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        if (vkCreateSampler(device_table_.device, &sampler_info, nullptr, &bindless_state_.default_sampler) != VK_SUCCESS)
        {
            set_error("Failed to create bindless default sampler");
            return false;
        }

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = queue_table_.graphics.family,
        };
        const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateCommandPool(device_table_.device, &pool_info, nullptr, &pool) != VK_SUCCESS ||
            vkCreateFence(device_table_.device, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            set_error("Failed to create bindless default transition objects");
            return false;
        }
        const VkCommandBufferAllocateInfo command_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        vkAllocateCommandBuffers(device_table_.device, &command_info, &commands);
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(commands, &begin);
        std::array<VkImageMemoryBarrier2, 3> to_transfer{};
        const std::array images{image(bindless_state_.default_white_image),
                                image(bindless_state_.default_normal_image),
                                image(bindless_state_.default_storage_image)};
        for (uint32_t index = 0; index < images.size(); index++)
        {
            to_transfer[index] = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image = images[index],
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
        }
        const VkDependencyInfo to_transfer_info{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = static_cast<uint32_t>(to_transfer.size()),
            .pImageMemoryBarriers = to_transfer.data(),
        };
        vkCmdPipelineBarrier2(commands, &to_transfer_info);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue white_color{{1.0F, 1.0F, 1.0F, 1.0F}};
        const VkClearColorValue normal_color{{0.5F, 0.5F, 1.0F, 1.0F}};
        const VkClearColorValue zero_color{{0.0F, 0.0F, 0.0F, 0.0F}};
        vkCmdClearColorImage(commands, images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white_color, 1, &range);
        vkCmdClearColorImage(commands, images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &normal_color, 1, &range);
        vkCmdClearColorImage(commands, images[2], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_color, 1, &range);
        std::array<VkImageMemoryBarrier2, 3> to_final{};
        for (uint32_t index = 0; index < images.size(); index++)
        {
            const bool storage_image = index == 2;
            to_final[index] = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = storage_image ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                              : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                .dstAccessMask = storage_image ? (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
                                               : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = storage_image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = images[index],
                .subresourceRange = range,
            };
        }
        const VkDependencyInfo to_final_info{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = static_cast<uint32_t>(to_final.size()),
            .pImageMemoryBarriers = to_final.data(),
        };
        vkCmdPipelineBarrier2(commands, &to_final_info);
        vkEndCommandBuffer(commands);
        const VkCommandBufferSubmitInfo command_submit{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commands,
        };
        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &command_submit,
        };
        const bool submitted = vkQueueSubmit2(queue_table_.graphics.queue, 1, &submit, fence) == VK_SUCCESS &&
                               vkWaitForFences(device_table_.device, 1, &fence, VK_TRUE,
                                               std::numeric_limits<uint64_t>::max()) == VK_SUCCESS;
        vkDestroyFence(device_table_.device, fence, nullptr);
        vkDestroyCommandPool(device_table_.device, pool, nullptr);
        if (!submitted)
        {
            set_error("Failed to initialize bindless default image layouts");
            return false;
        }

        bindless_state_.sampled_images[0].occupied = true;
        bindless_state_.sampled_images[1].occupied = true;
        bindless_state_.samplers[0].occupied = true;
        bindless_state_.storage_images[0].occupied = true;
        bindless_state_.uniform_buffers[0].occupied = true;
        bindless_state_.storage_buffers[0].occupied = true;
        const std::array image_infos{
            VkDescriptorImageInfo{VK_NULL_HANDLE, bindless_state_.default_white_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, bindless_state_.default_normal_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{bindless_state_.default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
            VkDescriptorImageInfo{VK_NULL_HANDLE, bindless_state_.default_storage_view, VK_IMAGE_LAYOUT_GENERAL},
        };
        const VkDescriptorBufferInfo buffer_info{buffer(bindless_state_.default_buffer), 0, 256};
        const std::array writes{
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set, 0, 0, 1,
                                 VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image_infos[0], nullptr, nullptr},
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set, 0, 1, 1,
                                 VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image_infos[1], nullptr, nullptr},
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set, 1, 0, 1,
                                 VK_DESCRIPTOR_TYPE_SAMPLER, &image_infos[2], nullptr, nullptr},
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set, 2, 0, 1,
                                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &image_infos[3], nullptr, nullptr},
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set, 3, 0, 1,
                                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffer_info, nullptr},
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set, 4, 0, 1,
                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffer_info, nullptr},
        };
        vkUpdateDescriptorSets(device_table_.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        bindless_state_.statistics.descriptor_updates += writes.size();
        return true;
    }

    vk_runtime_result vk_runtime::allocate_sampled_image(VkImageView view, VkImageLayout layout, vk_bindless_handle& output)
    {
        auto& table = bindless_state_.sampled_images;
        for (uint32_t index = 2; index < table.size(); index++)
        {
            auto& slot = table[index];
            if (slot.occupied || slot.safe_after_submission > frame_table_.completed_submission) continue;
            if (slot.generation != 1) bindless_state_.statistics.slot_reuses++;
            slot.occupied = true;
            slot.generation++;
            const VkDescriptorImageInfo info{VK_NULL_HANDLE, view, layout};
            const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set,
                                             binding(vk_bindless_table_kind::sampled_images), index, 1,
                                             VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &info, nullptr, nullptr};
            vkUpdateDescriptorSets(device_table_.device, 1, &write, 0, nullptr);
            output = {index, slot.generation, vk_bindless_table_kind::sampled_images};
            bindless_state_.statistics.slot_allocations++;
            bindless_state_.statistics.descriptor_updates++;
            return {};
        }
        return {.error = "Bindless sampled_images table capacity exhausted"};
    }

    vk_runtime_result vk_runtime::allocate_sampled_image(vk_image_resource_handle image_handle,
                                                          VkFormat format,
                                                          vk_bindless_handle& output)
    {
        const VkImage native = image(image_handle);
        if (native == VK_NULL_HANDLE) return {.error = "Cannot bind an invalid sampled image"};
        VkImageView view = VK_NULL_HANDLE;
        const VkImageViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = native,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(device_table_.device, &info, nullptr, &view) != VK_SUCCESS)
            return {.error = "vkCreateImageView failed for sampled image"};
        const auto allocated = allocate_sampled_image(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, output);
        if (!allocated)
        {
            vkDestroyImageView(device_table_.device, view, nullptr);
            return allocated;
        }
        bindless_state_.owned_views.push_back(view);
        return {};
    }

    vk_runtime_result vk_runtime::allocate_sampler(VkSampler sampler, vk_bindless_handle& output)
    {
        auto& table = bindless_state_.samplers;
        for (uint32_t index = 1; index < table.size(); index++)
        {
            auto& slot = table[index];
            if (slot.occupied || slot.safe_after_submission > frame_table_.completed_submission) continue;
            if (slot.generation != 1) bindless_state_.statistics.slot_reuses++;
            slot.occupied = true;
            slot.generation++;
            const VkDescriptorImageInfo info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set,
                                             binding(vk_bindless_table_kind::samplers), index, 1,
                                             VK_DESCRIPTOR_TYPE_SAMPLER, &info, nullptr, nullptr};
            vkUpdateDescriptorSets(device_table_.device, 1, &write, 0, nullptr);
            output = {index, slot.generation, vk_bindless_table_kind::samplers};
            bindless_state_.statistics.slot_allocations++;
            bindless_state_.statistics.descriptor_updates++;
            return {};
        }
        return {.error = "Bindless samplers table capacity exhausted"};
    }

    vk_runtime_result vk_runtime::create_sampler(const vk_sampler_desc& desc, vk_bindless_handle& output)
    {
        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = desc.mag_filter,
            .minFilter = desc.min_filter,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = desc.address_u,
            .addressModeV = desc.address_v,
            .addressModeW = desc.address_v,
            .maxLod = desc.max_lod,
        };
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(device_table_.device, &info, nullptr, &sampler) != VK_SUCCESS)
            return {.error = "vkCreateSampler failed"};
        const auto allocated = allocate_sampler(sampler, output);
        if (!allocated)
        {
            vkDestroySampler(device_table_.device, sampler, nullptr);
            return allocated;
        }
        bindless_state_.owned_samplers.push_back(sampler);
        return {};
    }

    vk_runtime_result vk_runtime::allocate_storage_image(VkImageView view, VkImageLayout layout, vk_bindless_handle& output)
    {
        auto& table = bindless_state_.storage_images;
        for (uint32_t index = 1; index < table.size(); index++)
        {
            auto& slot = table[index];
            if (slot.occupied || slot.safe_after_submission > frame_table_.completed_submission) continue;
            if (slot.generation != 1) bindless_state_.statistics.slot_reuses++;
            slot.occupied = true;
            slot.generation++;
            const VkDescriptorImageInfo info{VK_NULL_HANDLE, view, layout};
            const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_state_.set,
                                             binding(vk_bindless_table_kind::storage_images), index, 1,
                                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &info, nullptr, nullptr};
            vkUpdateDescriptorSets(device_table_.device, 1, &write, 0, nullptr);
            output = {index, slot.generation, vk_bindless_table_kind::storage_images};
            bindless_state_.statistics.slot_allocations++;
            bindless_state_.statistics.descriptor_updates++;
            return {};
        }
        return {.error = "Bindless storage_images table capacity exhausted"};
    }

    vk_runtime_result vk_runtime::allocate_storage_image(vk_image_resource_handle image_handle,
                                                          VkFormat format,
                                                          vk_bindless_handle& output)
    {
        const VkImage native = image(image_handle);
        if (native == VK_NULL_HANDLE) return {.error = "Cannot bind an invalid storage image"};
        VkImageView view = VK_NULL_HANDLE;
        const VkImageViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = native,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(device_table_.device, &info, nullptr, &view) != VK_SUCCESS)
            return {.error = "vkCreateImageView failed for storage image"};
        const auto allocated = allocate_storage_image(view, VK_IMAGE_LAYOUT_GENERAL, output);
        if (!allocated)
        {
            vkDestroyImageView(device_table_.device, view, nullptr);
            return allocated;
        }
        bindless_state_.owned_views.push_back(view);
        return {};
    }

    namespace
    {
        vk_runtime_result allocate_buffer_descriptor(vk_device_table& devices,
                                                     vk_frame_table& frames,
                                                     vk_bindless_state& state,
                                                     vk_bindless_table_kind kind,
                                                     VkBuffer buffer,
                                                     VkDeviceSize offset,
                                                     VkDeviceSize range,
                                                     vk_bindless_handle& output)
        {
            auto& table = slots(state, kind);
            for (uint32_t index = 1; index < table.size(); index++)
            {
                auto& slot = table[index];
                if (slot.occupied || slot.safe_after_submission > frames.completed_submission) continue;
                if (slot.generation != 1) state.statistics.slot_reuses++;
                slot.occupied = true;
                slot.generation++;
                const VkDescriptorBufferInfo info{buffer, offset, range};
                const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, state.set,
                                                 binding(kind), index, 1, descriptor_type(kind), nullptr, &info, nullptr};
                vkUpdateDescriptorSets(devices.device, 1, &write, 0, nullptr);
                output = {index, slot.generation, kind};
                state.statistics.slot_allocations++;
                state.statistics.descriptor_updates++;
                return {};
            }
            return {.error = kind == vk_bindless_table_kind::uniform_buffers
                                 ? "Bindless uniform_buffers table capacity exhausted"
                                 : "Bindless storage_buffers table capacity exhausted"};
        }
    } // namespace

    vk_runtime_result vk_runtime::allocate_uniform_buffer(vk_buffer_resource_handle handle,
                                                          VkDeviceSize offset,
                                                          VkDeviceSize range,
                                                          vk_bindless_handle& output)
    {
        const VkBuffer native = buffer(handle);
        if (native == VK_NULL_HANDLE) return {.error = "Cannot bind an invalid uniform buffer"};
        return allocate_buffer_descriptor(device_table_, frame_table_, bindless_state_,
                                          vk_bindless_table_kind::uniform_buffers, native, offset, range, output);
    }

    vk_runtime_result vk_runtime::allocate_storage_buffer(vk_buffer_resource_handle handle,
                                                          VkDeviceSize offset,
                                                          VkDeviceSize range,
                                                          vk_bindless_handle& output)
    {
        const VkBuffer native = buffer(handle);
        if (native == VK_NULL_HANDLE) return {.error = "Cannot bind an invalid storage buffer"};
        return allocate_buffer_descriptor(device_table_, frame_table_, bindless_state_,
                                          vk_bindless_table_kind::storage_buffers, native, offset, range, output);
    }

    void vk_runtime::release_bindless(vk_bindless_handle handle, uint64_t safe_after_submission)
    {
        auto& table = slots(bindless_state_, handle.table);
        const uint32_t reserved = handle.table == vk_bindless_table_kind::sampled_images ? 2u : 1u;
        if (handle.index < reserved || handle.index >= table.size()) return;
        auto& slot = table[handle.index];
        if (!slot.occupied || slot.generation != handle.generation) return;
        slot.occupied = false;
        slot.safe_after_submission = safe_after_submission;
    }

    bool vk_runtime::validate_bindless(vk_bindless_handle handle) const noexcept
    {
        const auto& table = slots(bindless_state_, handle.table);
        return handle.index < table.size() && table[handle.index].occupied &&
               table[handle.index].generation == handle.generation;
    }

    void vk_runtime::collect_bindless()
    {
        // Reuse eligibility is evaluated directly against completed_submission during allocation.
    }

    void vk_runtime::destroy_bindless() noexcept
    {
        if (device_table_.device == VK_NULL_HANDLE) return;
        if (bindless_state_.default_sampler != VK_NULL_HANDLE)
            vkDestroySampler(device_table_.device, bindless_state_.default_sampler, nullptr);
        for (const auto sampler : bindless_state_.owned_samplers)
            vkDestroySampler(device_table_.device, sampler, nullptr);
        for (const auto view : bindless_state_.owned_views)
            vkDestroyImageView(device_table_.device, view, nullptr);
        if (bindless_state_.default_white_view != VK_NULL_HANDLE)
            vkDestroyImageView(device_table_.device, bindless_state_.default_white_view, nullptr);
        if (bindless_state_.default_normal_view != VK_NULL_HANDLE)
            vkDestroyImageView(device_table_.device, bindless_state_.default_normal_view, nullptr);
        if (bindless_state_.default_storage_view != VK_NULL_HANDLE)
            vkDestroyImageView(device_table_.device, bindless_state_.default_storage_view, nullptr);
        if (bindless_state_.pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_table_.device, bindless_state_.pool, nullptr);
        if (bindless_state_.layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_table_.device, bindless_state_.layout, nullptr);
        bindless_state_ = {};
    }
} // namespace render_graph
