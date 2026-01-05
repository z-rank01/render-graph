#include <vulkan/vulkan.h>

#include <cstdint>
#include <iostream>
#include <vector>

#include "render_graph/system.h"
#include "render_graph/vk_backend.h"


namespace
{
    struct vk_context
    {
        VkInstance instance              = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device                  = VK_NULL_HANDLE;
        VkQueue graphics_queue           = VK_NULL_HANDLE;
        uint32_t graphics_queue_family   = 0;
    };

    bool create_instance(VkInstance* out)
    {
        VkApplicationInfo app{.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pNext              = nullptr,
                              .pApplicationName   = "render-graph-sample",
                              .applicationVersion = 1,
                              .pEngineName        = "render-graph",
                              .engineVersion      = 1,
                              .apiVersion         = VK_API_VERSION_1_1};

        VkInstanceCreateInfo ci{
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .pApplicationInfo        = &app,
            .enabledLayerCount       = 0,
            .ppEnabledLayerNames     = nullptr,
            .enabledExtensionCount   = 0,
            .ppEnabledExtensionNames = nullptr,
        };

        return vkCreateInstance(&ci, nullptr, out) == VK_SUCCESS;
    }

    bool pick_physical_device(VkInstance instance, VkPhysicalDevice* out)
    {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0)
        {
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS)
        {
            return false;
        }
        *out = devices[0];
        return true;
    }

    bool pick_queue_family(VkPhysicalDevice phys, uint32_t* out_family)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
        if (count == 0)
        {
            return false;
        }
        std::vector<VkQueueFamilyProperties> props(count);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props.data());
        for (uint32_t i = 0; i < count; i++)
        {
            if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                *out_family = i;
                return true;
            }
        }
        *out_family = 0;
        return true;
    }

    bool create_device(VkPhysicalDevice phys, uint32_t queue_family, VkDevice* out_device, VkQueue* out_queue)
    {
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo qci{
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .queueFamilyIndex = queue_family,
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
        VkDeviceCreateInfo dci{
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .queueCreateInfoCount    = 1,
            .pQueueCreateInfos       = &qci,
            .enabledLayerCount       = 0,
            .ppEnabledLayerNames     = nullptr,
            .enabledExtensionCount   = 0,
            .ppEnabledExtensionNames = nullptr,
            .pEnabledFeatures        = nullptr,
        };

        if (vkCreateDevice(phys, &dci, nullptr, out_device) != VK_SUCCESS)
        {
            return false;
        }
        vkGetDeviceQueue(*out_device, queue_family, 0, out_queue);
        return true;
    }

    uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_filter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties mem_props{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        {
            const bool type_ok = (type_filter & (1U << i)) != 0;
            const bool prop_ok = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
            if (type_ok && prop_ok)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool create_imported_image(const vk_context& ctx, VkImage* out_image, VkDeviceMemory* out_mem)
    {
        VkImageCreateInfo ci{
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = VK_FORMAT_R8G8B8A8_UNORM,
            .extent                = VkExtent3D{64, 64, 1},
            .mipLevels             = 1,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(ctx.device, &ci, nullptr, &image) != VK_SUCCESS)
        {
            return false;
        }

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(ctx.device, image, &req);
        const auto mem_type = find_memory_type(ctx.physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mem_type == UINT32_MAX)
        {
            vkDestroyImage(ctx.device, image, nullptr);
            return false;
        }

        VkMemoryAllocateInfo ai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 
            .pNext = nullptr, 
            .allocationSize = req.size, 
            .memoryTypeIndex = mem_type};
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(ctx.device, &ai, nullptr, &mem) != VK_SUCCESS)
        {
            vkDestroyImage(ctx.device, image, nullptr);
            return false;
        }
        (void)vkBindImageMemory(ctx.device, image, mem, 0);

        *out_image = image;
        *out_mem   = mem;
        return true;
    }

    void destroy_context(vk_context& ctx)
    {
        if (ctx.device != nullptr)
        {
            vkDeviceWaitIdle(ctx.device);
            vkDestroyDevice(ctx.device, nullptr);
            ctx.device = VK_NULL_HANDLE;
        }
        if (ctx.instance != nullptr)
        {
            vkDestroyInstance(ctx.instance, nullptr);
            ctx.instance = VK_NULL_HANDLE;
        }
    }
} // namespace

