#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "render_graph/resource_types.h"
#include "render_graph/diagnostic.h"
#include "render_graph/backend/vulkan/surface_provider.h"

namespace render_graph
{
    struct vk_runtime_config
    {
        std::string application_name = "RenderGraph";
        uint32_t frames_in_flight = 3;
        bool validation = false;
        vk_surface_provider surface;
        diagnostic_sink diagnostics;
    };

    struct vk_runtime_result
    {
        std::string error;
        [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
    };

    enum class vk_resize_status : uint8_t { resized, skipped, failed };
    struct vk_resize_result
    {
        vk_resize_status status = vk_resize_status::resized;
        std::string error;
        [[nodiscard]] explicit operator bool() const noexcept { return status != vk_resize_status::failed; }
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

    struct vk_upload_checkpoint
    {
        std::size_t buffer_copy_count = 0;
        std::size_t image_copy_count = 0;
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

    enum class vk_bindless_table_kind : uint8_t
    {
        sampled_images = 0,
        samplers,
        storage_images,
        uniform_buffers,
        storage_buffers,
    };

    struct vk_bindless_handle
    {
        uint32_t index = 0;
        uint32_t generation = 0;
        vk_bindless_table_kind table = vk_bindless_table_kind::sampled_images;
        [[nodiscard]] explicit operator bool() const noexcept { return generation != 0; }
    };

    struct vk_bindless_slot_row
    {
        uint32_t generation = 1;
        uint64_t safe_after_submission = 0;
        bool occupied = false;
        VkImageView owned_view = VK_NULL_HANDLE;
        VkSampler owned_sampler = VK_NULL_HANDLE;
    };

    struct vk_bindless_statistics
    {
        uint64_t slot_allocations = 0;
        uint64_t descriptor_updates = 0;
        uint64_t slot_reuses = 0;
    };

    struct vk_bindless_state
    {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkSampler default_sampler = VK_NULL_HANDLE;
        VkImageView default_white_view = VK_NULL_HANDLE;
        VkImageView default_normal_view = VK_NULL_HANDLE;
        VkImageView default_storage_view = VK_NULL_HANDLE;
        vk_image_resource_handle default_white_image;
        vk_image_resource_handle default_normal_image;
        vk_image_resource_handle default_storage_image;
        vk_buffer_resource_handle default_buffer;
        std::vector<vk_bindless_slot_row> sampled_images;
        std::vector<vk_bindless_slot_row> samplers;
        std::vector<vk_bindless_slot_row> storage_images;
        std::vector<vk_bindless_slot_row> uniform_buffers;
        std::vector<vk_bindless_slot_row> storage_buffers;
        vk_bindless_statistics statistics;
    };

    struct vk_sampler_desc
    {
        VkFilter min_filter = VK_FILTER_LINEAR;
        VkFilter mag_filter = VK_FILTER_LINEAR;
        VkSamplerAddressMode address_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerAddressMode address_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        float max_lod = 0.0F;
    };

    struct vk_shader_stage_row
    {
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
        std::vector<uint32_t> spirv;
        std::string entry = "main";
    };

    struct vk_vertex_layout_row
    {
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
    };

    struct vk_raster_state_row
    {
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
        VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
        VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        bool depth_test = true;
        bool depth_write = true;
        bool blend = false;
    };

    struct vk_graphics_pipeline_desc
    {
        std::vector<vk_shader_stage_row> shaders;
        vk_vertex_layout_row vertex_layout;
        vk_raster_state_row raster;
        std::vector<VkFormat> color_formats;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        std::vector<VkPushConstantRange> push_constants;
    };

    struct vk_compute_pipeline_desc
    {
        vk_shader_stage_row shader{.stage = VK_SHADER_STAGE_COMPUTE_BIT};
        std::vector<VkPushConstantRange> push_constants;
    };

    struct vk_pipeline_handle
    {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return index != UINT32_MAX; }
    };

    struct vk_pipeline_row
    {
        uint64_t key = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        uint32_t generation = 1;
        bool alive = false;
    };

    struct vk_pipeline_table
    {
        VkPipelineCache cache = VK_NULL_HANDLE;
        std::vector<vk_pipeline_row> rows;
        uint64_t creations = 0;
        uint64_t cache_hits = 0;
    };

    struct vk_indirect_group_row
    {
        vk_pipeline_handle pipeline;
        VkDeviceSize command_offset = 0;
        uint32_t command_count = 0;
    };

    struct vk_indexed_scene_record
    {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkExtent2D extent{};
        VkBuffer geometry = VK_NULL_HANDLE;
        VkBuffer indirect = VK_NULL_HANDLE;
        VkIndexType index_type = VK_INDEX_TYPE_UINT32;
        std::span<const std::byte> push_constants;
        VkShaderStageFlags push_stages = 0;
        std::span<const vk_indirect_group_row> groups;
        uint32_t indirect_stride = sizeof(VkDrawIndexedIndirectCommand);
    };

    struct vk_indexed_indirect_draw_row
    {
        vk_pipeline_handle pipeline;
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VkDeviceSize vertex_offset = 0;
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkDeviceSize index_offset = 0;
        VkIndexType index_type = VK_INDEX_TYPE_UINT32;
        VkBuffer indirect_buffer = VK_NULL_HANDLE;
        VkDeviceSize indirect_offset = 0;
        uint32_t draw_count = 0;
        uint32_t stride = sizeof(VkDrawIndexedIndirectCommand);
    };

