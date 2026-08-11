#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "resource_types.h"

namespace render_graph
{
    struct vk_surface_provider
    {
        void* state = nullptr;
        bool (*instance_extensions)(void*, const char* const*&, uint32_t&, std::string&) = nullptr;
        bool (*create_surface)(void*, VkInstance, VkSurfaceKHR&, std::string&) = nullptr;
        VkExtent2D (*drawable_extent)(void*) = nullptr;
    };

    struct vk_runtime_config
    {
        std::string application_name = "RenderGraph";
        uint32_t frames_in_flight = 3;
        bool validation = false;
        vk_surface_provider surface;
    };

    struct vk_runtime_result
    {
        std::string error;
        [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
    };

    enum class vk_frame_status : uint8_t
    {
        ready = 0,
        skipped,
        failed,
    };

    struct vk_device_table
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VmaAllocator allocator = VK_NULL_HANDLE;
    };

    struct vk_queue_row
    {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = VK_QUEUE_FAMILY_IGNORED;
    };

    struct vk_queue_table
    {
        vk_queue_row graphics;
        vk_queue_row compute;
        vk_queue_row copy;
    };

    struct vk_frame_row
    {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t submission = 0;
    };

    struct vk_frame_table
    {
        std::vector<vk_frame_row> rows;
        uint32_t cursor = 0;
        uint64_t next_submission = 1;
        uint64_t completed_submission = 0;
    };

