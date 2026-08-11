#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "resource_types.h"

namespace render_graph
{
    template <typename Tag>
    struct device_handle
    {
        uint32_t index = std::numeric_limits<uint32_t>::max();
        uint32_t generation = 0;
        [[nodiscard]] explicit operator bool() const noexcept
        { return index != std::numeric_limits<uint32_t>::max() && generation != 0; }
        [[nodiscard]] auto operator<=>(const device_handle&) const noexcept = default;
    };

    using device_buffer_handle = device_handle<struct buffer_tag>;
    using device_image_handle = device_handle<struct image_tag>;
    using device_sampler_handle = device_handle<struct sampler_tag>;
    using device_pipeline_handle = device_handle<struct pipeline_tag>;
    using device_bindless_handle = device_handle<struct bindless_tag>;

    enum class bindless_table_kind : uint8_t
    {
        sampled_images,
        samplers,
        storage_images,
        uniform_buffers,
        storage_buffers,
    };

    enum class sampler_filter : uint8_t { nearest, linear };
    enum class sampler_address_mode : uint8_t { repeat, mirrored_repeat, clamp_to_edge };
    enum class shader_stage : uint8_t { vertex, fragment, compute };
    enum class shader_binary_format : uint8_t { spirv, dxil, metallib };
    enum class vertex_format : uint8_t
    {
        float2,
        float3,
        float4,
        uint4,
    };
    enum class primitive_topology : uint8_t { triangle_list, line_list, point_list };
    enum class cull_mode : uint8_t { none, front, back };
    enum class front_face : uint8_t { counter_clockwise, clockwise };
    enum class index_format : uint8_t { uint16, uint32 };

    struct sampler_desc
    {
        sampler_filter min_filter = sampler_filter::linear;
        sampler_filter mag_filter = sampler_filter::linear;
        sampler_address_mode address_u = sampler_address_mode::repeat;
        sampler_address_mode address_v = sampler_address_mode::repeat;
        float max_lod = 0.0F;
    };

    struct shader_stage_row
    {
        shader_stage stage = shader_stage::vertex;
        shader_binary_format binary_format = shader_binary_format::spirv;
        std::vector<uint32_t> binary;
        std::string entry = "main";
    };

    struct vertex_binding_row
    {
        uint32_t binding = 0;
        uint32_t stride = 0;
        bool per_instance = false;
    };

    struct vertex_attribute_row
    {
        uint32_t location = 0;
        uint32_t binding = 0;
        vertex_format format = vertex_format::float3;
        uint32_t offset = 0;
    };

    struct push_constant_range
    {
        uint32_t stage_mask = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    inline constexpr uint32_t shader_stage_vertex_bit = 1u << 0;
    inline constexpr uint32_t shader_stage_fragment_bit = 1u << 1;
    inline constexpr uint32_t shader_stage_compute_bit = 1u << 2;

    struct graphics_pipeline_desc
    {
        std::vector<shader_stage_row> shaders;
        std::vector<vertex_binding_row> vertex_bindings;
        std::vector<vertex_attribute_row> vertex_attributes;
        primitive_topology topology = primitive_topology::triangle_list;
        cull_mode cull = cull_mode::back;
        front_face winding = front_face::counter_clockwise;
        bool depth_test = true;
        bool depth_write = true;
        bool blend = false;
        std::vector<format> color_formats;
        format depth_format = format::UNDEFINED;
        sample_count samples = sample_count::x1;
        std::vector<push_constant_range> push_constants;
    };

    struct buffer_create_row { buffer_desc desc; };
    struct image_create_row { image_desc desc; };
    struct sampler_create_row { sampler_desc desc; };
    struct pipeline_create_row { graphics_pipeline_desc desc; };
    struct buffer_upload_row
    {
        device_buffer_handle destination;
        uint64_t offset = 0;
        std::span<const std::byte> bytes;
    };
    struct image_upload_row
    {
        device_image_handle destination;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mip_level = 0;
        std::span<const std::byte> bytes;
    };
    struct bindless_publish_row
    {
        bindless_table_kind table = bindless_table_kind::sampled_images;
        device_buffer_handle buffer;
        device_image_handle image;
        device_sampler_handle sampler;
        uint64_t offset = 0;
        uint64_t size = 0;
    };
    struct resource_retire_row
    {
        device_buffer_handle buffer;
        device_image_handle image;
        device_sampler_handle sampler;
        device_pipeline_handle pipeline;
        device_bindless_handle bindless;
    };

    struct resource_change_batch
    {
        std::span<const buffer_create_row> buffer_creates;
        std::span<const image_create_row> image_creates;
        std::span<const sampler_create_row> sampler_creates;
        std::span<const pipeline_create_row> pipeline_creates;
        std::span<const buffer_upload_row> buffer_uploads;
        std::span<const image_upload_row> image_uploads;
        std::span<const bindless_publish_row> bindless_publishes;
        std::span<const resource_retire_row> retires;
    };