int main()
{
    vk_context vk{};
    if (!create_instance(&vk.instance) || !pick_physical_device(vk.instance, &vk.physical_device) ||
        !pick_queue_family(vk.physical_device, &vk.graphics_queue_family) ||
        !create_device(vk.physical_device, vk.graphics_queue_family, &vk.device, &vk.graphics_queue))
    {
        std::cout << "vulkan_render_graph_sample: Vulkan init failed; will still build/compile graph without creating native resources.\n";
        destroy_context(vk);
    }

    using rg_system_t = render_graph::render_graph_system<render_graph::vk_backend>;

    // Create an imported VkImage (acts like swapchain/external input).
    VkImage imported_image            = VK_NULL_HANDLE;
    VkDeviceMemory imported_image_mem = VK_NULL_HANDLE;
    if ((vk.device != nullptr) && (vk.physical_device != nullptr))
    {
        (void)create_imported_image(vk, &imported_image, &imported_image_mem);
    }

    rg_system_t rg;
    if ((vk.device != nullptr) && (vk.physical_device != nullptr))
    {
        rg.set_backend_context(vk.physical_device, vk.device);
    }

    struct state_t
    {
        render_graph::resource_handle g0        = render_graph::invalid_resource;
        render_graph::resource_handle g1        = render_graph::invalid_resource;
        render_graph::resource_handle t0        = render_graph::invalid_resource;
        render_graph::resource_handle l0        = render_graph::invalid_resource;
        render_graph::resource_handle external  = render_graph::invalid_resource;
        render_graph::resource_handle final_img = render_graph::invalid_resource;
        render_graph::resource_handle b0        = render_graph::invalid_resource;
        render_graph::resource_handle b1        = render_graph::invalid_resource;
    } state;

    auto noop_execute = [](rg_system_t::pass_execute_context&) {};

    const auto make_image = [](VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) -> VkImageCreateInfo
    {
        VkImageCreateInfo ci{};
        ci.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.pNext                 = nullptr;
        ci.flags                 = 0;
        ci.imageType             = VK_IMAGE_TYPE_2D;
        ci.format                = fmt;
        ci.extent                = VkExtent3D{w, h, 1};
        ci.mipLevels             = 1;
        ci.arrayLayers           = 1;
        ci.samples               = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling                = VK_IMAGE_TILING_OPTIMAL;
        ci.usage                 = usage;
        ci.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
        ci.queueFamilyIndexCount = 0;
        ci.pQueueFamilyIndices   = nullptr;
        ci.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;
        return ci;
    };

    const auto make_buffer = [](VkDeviceSize size, VkBufferUsageFlags usage) -> VkBufferCreateInfo
    {
        VkBufferCreateInfo ci{};
        ci.sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.pNext                 = nullptr;
        ci.flags                 = 0;
        ci.size                  = size;
        ci.usage                 = usage;
        ci.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
        ci.queueFamilyIndexCount = 0;
        ci.pQueueFamilyIndices   = nullptr;
        return ci;
    };

    // Pass 0: create/write g0, g1, b0
    rg.add_pass(
        [&](rg_system_t::pass_setup_context& ctx)
        {
            state.g0 = ctx.create_and_write_image(
                "g0",
                make_image(VK_FORMAT_R8G8B8A8_UNORM, 320, 180, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
                render_graph::image_usage::COLOR_ATTACHMENT);

            state.g1 = ctx.create_and_write_image(
                "g1",
                make_image(VK_FORMAT_R8G8B8A8_UNORM, 320, 180, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
                render_graph::image_usage::COLOR_ATTACHMENT);

            state.b0 =
                ctx.create_and_write_buffer("b0", make_buffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), render_graph::buffer_usage::STORAGE_BUFFER);
        },
        noop_execute);

    // Pass 1: read g0/g1/b0, rewrite g1, write a short-lived t0 (to be aliased later)
    rg.add_pass(
        [&](rg_system_t::pass_setup_context& ctx)
        {
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g1, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            ctx.write_image(state.g1, render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.write_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            state.t0 = ctx.create_and_write_image("t0",
                                                  make_image(VK_FORMAT_R8G8B8A8_UNORM, 320, 180, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                                                  render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    // Pass 2: bind/read imported external, create/write l0, and write b1
    rg.add_pass(
        [&](rg_system_t::pass_setup_context& ctx)
        {
            state.external = ctx.create_image(
                "external", make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT), true);

            if (imported_image != VK_NULL_HANDLE)
            {
                rg.bind_imported_image(state.external, imported_image);
            }

            ctx.read_image(state.external, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);

            state.l0 = ctx.create_and_write_image(
                "l0",
                make_image(VK_FORMAT_R8G8B8A8_UNORM, 320, 180, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
                render_graph::image_usage::COLOR_ATTACHMENT);

            state.b1 =
                ctx.create_and_write_buffer("b1", make_buffer(1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT), render_graph::buffer_usage::UNIFORM_BUFFER);
        },
        noop_execute);

    // Pass 3: read l0/g0, write final output.
    rg.add_pass(
        [&](rg_system_t::pass_setup_context& ctx)
        {
            ctx.read_image(state.l0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b1, render_graph::buffer_usage::UNIFORM_BUFFER);

            state.final_img = ctx.create_and_write_image("final",
                                                         make_image(VK_FORMAT_R8G8B8A8_UNORM, 320, 180, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                                                         render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.final_img);
        },
        noop_execute);

    // Pass 4: culled pass (no connection to outputs)
    rg.add_pass(
        [&](rg_system_t::pass_setup_context& ctx)
        {
            const auto trash = ctx.create_image("trash", make_image(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
            ctx.write_image(trash, render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    rg.compile();

    std::cout << "vulkan_render_graph_sample: compile OK\n";

    // Note: this sample focuses on demonstrating the graph API + compilation.
    // Native resource allocation/cleanup is backend-owned and intentionally not exposed here.

    if (vk.device != nullptr)
    {
        if (imported_image_mem != nullptr)
        {
            vkFreeMemory(vk.device, imported_image_mem, nullptr);
        }
        if (imported_image != nullptr)
        {
            vkDestroyImage(vk.device, imported_image, nullptr);
        }
    }

    destroy_context(vk);
    return 0;
}