    struct vk_swapchain_image_row
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore render_finished = VK_NULL_HANDLE;
    };

    struct vk_swapchain_image_table
    {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        VkExtent2D extent{};
        std::vector<vk_swapchain_image_row> rows;
    };

    struct vk_buffer_resource_handle
    {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return index != UINT32_MAX; }
        [[nodiscard]] auto operator<=>(const vk_buffer_resource_handle&) const noexcept = default;
    };

    struct vk_image_resource_handle
    {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return index != UINT32_MAX; }
        [[nodiscard]] auto operator<=>(const vk_image_resource_handle&) const noexcept = default;
    };

    struct vk_buffer_slice
    {
        vk_buffer_resource_handle buffer;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
    };

    struct vk_buffer_span
    {
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
    };

    struct vk_buffer_resource_row
    {
        buffer_desc desc;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo allocation_info{};
        void* mapped = nullptr;
        VkDeviceSize cursor = 0;
        uint32_t generation = 1;
        bool alive = false;
        std::vector<vk_buffer_span> free_spans;
    };

    struct vk_buffer_copy_row
    {
        vk_buffer_resource_handle source;
        vk_buffer_resource_handle destination;
        VkBufferCopy region{};
        vk_buffer_slice staging_slice;
    };

    struct vk_buffer_retirement_row
    {
        uint64_t safe_after_submission = 0;
        vk_buffer_slice slice;
    };

    struct vk_buffer_destroy_row
    {
        uint64_t safe_after_submission = 0;
        vk_buffer_resource_handle buffer;
    };

    struct vk_image_resource_row
    {
        image_desc desc;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo allocation_info{};
        uint32_t generation = 1;
        bool alive = false;
    };

    struct vk_image_copy_row
    {
        vk_buffer_resource_handle source;
        vk_image_resource_handle destination;
        VkBufferImageCopy region{};
        vk_buffer_slice staging_slice;
    };

    struct vk_image_destroy_row
    {
        uint64_t safe_after_submission = 0;
        vk_image_resource_handle image;
    };

    struct vk_resource_table
    {
        std::vector<vk_buffer_resource_row> buffers;
        std::vector<vk_image_resource_row> images;
        std::vector<vk_buffer_copy_row> pending_buffer_copies;
        std::vector<vk_image_copy_row> pending_image_copies;
        std::vector<vk_buffer_retirement_row> retired_buffer_slices;
        std::vector<vk_buffer_destroy_row> retired_buffers;
        std::vector<vk_image_destroy_row> retired_images;
        vk_buffer_resource_handle upload_arena;
    };

    struct vk_allocation_table
    {
        std::vector<VmaAllocation> buffer_allocations;
        std::vector<VmaAllocation> image_allocations;
    };

    struct vk_retirement_row
    {
        uint64_t safe_after_submission = 0;
        void* state = nullptr;
        void (*destroy)(void*) noexcept = nullptr;
    };

    struct vk_retirement_table
    {
        std::vector<vk_retirement_row> rows;
    };

    struct vk_frame_token
    {
        uint32_t frame_index = 0;
        uint32_t image_index = 0;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        bool acquire_suboptimal = false;
    };

    struct vk_runtime_statistics
    {
        uint64_t acquired_frames = 0;
        uint64_t recorded_batches = 0;
        uint64_t submitted_frames = 0;
        uint64_t presented_frames = 0;
        uint64_t retired_rows = 0;
    };

    using vk_record_callback = bool (*)(void*, VkCommandBuffer, uint32_t);

    class vk_runtime
    {
    public:
        vk_runtime() = default;
        ~vk_runtime();
        vk_runtime(const vk_runtime&) = delete;
        vk_runtime& operator=(const vk_runtime&) = delete;

        [[nodiscard]] vk_runtime_result initialize(const vk_runtime_config& config);
        [[nodiscard]] vk_frame_status acquire(vk_frame_token& token);
        [[nodiscard]] bool realize_resources();
        [[nodiscard]] vk_runtime_result create_buffer(const buffer_desc&, vk_buffer_resource_handle&);
        [[nodiscard]] VkBuffer buffer(vk_buffer_resource_handle) const noexcept;
        [[nodiscard]] void* mapped_buffer(vk_buffer_resource_handle) const noexcept;
        [[nodiscard]] bool update_buffer(vk_buffer_resource_handle, VkDeviceSize, std::span<const std::byte>);
        [[nodiscard]] bool read_buffer(vk_buffer_resource_handle, VkDeviceSize, std::span<std::byte>);
        [[nodiscard]] bool allocate_buffer_slice(vk_buffer_resource_handle, VkDeviceSize, VkDeviceSize, vk_buffer_slice&);
        [[nodiscard]] bool stage_buffer_upload(vk_buffer_slice destination, std::span<const std::byte> bytes);
        [[nodiscard]] bool has_pending_uploads() const noexcept;
        [[nodiscard]] bool record_pending_uploads(VkCommandBuffer);
        void commit_pending_uploads(uint64_t submission);
        void release_buffer_slice(vk_buffer_slice, uint64_t safe_after_submission);
        void destroy_buffer(vk_buffer_resource_handle, uint64_t safe_after_submission);
        [[nodiscard]] vk_runtime_result create_image(const image_desc&, vk_image_resource_handle&);
        [[nodiscard]] VkImage image(vk_image_resource_handle) const noexcept;
        [[nodiscard]] bool stage_image_upload(vk_image_resource_handle,
                                              uint32_t mip_level,
                                              uint32_t array_layer,
                                              extent_3d,
                                              std::span<const std::byte>);
        void destroy_image(vk_image_resource_handle, uint64_t safe_after_submission);
        [[nodiscard]] bool record_batches(vk_frame_token& token, void* state, vk_record_callback callback);
        [[nodiscard]] bool submit(const vk_frame_token& token);
        [[nodiscard]] vk_frame_status present(const vk_frame_token& token);
        void collect_retired();
        void retire(vk_retirement_row row);
        [[nodiscard]] vk_runtime_result resize();
        void shutdown() noexcept;

        [[nodiscard]] const vk_device_table& devices() const noexcept { return device_table_; }
        [[nodiscard]] const vk_queue_table& queues() const noexcept { return queue_table_; }
        [[nodiscard]] const vk_frame_table& frames() const noexcept { return frame_table_; }
        [[nodiscard]] const vk_swapchain_image_table& swapchain_images() const noexcept { return swapchain_table_; }
        [[nodiscard]] vk_resource_table& resources() noexcept { return resource_table_; }
        [[nodiscard]] vk_allocation_table& allocations() noexcept { return allocation_table_; }
        [[nodiscard]] const vk_runtime_statistics& statistics() const noexcept { return statistics_; }
        [[nodiscard]] uint32_t validation_error_count() const noexcept { return validation_errors_.load(); }
        [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    private:
        [[nodiscard]] bool create_instance();
        [[nodiscard]] bool select_physical_device();
        [[nodiscard]] bool create_device();
        [[nodiscard]] bool create_allocator();
        [[nodiscard]] bool create_swapchain();
        [[nodiscard]] bool create_frame_rows();
        [[nodiscard]] bool ensure_upload_arena();
        [[nodiscard]] vk_buffer_resource_row* find_buffer(vk_buffer_resource_handle) noexcept;
        [[nodiscard]] const vk_buffer_resource_row* find_buffer(vk_buffer_resource_handle) const noexcept;
        [[nodiscard]] vk_image_resource_row* find_image(vk_image_resource_handle) noexcept;
        [[nodiscard]] const vk_image_resource_row* find_image(vk_image_resource_handle) const noexcept;
        void collect_buffer_slices();
        void destroy_resources() noexcept;
        void destroy_swapchain() noexcept;
        void set_error(std::string message);
        static VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                                  const VkDebugUtilsMessengerCallbackDataEXT*,
                                                                  void*);

        vk_runtime_config config_;
        vk_device_table device_table_;
        vk_queue_table queue_table_;
        vk_frame_table frame_table_;
        vk_swapchain_image_table swapchain_table_;
        vk_resource_table resource_table_;
        vk_allocation_table allocation_table_;
        vk_retirement_table retirement_table_;
        vk_runtime_statistics statistics_;
        std::atomic_uint32_t validation_errors_ = 0;
        std::string last_error_;
        bool initialized_ = false;
    };
} // namespace render_graph
