#pragma once

#include <vulkan/vulkan.h>

#include "render_graph/resource_types.h"

namespace render_graph
{
    [[nodiscard]] inline VkFormat lower_vk_format(format value) noexcept
    {
        switch (value)
        {
        case format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case format::R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case format::B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case format::B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case format::D32_SFLOAT: return VK_FORMAT_D32_SFLOAT;
        case format::UNDEFINED: return VK_FORMAT_UNDEFINED;
        }
        return VK_FORMAT_UNDEFINED;
    }

    [[nodiscard]] inline format normalize_vk_format(VkFormat value) noexcept
    {
        switch (value)
        {
        case VK_FORMAT_R8G8B8A8_UNORM: return format::R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return format::R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return format::B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return format::B8G8R8A8_SRGB;
        case VK_FORMAT_D32_SFLOAT: return format::D32_SFLOAT;
        default: return format::UNDEFINED;
        }
    }

    [[nodiscard]] inline VkImageUsageFlags lower_vk_image_usage(image_usage usage) noexcept
    {
        VkImageUsageFlags result = 0;
        if ((usage & image_usage::TRANSFER_SRC) != image_usage::NONE) result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ((usage & image_usage::TRANSFER_DST) != image_usage::NONE) result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((usage & image_usage::SAMPLED) != image_usage::NONE) result |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if ((usage & image_usage::STORAGE) != image_usage::NONE) result |= VK_IMAGE_USAGE_STORAGE_BIT;
        if ((usage & image_usage::COLOR_ATTACHMENT) != image_usage::NONE) result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if ((usage & image_usage::DEPTH_STENCIL_ATTACHMENT) != image_usage::NONE)
            result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return result;
    }

    [[nodiscard]] inline image_usage normalize_vk_image_usage(VkImageUsageFlags usage) noexcept
    {
        image_usage result = image_usage::NONE;
        if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) result = result | image_usage::TRANSFER_SRC;
        if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) result = result | image_usage::TRANSFER_DST;
        if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) result = result | image_usage::SAMPLED;
        if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) result = result | image_usage::STORAGE;
        if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) result = result | image_usage::COLOR_ATTACHMENT;
        if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
            result = result | image_usage::DEPTH_STENCIL_ATTACHMENT;
        return result;
    }

    [[nodiscard]] inline VkBufferUsageFlags lower_vk_buffer_usage(buffer_usage usage) noexcept
    {
        VkBufferUsageFlags result = 0;
        if ((usage & buffer_usage::TRANSFER_SRC) != buffer_usage::NONE) result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if ((usage & buffer_usage::TRANSFER_DST) != buffer_usage::NONE) result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if ((usage & buffer_usage::UNIFORM_BUFFER) != buffer_usage::NONE) result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if ((usage & buffer_usage::STORAGE_BUFFER) != buffer_usage::NONE) result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if ((usage & buffer_usage::INDEX_BUFFER) != buffer_usage::NONE) result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if ((usage & buffer_usage::VERTEX_BUFFER) != buffer_usage::NONE) result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if ((usage & buffer_usage::INDIRECT_BUFFER) != buffer_usage::NONE) result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        return result;
    }

    [[nodiscard]] inline buffer_usage normalize_vk_buffer_usage(VkBufferUsageFlags usage) noexcept
    {
        buffer_usage result = buffer_usage::NONE;
        if ((usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) != 0) result = result | buffer_usage::TRANSFER_SRC;
        if ((usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0) result = result | buffer_usage::TRANSFER_DST;
        if ((usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0) result = result | buffer_usage::UNIFORM_BUFFER;
        if ((usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0) result = result | buffer_usage::STORAGE_BUFFER;
        if ((usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0) result = result | buffer_usage::INDEX_BUFFER;
        if ((usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0) result = result | buffer_usage::VERTEX_BUFFER;
        if ((usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0) result = result | buffer_usage::INDIRECT_BUFFER;
        return result;
    }

    [[nodiscard]] inline image_desc normalize_vk_image_desc(const VkImageCreateInfo& desc) noexcept
    {
        image_flags flags = image_flags::NONE;
        if ((desc.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0) flags = flags | image_flags::CUBE_COMPATIBLE;
        if ((desc.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) flags = flags | image_flags::MUTABLE_FORMAT;
        return image_desc{
            .fmt = normalize_vk_format(desc.format),
            .extent = {.width = desc.extent.width, .height = desc.extent.height, .depth = desc.extent.depth},
            .usage = normalize_vk_image_usage(desc.usage),
            .type = desc.imageType == VK_IMAGE_TYPE_1D ? image_type::TYPE_1D
                  : desc.imageType == VK_IMAGE_TYPE_3D ? image_type::TYPE_3D
                                                       : image_type::TYPE_2D,
            .flags = flags,
            .mip_levels = desc.mipLevels,
            .array_layers = desc.arrayLayers,
            .samples = static_cast<sample_count>(desc.samples),
        };
    }

    [[nodiscard]] inline buffer_desc normalize_vk_buffer_desc(const VkBufferCreateInfo& desc) noexcept
    {
        return buffer_desc{.size = desc.size, .usage = normalize_vk_buffer_usage(desc.usage)};
    }

    [[nodiscard]] inline VkImageCreateInfo lower_vk_image_desc(const image_desc& desc) noexcept
    {
        VkImageCreateFlags flags = 0;
        if ((desc.flags & image_flags::CUBE_COMPATIBLE) != image_flags::NONE) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if ((desc.flags & image_flags::MUTABLE_FORMAT) != image_flags::NONE) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (desc.aliasing != aliasing_policy::forbidden && desc.lifetime == resource_lifetime_class::transient)
            flags |= VK_IMAGE_CREATE_ALIAS_BIT;
        return VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = flags,
            .imageType = desc.type == image_type::TYPE_1D ? VK_IMAGE_TYPE_1D
                       : desc.type == image_type::TYPE_3D ? VK_IMAGE_TYPE_3D
                                                          : VK_IMAGE_TYPE_2D,
            .format = lower_vk_format(desc.fmt),
            .extent = {.width = desc.extent.width, .height = desc.extent.height, .depth = desc.extent.depth},
            .mipLevels = desc.mip_levels,
            .arrayLayers = desc.array_layers,
            .samples = static_cast<VkSampleCountFlagBits>(static_cast<uint32_t>(desc.samples)),
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = lower_vk_image_usage(desc.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
    }

    [[nodiscard]] inline VkBufferCreateInfo lower_vk_buffer_desc(const buffer_desc& desc) noexcept
    {
        return VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = desc.size,
            .usage = lower_vk_buffer_usage(desc.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
    }
} // namespace render_graph