    struct vk_indexed_indirect_record
    {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkExtent2D extent{};
        std::span<const std::byte> push_constants;
        VkShaderStageFlags push_stages = 0;
        std::span<const vk_indexed_indirect_draw_row> rows;
    };

    struct vk_buffer_copy_command_row
    {
        VkBuffer source = VK_NULL_HANDLE;
        VkBuffer destination = VK_NULL_HANDLE;
        VkDeviceSize source_offset = 0;
        VkDeviceSize destination_offset = 0;
        VkDeviceSize size = 0;
    };

    struct vk_dispatch_command_row
    {
        vk_pipeline_handle pipeline;
        uint32_t x = 1;
        uint32_t y = 1;
        uint32_t z = 1;
        uint32_t push_constant_offset = 0;
        uint32_t push_constant_size = 0;
        VkShaderStageFlags push_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    };

    struct vk_dispatch_record
    {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        std::span<const std::byte> push_constants;
        std::span<const vk_dispatch_command_row> rows;
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
        [[nodiscard]] vk_upload_checkpoint upload_checkpoint() const noexcept;
        void rollback_pending_uploads(vk_upload_checkpoint) noexcept;
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
        [[nodiscard]] vk_runtime_result allocate_sampled_image(VkImageView, VkImageLayout, vk_bindless_handle&);
        [[nodiscard]] vk_runtime_result allocate_sampled_image(vk_image_resource_handle, VkFormat, vk_bindless_handle&);
        [[nodiscard]] vk_runtime_result allocate_sampler(VkSampler, vk_bindless_handle&);
        [[nodiscard]] vk_runtime_result create_sampler(const vk_sampler_desc&, vk_bindless_handle&);
        [[nodiscard]] vk_runtime_result allocate_storage_image(VkImageView, VkImageLayout, vk_bindless_handle&);
        [[nodiscard]] vk_runtime_result allocate_storage_image(vk_image_resource_handle, VkFormat, vk_bindless_handle&);
        [[nodiscard]] vk_runtime_result allocate_uniform_buffer(vk_buffer_resource_handle,
                                                                VkDeviceSize,
                                                                VkDeviceSize,
                                                                vk_bindless_handle&);
        [[nodiscard]] vk_runtime_result allocate_storage_buffer(vk_buffer_resource_handle,
                                                                VkDeviceSize,
                                                                VkDeviceSize,
                                                                vk_bindless_handle&);
        void release_bindless(vk_bindless_handle, uint64_t safe_after_submission);
        [[nodiscard]] bool validate_bindless(vk_bindless_handle) const noexcept;
        [[nodiscard]] VkDescriptorSet bindless_set() const noexcept { return bindless_state_.set; }
        [[nodiscard]] VkDescriptorSetLayout bindless_layout() const noexcept { return bindless_state_.layout; }
        [[nodiscard]] const vk_bindless_state& bindless() const noexcept { return bindless_state_; }
        [[nodiscard]] vk_runtime_result create_graphics_pipeline(const vk_graphics_pipeline_desc&, vk_pipeline_handle&);
        [[nodiscard]] vk_runtime_result create_compute_pipeline(const vk_compute_pipeline_desc&, vk_pipeline_handle&);
        [[nodiscard]] VkPipeline pipeline(vk_pipeline_handle) const noexcept;
        [[nodiscard]] VkPipelineLayout pipeline_layout(vk_pipeline_handle) const noexcept;
        // Immediately destroys a pipeline row created by the current transaction;
        // used by resource-change rollback before the handle is published.
        void destroy_pipeline(vk_pipeline_handle) noexcept;
        [[nodiscard]] bool record_indexed_scene(const vk_indexed_scene_record&);
        [[nodiscard]] bool record_indexed_indirect(const vk_indexed_indirect_record&);
        [[nodiscard]] bool record_buffer_copies(VkCommandBuffer, std::span<const vk_buffer_copy_command_row>);
        [[nodiscard]] bool record_dispatches(const vk_dispatch_record&);
        [[nodiscard]] const vk_pipeline_table& pipelines() const noexcept { return pipeline_table_; }
        [[nodiscard]] bool record_batches(vk_frame_token& token, void* state, vk_record_callback callback);
        [[nodiscard]] bool submit(const vk_frame_token& token);
        [[nodiscard]] vk_frame_status present(const vk_frame_token& token);
        void collect_retired();
        void retire(vk_retirement_row row);
        [[nodiscard]] vk_resize_result resize();
        void wait_idle() noexcept;
        [[nodiscard]] VkDeviceSize min_uniform_buffer_offset_alignment() const noexcept;
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
        [[nodiscard]] bool initialize_bindless();
        void collect_bindless();
        void destroy_bindless() noexcept;
        [[nodiscard]] bool initialize_default_bindless_resources();
        void destroy_pipelines() noexcept;
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
        vk_bindless_state bindless_state_;
        vk_pipeline_table pipeline_table_;
        std::atomic_uint32_t validation_errors_ = 0;
        std::string last_error_;
        bool initialized_ = false;
    };
} // namespace render_graph
