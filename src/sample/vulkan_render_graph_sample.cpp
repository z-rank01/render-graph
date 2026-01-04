#include <cstdint>
#include <iostream>
#include <vector>

#include "render_graph/system.h"
#include "render_graph/vulkan_backend.h"

#include <vulkan/vulkan.h>

namespace
{
    struct vk_context
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t graphics_queue_family = 0;
        VkQueue graphics_queue = VK_NULL_HANDLE;
    };

    bool create_instance(VkInstance* out)
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "render-graph-sample";
        app.applicationVersion = 1;
        app.pEngineName = "render-graph";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = nullptr;
        ci.enabledLayerCount = 0;
        ci.ppEnabledLayerNames = nullptr;

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
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 
            .pNext = nullptr, 
            .flags = 0,
            .queueFamilyIndex = queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        VkDeviceCreateInfo dci{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 
            .pNext = nullptr, 
            .flags = 0,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &qci,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = 0,
            .ppEnabledExtensionNames = nullptr,
            .pEnabledFeatures = nullptr,
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
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 
            .pNext = nullptr, 
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D, 
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = VkExtent3D{64, 64, 1}, 
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
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

        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mem_type;

        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(ctx.device, &ai, nullptr, &mem) != VK_SUCCESS)
        {
            vkDestroyImage(ctx.device, image, nullptr);
            return false;
        }
        (void)vkBindImageMemory(ctx.device, image, mem, 0);

        *out_image = image;
        *out_mem = mem;
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
}

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

    render_graph::vk_backend backend;
    if ((vk.device != nullptr) && (vk.physical_device != nullptr))
    {
        backend.set_context(vk.physical_device, vk.device);
    }

    // Create an imported VkImage (acts like swapchain/external input).
    VkImage imported_image = VK_NULL_HANDLE;
    VkDeviceMemory imported_image_mem = VK_NULL_HANDLE;
    if ((vk.device != nullptr) && (vk.physical_device != nullptr))
    {
        (void)create_imported_image(vk, &imported_image, &imported_image_mem);
    }

    render_graph::render_graph_system system;
    system.set_backend(&backend);

    struct state_t
    {
        render_graph::resource_handle g0 = 0;
        render_graph::resource_handle g1 = 0;
        render_graph::resource_handle t0 = 0;
        render_graph::resource_handle l0 = 0;
        render_graph::resource_handle external = 0;
        render_graph::resource_handle final_img = 0;

        render_graph::resource_handle b0 = 0;
        render_graph::resource_handle b1 = 0;
    } state;

    auto noop_execute = [](render_graph::pass_execute_context&) {};

    // Pass 0: create/write g0, g1, b0
    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            state.g0 = ctx.create_image(render_graph::image_info{
                .name          = "g0",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT | render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.g0, render_graph::image_usage::COLOR_ATTACHMENT);

            state.g1 = ctx.create_image(render_graph::image_info{
                .name          = "g1",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT | render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.g1, render_graph::image_usage::COLOR_ATTACHMENT);

            state.b0 = ctx.create_buffer(render_graph::buffer_info{
                .name     = "b0",
                .size     = 4096,
                .usage    = render_graph::buffer_usage::STORAGE_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);
        },
        noop_execute);

    // Pass 1: read g0/g1/b0, rewrite g1, write a short-lived t0 (to be aliased later)
    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g1, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            ctx.write_image(state.g1, render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.write_buffer(state.b0, render_graph::buffer_usage::STORAGE_BUFFER);

            state.t0 = ctx.create_image(render_graph::image_info{
                .name          = "t0",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.t0, render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    // Pass 2: bind/read imported external, create/write l0, and write b1
    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            state.external = ctx.create_image(render_graph::image_info{
                .name          = "external",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 64, .height = 64, .depth = 1},
                .usage         = render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = true,
            });

            if (imported_image)
            {
                backend.bind_imported_image(state.external, static_cast<render_graph::backend::native_handle>(reinterpret_cast<uintptr_t>(imported_image)));
            }

            ctx.read_image(state.external, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);

            state.l0 = ctx.create_image(render_graph::image_info{
                .name          = "l0",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT | render_graph::image_usage::SAMPLED,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.l0, render_graph::image_usage::COLOR_ATTACHMENT);

            state.b1 = ctx.create_buffer(render_graph::buffer_info{
                .name     = "b1",
                .size     = 1024,
                .usage    = render_graph::buffer_usage::UNIFORM_BUFFER,
                .imported = false,
            });
            ctx.write_buffer(state.b1, render_graph::buffer_usage::UNIFORM_BUFFER);
        },
        noop_execute);

    // Pass 3: read l0/g0, write final output.
    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            ctx.read_image(state.l0, render_graph::image_usage::SAMPLED);
            ctx.read_image(state.g0, render_graph::image_usage::SAMPLED);
            ctx.read_buffer(state.b1, render_graph::buffer_usage::UNIFORM_BUFFER);

            state.final_img = ctx.create_image(render_graph::image_info{
                .name          = "final",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 320, .height = 180, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(state.final_img, render_graph::image_usage::COLOR_ATTACHMENT);
            ctx.declare_image_output(state.final_img);
        },
        noop_execute);

    // Pass 4: culled pass (no connection to outputs)
    system.add_pass(
        [&](render_graph::pass_setup_context& ctx)
        {
            const auto trash = ctx.create_image(render_graph::image_info{
                .name          = "trash",
                .fmt           = render_graph::format::R8G8B8A8_UNORM,
                .extent        = {.width = 128, .height = 128, .depth = 1},
                .usage         = render_graph::image_usage::COLOR_ATTACHMENT,
                .type          = render_graph::image_type::TYPE_2D,
                .flags         = render_graph::image_flags::NONE,
                .mip_levels    = 1,
                .array_layers  = 1,
                .sample_counts = 1,
                .imported      = false,
            });
            ctx.write_image(trash, render_graph::image_usage::COLOR_ATTACHMENT);
        },
        noop_execute);

    system.compile();

    std::cout << "vulkan_render_graph_sample: compile OK\n";
    std::cout << "  logical images: " << system.meta_table.image_metas.names.size() << "\n";
    std::cout << "  logical buffers: " << system.meta_table.buffer_metas.names.size() << "\n";
    std::cout << "  physical images: " << system.physical_resource_metas.physical_image_meta.size() << "\n";
    std::cout << "  physical buffers: " << system.physical_resource_metas.physical_buffer_meta.size() << "\n";

    std::cout << "  aliasing (logical->physical):\n";
    std::cout << "    g0      -> img#" << backend.get_physical_image_id(state.g0) << "\n";
    std::cout << "    g1      -> img#" << backend.get_physical_image_id(state.g1) << "\n";
    std::cout << "    t0      -> img#" << backend.get_physical_image_id(state.t0) << "\n";
    std::cout << "    l0      -> img#" << backend.get_physical_image_id(state.l0) << "\n";
    std::cout << "    external-> img#" << backend.get_physical_image_id(state.external) << "\n";
    std::cout << "    final   -> img#" << backend.get_physical_image_id(state.final_img) << "\n";
    std::cout << "    b0      -> buf#" << backend.get_physical_buffer_id(state.b0) << "\n";
    std::cout << "    b1      -> buf#" << backend.get_physical_buffer_id(state.b1) << "\n";

    size_t created_images = 0;
    for (auto *img : backend.images)
    {
        if (img != nullptr) { created_images++; }
    }
    size_t created_buffers = 0;
    for (auto *buf : backend.buffers)
    {
        if (buf != nullptr) { created_buffers++; }
    }
    std::cout << "  backend native handles (non-null): images=" << created_images
              << ", buffers=" << created_buffers << "\n";

    // Expected results (no asserts; verify by reading the printed mapping above):
    // - The pass named "trash" is culled (it does not contribute to declared outputs).
    // - The imported image "external" always maps to its own physical id (never aliases).
    // - The short-lived image "t0" is eligible to alias with later transient images of the
    //   same shape/format/usage if lifetimes do not overlap (greedy first-fit).
    // - Buffers b0 (passes 0-1) and b1 (passes 2-3) have disjoint lifetimes and may alias.

    if (vk.device != nullptr)
    {
        for (size_t i = 0; i < backend.images.size(); i++)
        {
            if (backend.image_memories[i] != nullptr)
            {
                vkFreeMemory(vk.device, backend.image_memories[i], nullptr);
            }
            if ((backend.images[i] != nullptr) && backend.images[i] != imported_image)
            {
                vkDestroyImage(vk.device, backend.images[i], nullptr);
            }
        }
        for (size_t i = 0; i < backend.buffers.size(); i++)
        {
            if (backend.buffer_memories[i] != nullptr)
            {
                vkFreeMemory(vk.device, backend.buffer_memories[i], nullptr);
            }
            if (backend.buffers[i] != nullptr)
            {
                vkDestroyBuffer(vk.device, backend.buffers[i], nullptr);
            }
        }
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