    struct resource_change_result
    {
        std::vector<device_buffer_handle> buffers;
        std::vector<device_image_handle> images;
        std::vector<device_sampler_handle> samplers;
        std::vector<device_pipeline_handle> pipelines;
        std::vector<device_bindless_handle> bindless;
        std::vector<uint32_t> bindless_slots;
        std::string error;
        [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
    };

    struct frame_environment
    {
        extent_3d extent{};
        format color_format = format::UNDEFINED;
        uint32_t frame_index = 0;
        uint64_t submission = 0;
        uint64_t completed_submission = 0;
    };

    struct draw_indexed_indirect_row
    {
        device_pipeline_handle pipeline;
        device_buffer_handle vertex_buffer;
        uint64_t vertex_offset = 0;
        device_buffer_handle index_buffer;
        uint64_t index_offset = 0;
        device_buffer_handle indirect_buffer;
        uint64_t indirect_offset = 0;
        uint32_t draw_count = 0;
        uint32_t stride = 0;
        index_format indices = index_format::uint32;
    };

    struct indexed_indirect_command
    {
        uint32_t index_count = 0;
        uint32_t instance_count = 0;
        uint32_t first_index = 0;
        int32_t vertex_offset = 0;
        uint32_t first_instance = 0;
    };

    struct copy_buffer_row
    {
        device_buffer_handle source;
        device_buffer_handle destination;
        uint64_t source_offset = 0;
        uint64_t destination_offset = 0;
        uint64_t size = 0;
    };

    struct dispatch_row { uint32_t x = 1; uint32_t y = 1; uint32_t z = 1; };

    struct frame_plan
    {
        uint64_t cache_key = 0;
        std::string pass_name = "RasterPass";
        std::array<float, 4> clear_color{0.0F, 0.0F, 0.0F, 1.0F};
        bool depth_attachment = true;
        std::span<const std::byte> push_constants;
        uint32_t push_constant_stage_mask = 0;
        std::span<const draw_indexed_indirect_row> indexed_indirect_draws;
        std::span<const copy_buffer_row> buffer_copies;
        std::span<const dispatch_row> dispatches;
    };

    struct frame_build_result
    {
        std::string error;
        [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
    };

    struct frame_recipe
    {
        void* state = nullptr;
        frame_build_result (*build)(void*, const frame_environment&, frame_plan&) = nullptr;
    };

    enum class frame_status : uint8_t { rendered, skipped, failed };
    struct frame_result { frame_status status = frame_status::failed; std::string error; };
    struct render_statistics
    {
        uint64_t graph_compiles = 0;
        uint64_t upload_pass_executions = 0;
        uint64_t draw_pass_executions = 0;
        uint64_t presented_frames = 0;
        uint64_t descriptor_updates = 0;
        uint64_t pipeline_creations = 0;
        uint64_t indirect_groups = 0;
    };

    struct render_device_api
    {
        resource_change_result (*apply_resource_changes)(void*, const resource_change_batch&);
        frame_result (*render)(void*, const frame_recipe&);
        void (*request_resize)(void*) noexcept;
        void (*shutdown)(void*) noexcept;
        render_statistics (*statistics)(const void*) noexcept;
        uint32_t (*validation_error_count)(const void*) noexcept;
        void (*destroy)(void*) noexcept;
    };

    class render_device
    {
    public:
        render_device() = default;
        render_device(void* value, const render_device_api* functions) : state_(value), api_(functions) {}
        ~render_device() { reset(); }
        render_device(const render_device&) = delete;
        render_device& operator=(const render_device&) = delete;
        render_device(render_device&& other) noexcept : state_(other.state_), api_(other.api_)
        { other.state_ = nullptr; other.api_ = nullptr; }
        render_device& operator=(render_device&& other) noexcept
        {
            if (this != &other)
            {
                reset(); state_ = other.state_; api_ = other.api_;
                other.state_ = nullptr; other.api_ = nullptr;
            }
            return *this;
        }
        [[nodiscard]] explicit operator bool() const noexcept { return state_ && api_; }
        [[nodiscard]] resource_change_result apply_resource_changes(const resource_change_batch& batch)
        { return api_->apply_resource_changes(state_, batch); }
        [[nodiscard]] frame_result render(const frame_recipe& recipe) { return api_->render(state_, recipe); }
        void request_resize() noexcept { api_->request_resize(state_); }
        void shutdown() noexcept { if (state_) api_->shutdown(state_); }
        [[nodiscard]] render_statistics statistics() const noexcept { return api_->statistics(state_); }
        [[nodiscard]] uint32_t validation_error_count() const noexcept { return api_->validation_error_count(state_); }
    private:
        void reset() noexcept
        {
            if (state_ && api_ && api_->destroy) api_->destroy(state_);
            state_ = nullptr; api_ = nullptr;
        }
        void* state_ = nullptr;
        const render_device_api* api_ = nullptr;
    };
} // namespace render_graph
