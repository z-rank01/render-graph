#include "vk_runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <iostream>
#include <set>
#include <span>
#include <utility>

namespace render_graph
{
    namespace
    {
        [[nodiscard]] bool has_extension(VkPhysicalDevice device, const char* requested)
        {
            uint32_t count = 0;
            if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) return false;
            std::vector<VkExtensionProperties> extensions(count);
            if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
            return std::ranges::any_of(extensions, [requested](const auto& extension)
            {
                return std::strcmp(extension.extensionName, requested) == 0;
            });
        }

        [[nodiscard]] bool present_supported(VkPhysicalDevice device, uint32_t family, VkSurfaceKHR surface)
        {
            VkBool32 supported = VK_FALSE;
            return vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface, &supported) == VK_SUCCESS && supported == VK_TRUE;
        }

        [[nodiscard]] VkSurfaceFormatKHR choose_surface_format(std::span<const VkSurfaceFormatKHR> formats)
        {
            const auto preferred = std::ranges::find_if(formats, [](const auto& candidate)
            {
                return candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
                       candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            return preferred != formats.end() ? *preferred : formats.front();
        }
    } // namespace

    vk_runtime::~vk_runtime()
    {
        shutdown();
    }

    void vk_runtime::set_error(std::string message)
    {
        last_error_ = std::move(message);
    }

    vk_runtime_result vk_runtime::initialize(const vk_runtime_config& config)
    {
        shutdown();
        config_ = config;
        config_.frames_in_flight = std::max(1u, config.frames_in_flight);
        last_error_.clear();
        validation_errors_.store(0);
        statistics_ = {};

        if (config_.surface.instance_extensions == nullptr || config_.surface.create_surface == nullptr ||
            config_.surface.drawable_extent == nullptr)
        {
            return {.error = "Vulkan runtime requires a complete surface provider"};
        }
        if (!create_instance() || !config_.surface.create_surface(config_.surface.state,
                                                                   device_table_.instance,
                                                                   device_table_.surface,
                                                                   last_error_) ||
            !select_physical_device() || !create_device() || !create_allocator() || !create_swapchain() || !create_frame_rows() ||
            !initialize_bindless())
        {
            const std::string error = last_error_.empty() ? "Vulkan runtime initialization failed" : last_error_;
            shutdown();
            return {.error = error};
        }
        initialized_ = true;
        return {};
    }

    bool vk_runtime::create_instance()
    {
        const char* const* provider_extensions = nullptr;
        uint32_t provider_extension_count = 0;
        if (!config_.surface.instance_extensions(config_.surface.state,
                                                  provider_extensions,
                                                  provider_extension_count,
                                                  last_error_))
        {
            return false;
        }
        std::vector<const char*> extensions(provider_extensions, provider_extensions + provider_extension_count);
        std::vector<const char*> layers;
        if (config_.validation)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        const VkApplicationInfo application{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = config_.application_name.c_str(),
            .applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
            .pEngineName = "RenderGraph",
            .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
            .apiVersion = VK_API_VERSION_1_3,
        };
        const VkInstanceCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &application,
            .enabledLayerCount = static_cast<uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        const VkResult created = vkCreateInstance(&create_info, nullptr, &device_table_.instance);
        if (created != VK_SUCCESS)
        {
            set_error("vkCreateInstance failed with VkResult " + std::to_string(created));
            return false;
        }
        if (config_.validation)
        {
            const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device_table_.instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_messenger == nullptr)
            {
                set_error("VK_EXT_debug_utils was enabled but vkCreateDebugUtilsMessengerEXT is unavailable");
                return false;
            }
            const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = &vk_runtime::validation_callback,
                .pUserData = this,
            };
            if (create_messenger(device_table_.instance, &messenger_info, nullptr, &device_table_.debug_messenger) != VK_SUCCESS)
            {
                set_error("vkCreateDebugUtilsMessengerEXT failed");
                return false;
            }
        }
        return true;
    }

    bool vk_runtime::select_physical_device()
    {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(device_table_.instance, &count, nullptr) != VK_SUCCESS || count == 0)
        {
            set_error("No Vulkan physical device is available");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(device_table_.instance, &count, devices.data());
        int best_score = std::numeric_limits<int>::min();
        for (VkPhysicalDevice candidate : devices)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3 || !has_extension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

            VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan13Features features13{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                .pNext = &features12,
            };
            VkPhysicalDeviceFeatures2 features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features13};
            vkGetPhysicalDeviceFeatures2(candidate, &features2);
            if (!features13.synchronization2 || !features13.dynamicRendering ||
                !features12.runtimeDescriptorArray || !features12.descriptorBindingPartiallyBound ||
                !features12.descriptorBindingSampledImageUpdateAfterBind ||
                !features12.descriptorBindingStorageImageUpdateAfterBind ||
                !features12.descriptorBindingUniformBufferUpdateAfterBind ||
                !features12.descriptorBindingStorageBufferUpdateAfterBind ||
                !features12.descriptorBindingUpdateUnusedWhilePending ||
                !features12.shaderSampledImageArrayNonUniformIndexing ||
                !features12.shaderStorageImageArrayNonUniformIndexing ||
                !features12.shaderUniformBufferArrayNonUniformIndexing ||
                !features12.shaderStorageBufferArrayNonUniformIndexing) continue;

            uint32_t queue_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
            uint32_t graphics_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t compute_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t copy_family = VK_QUEUE_FAMILY_IGNORED;
            for (uint32_t family = 0; family < queue_count; family++)
            {
                if (graphics_family == VK_QUEUE_FAMILY_IGNORED && (queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                    present_supported(candidate, family, device_table_.surface)) graphics_family = family;
                if ((queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                    (queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) compute_family = family;
                if ((queues[family].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0 &&
                    (queues[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0) copy_family = family;
            }
            if (graphics_family == VK_QUEUE_FAMILY_IGNORED) continue;
            if (compute_family == VK_QUEUE_FAMILY_IGNORED) compute_family = graphics_family;
            if (copy_family == VK_QUEUE_FAMILY_IGNORED) copy_family = graphics_family;
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 0;
            if (score <= best_score) continue;
            best_score = score;
            device_table_.physical_device = candidate;
            queue_table_.graphics.family = graphics_family;
            queue_table_.compute.family = compute_family;
            queue_table_.copy.family = copy_family;
        }
        if (device_table_.physical_device == VK_NULL_HANDLE)
        {
            set_error("No Vulkan 1.3 device satisfies required features: synchronization2, dynamicRendering, "
                      "runtimeDescriptorArray, descriptorBindingPartiallyBound, "
                      "descriptorBindingSampledImageUpdateAfterBind, descriptorBindingStorageImageUpdateAfterBind, "
                      "descriptorBindingUniformBufferUpdateAfterBind, descriptorBindingStorageBufferUpdateAfterBind, "
                      "descriptorBindingUpdateUnusedWhilePending, shaderSampledImageArrayNonUniformIndexing, "
                      "shaderStorageImageArrayNonUniformIndexing, shaderUniformBufferArrayNonUniformIndexing, "
                      "shaderStorageBufferArrayNonUniformIndexing, swapchain and presentation");
            return false;
        }
        return true;
    }

    bool vk_runtime::create_device()
    {
        const std::set<uint32_t> unique_families{
            queue_table_.graphics.family,
            queue_table_.compute.family,
            queue_table_.copy.family,
        };
        constexpr float priority = 1.0F;
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        queue_infos.reserve(unique_families.size());
        for (uint32_t family : unique_families)
        {
            queue_infos.push_back(VkDeviceQueueCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = family,
                .queueCount = 1,
                .pQueuePriorities = &priority,
            });
        }
        VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        features12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        VkPhysicalDeviceVulkan13Features features13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &features12,
            .synchronization2 = VK_TRUE,
            .dynamicRendering = VK_TRUE,
        };
        constexpr std::array extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const VkDeviceCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features13,
            .queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size()),
            .pQueueCreateInfos = queue_infos.data(),
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        const VkResult created = vkCreateDevice(device_table_.physical_device, &create_info, nullptr, &device_table_.device);
        if (created != VK_SUCCESS)
        {
            set_error("vkCreateDevice failed with VkResult " + std::to_string(created));
            return false;
        }
        vkGetDeviceQueue(device_table_.device, queue_table_.graphics.family, 0, &queue_table_.graphics.queue);
        vkGetDeviceQueue(device_table_.device, queue_table_.compute.family, 0, &queue_table_.compute.queue);
        vkGetDeviceQueue(device_table_.device, queue_table_.copy.family, 0, &queue_table_.copy.queue);
        return true;
    }

    bool vk_runtime::create_allocator()
    {
        const VmaAllocatorCreateInfo create_info{
            .flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT,
            .physicalDevice = device_table_.physical_device,
            .device = device_table_.device,
            .instance = device_table_.instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };
        const VkResult created = vmaCreateAllocator(&create_info, &device_table_.allocator);
        if (created != VK_SUCCESS)
        {
            set_error("vmaCreateAllocator failed with VkResult " + std::to_string(created));
            return false;
        }
        return true;
    }

    bool vk_runtime::create_swapchain()
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_table_.physical_device, device_table_.surface, &capabilities) != VK_SUCCESS)
        {
            set_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
            return false;
        }
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device_table_.physical_device, device_table_.surface, &format_count, nullptr);
        if (format_count == 0)
        {
            set_error("Vulkan surface exposes no formats");
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device_table_.physical_device, device_table_.surface, &format_count, formats.data());
        const VkSurfaceFormatKHR surface_format = choose_surface_format(formats);
        VkExtent2D extent = config_.surface.drawable_extent(config_.surface.state);
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) extent = capabilities.currentExtent;
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        uint32_t image_count = std::max(capabilities.minImageCount, config_.frames_in_flight);
        if (capabilities.maxImageCount != 0) image_count = std::min(image_count, capabilities.maxImageCount);
        const VkSwapchainCreateInfoKHR create_info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = device_table_.surface,
            .minImageCount = image_count,
            .imageFormat = surface_format.format,
            .imageColorSpace = surface_format.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = capabilities.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
        };
        const VkResult created = vkCreateSwapchainKHR(device_table_.device, &create_info, nullptr, &swapchain_table_.swapchain);
        if (created != VK_SUCCESS)
        {
            set_error("vkCreateSwapchainKHR failed with VkResult " + std::to_string(created));
            return false;
        }
        swapchain_table_.format = surface_format.format;
        swapchain_table_.color_space = surface_format.colorSpace;
        swapchain_table_.extent = extent;
        vkGetSwapchainImagesKHR(device_table_.device, swapchain_table_.swapchain, &image_count, nullptr);
        std::vector<VkImage> images(image_count);
        vkGetSwapchainImagesKHR(device_table_.device, swapchain_table_.swapchain, &image_count, images.data());
        swapchain_table_.rows.resize(image_count);
        for (uint32_t index = 0; index < image_count; index++)
        {
            auto& row = swapchain_table_.rows[index];
            row.image = images[index];
            const VkImageViewCreateInfo view_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = row.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = swapchain_table_.format,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateImageView(device_table_.device, &view_info, nullptr, &row.view) != VK_SUCCESS ||
                vkCreateSemaphore(device_table_.device, &semaphore_info, nullptr, &row.render_finished) != VK_SUCCESS)
            {
                set_error("Failed to create swapchain image view or render-finished semaphore");
                return false;
            }
        }
        return true;
    }

    bool vk_runtime::create_frame_rows()
    {
        frame_table_.rows.resize(config_.frames_in_flight);
        for (auto& row : frame_table_.rows)
        {
            const VkCommandPoolCreateInfo pool_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = queue_table_.graphics.family,
            };
            if (vkCreateCommandPool(device_table_.device, &pool_info, nullptr, &row.command_pool) != VK_SUCCESS)
            {
                set_error("vkCreateCommandPool failed");
                return false;
            }
            const VkCommandBufferAllocateInfo allocate_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = row.command_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
            if (vkAllocateCommandBuffers(device_table_.device, &allocate_info, &row.command_buffer) != VK_SUCCESS ||
                vkCreateSemaphore(device_table_.device, &semaphore_info, nullptr, &row.image_available) != VK_SUCCESS ||
                vkCreateFence(device_table_.device, &fence_info, nullptr, &row.fence) != VK_SUCCESS)
            {
                set_error("Failed to create per-frame command or synchronization objects");
                return false;
            }
        }
        return true;
    }

    vk_frame_status vk_runtime::acquire(vk_frame_token& token)
    {
        if (!initialized_ || frame_table_.rows.empty()) return vk_frame_status::failed;
        auto& frame = frame_table_.rows[frame_table_.cursor];
        if (vkWaitForFences(device_table_.device, 1, &frame.fence, VK_TRUE, std::numeric_limits<uint64_t>::max()) != VK_SUCCESS)
        {
            set_error("vkWaitForFences failed");
            return vk_frame_status::failed;
        }
        frame_table_.completed_submission = std::max(frame_table_.completed_submission, frame.submission);
        collect_retired();
        uint32_t image_index = 0;
        const VkResult acquired = vkAcquireNextImageKHR(device_table_.device,
                                                        swapchain_table_.swapchain,
                                                        std::numeric_limits<uint64_t>::max(),
                                                        frame.image_available,
                                                        VK_NULL_HANDLE,
                                                        &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return vk_frame_status::skipped;
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            set_error("vkAcquireNextImageKHR failed with VkResult " + std::to_string(acquired));
            return vk_frame_status::failed;
        }
        token = {
            .frame_index = frame_table_.cursor,
            .image_index = image_index,
            .commands = frame.command_buffer,
            .acquire_suboptimal = acquired == VK_SUBOPTIMAL_KHR,
        };
        statistics_.acquired_frames++;
        return vk_frame_status::ready;
    }

    bool vk_runtime::realize_resources()
    {
        return initialized_;
    }

    bool vk_runtime::record_batches(vk_frame_token& token, void* state, vk_record_callback callback)
    {
        auto& frame = frame_table_.rows[token.frame_index];
        if (vkResetCommandPool(device_table_.device, frame.command_pool, 0) != VK_SUCCESS)
        {
            set_error("vkResetCommandPool failed");
            return false;
        }
        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        if (vkBeginCommandBuffer(frame.command_buffer, &begin_info) != VK_SUCCESS || callback == nullptr ||
            !callback(state, frame.command_buffer, token.image_index) || vkEndCommandBuffer(frame.command_buffer) != VK_SUCCESS)
        {
            set_error("Vulkan record_batches phase failed");
            return false;
        }
        statistics_.recorded_batches++;
        return true;
    }

    bool vk_runtime::submit(const vk_frame_token& token)
    {
        auto& frame = frame_table_.rows[token.frame_index];
        const auto& image = swapchain_table_.rows[token.image_index];
        const VkSemaphoreSubmitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = frame.image_available,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        };
        const VkCommandBufferSubmitInfo commands{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = frame.command_buffer,
        };
        const VkSemaphoreSubmitInfo signal{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = image.render_finished,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        const VkSubmitInfo2 submit_info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &wait,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &commands,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signal,
        };
        if (vkResetFences(device_table_.device, 1, &frame.fence) != VK_SUCCESS ||
            vkQueueSubmit2(queue_table_.graphics.queue, 1, &submit_info, frame.fence) != VK_SUCCESS)
        {
            set_error("Vulkan submit phase failed");
            return false;
        }
        frame.submission = frame_table_.next_submission++;
        statistics_.submitted_frames++;
        return true;
    }

    vk_frame_status vk_runtime::present(const vk_frame_token& token)
    {
        const VkSemaphore semaphore = swapchain_table_.rows[token.image_index].render_finished;
        const VkPresentInfoKHR present_info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &semaphore,
            .swapchainCount = 1,
            .pSwapchains = &swapchain_table_.swapchain,
            .pImageIndices = &token.image_index,
        };
        const VkResult presented = vkQueuePresentKHR(queue_table_.graphics.queue, &present_info);
        frame_table_.cursor = (frame_table_.cursor + 1) % static_cast<uint32_t>(frame_table_.rows.size());
        if (presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR) statistics_.presented_frames++;
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR || token.acquire_suboptimal)
            return vk_frame_status::skipped;
        if (presented != VK_SUCCESS)
        {
            set_error("vkQueuePresentKHR failed with VkResult " + std::to_string(presented));
            return vk_frame_status::failed;
        }
        return vk_frame_status::ready;
    }

    void vk_runtime::retire(vk_retirement_row row)
    {
        retirement_table_.rows.push_back(row);
    }

    void vk_runtime::collect_retired()
    {
        collect_bindless();
        collect_buffer_slices();
        std::size_t kept = 0;
        for (std::size_t index = 0; index < retirement_table_.rows.size(); index++)
        {
            auto& row = retirement_table_.rows[index];
            if (row.safe_after_submission <= frame_table_.completed_submission)
            {
                if (row.destroy != nullptr) row.destroy(row.state);
                statistics_.retired_rows++;
            }
            else
            {
                retirement_table_.rows[kept++] = row;
            }
        }
        retirement_table_.rows.resize(kept);
    }

    vk_resize_result vk_runtime::resize()
    {
        if (!initialized_) return {.status = vk_resize_status::failed, .error = "Vulkan runtime is not initialized"};
        const VkExtent2D requested = config_.surface.drawable_extent(config_.surface.state);
        if (requested.width == 0 || requested.height == 0) return {.status = vk_resize_status::skipped};
        if (vkDeviceWaitIdle(device_table_.device) != VK_SUCCESS)
            return {.status = vk_resize_status::failed, .error = "vkDeviceWaitIdle failed during resize"};
        frame_table_.completed_submission = frame_table_.next_submission - 1;
        collect_retired();
        destroy_swapchain();
        if (!create_swapchain()) return {.status = vk_resize_status::failed, .error = last_error_};
        return {.status = vk_resize_status::resized};
    }

    void vk_runtime::destroy_swapchain() noexcept
    {
        if (device_table_.device == VK_NULL_HANDLE) return;
        for (auto& row : swapchain_table_.rows)
        {
            if (row.render_finished != VK_NULL_HANDLE) vkDestroySemaphore(device_table_.device, row.render_finished, nullptr);
            if (row.view != VK_NULL_HANDLE) vkDestroyImageView(device_table_.device, row.view, nullptr);
        }
        swapchain_table_.rows.clear();
        if (swapchain_table_.swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device_table_.device, swapchain_table_.swapchain, nullptr);
        swapchain_table_.swapchain = VK_NULL_HANDLE;
    }

    void vk_runtime::shutdown() noexcept
    {
        if (device_table_.device != VK_NULL_HANDLE) vkDeviceWaitIdle(device_table_.device);
        frame_table_.completed_submission = std::numeric_limits<uint64_t>::max();
        collect_retired();
        if (device_table_.device != VK_NULL_HANDLE)
        {
            for (auto& row : frame_table_.rows)
            {
                if (row.fence != VK_NULL_HANDLE) vkDestroyFence(device_table_.device, row.fence, nullptr);
                if (row.image_available != VK_NULL_HANDLE) vkDestroySemaphore(device_table_.device, row.image_available, nullptr);
                if (row.command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device_table_.device, row.command_pool, nullptr);
            }
        }
        frame_table_ = {};
        destroy_swapchain();
        destroy_pipelines();
        destroy_bindless();
        destroy_resources();
        if (device_table_.allocator != VK_NULL_HANDLE) vmaDestroyAllocator(device_table_.allocator);
        device_table_.allocator = VK_NULL_HANDLE;
        if (device_table_.device != VK_NULL_HANDLE) vkDestroyDevice(device_table_.device, nullptr);
        device_table_.device = VK_NULL_HANDLE;
        if (device_table_.surface != VK_NULL_HANDLE && device_table_.instance != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(device_table_.instance, device_table_.surface, nullptr);
        device_table_.surface = VK_NULL_HANDLE;
        if (device_table_.debug_messenger != VK_NULL_HANDLE && device_table_.instance != VK_NULL_HANDLE)
        {
            const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device_table_.instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_messenger != nullptr) destroy_messenger(device_table_.instance, device_table_.debug_messenger, nullptr);
        }
        device_table_.debug_messenger = VK_NULL_HANDLE;
        if (device_table_.instance != VK_NULL_HANDLE) vkDestroyInstance(device_table_.instance, nullptr);
        device_table_.instance = VK_NULL_HANDLE;
        device_table_.physical_device = VK_NULL_HANDLE;
        queue_table_ = {};
        resource_table_ = {};
        allocation_table_ = {};
        retirement_table_ = {};
        initialized_ = false;
    }

    void vk_runtime::wait_idle() noexcept
    {
        if (device_table_.device != VK_NULL_HANDLE) (void)vkDeviceWaitIdle(device_table_.device);
    }

    VkDeviceSize vk_runtime::min_uniform_buffer_offset_alignment() const noexcept
    {
        if (device_table_.physical_device == VK_NULL_HANDLE) return 1;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device_table_.physical_device, &properties);
        return properties.limits.minUniformBufferOffsetAlignment;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL vk_runtime::validation_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                                   const VkDebugUtilsMessengerCallbackDataEXT* message,
                                                                   void* state)
    {
        auto* runtime = static_cast<vk_runtime*>(state);
        if (runtime != nullptr && (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
            runtime->validation_errors_.fetch_add(1);
        if (message != nullptr && message->pMessage != nullptr &&
            (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0)
            std::cerr << "[Vulkan] " << message->pMessage << '\n';
        return VK_FALSE;
    }
} // namespace render_graph
