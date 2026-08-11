#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "resource.h"

namespace render_graph
{
    enum class pass_kind : uint8_t
    {
        raster = 0,
        compute,
        copy,
    };

    enum class attachment_load_op : uint8_t
    {
        load = 0,
        clear,
        dont_care,
    };

    enum class attachment_store_op : uint8_t
    {
        store = 0,
        dont_care,
    };

    struct clear_value
    {
        std::array<float, 4> color{0, 0, 0, 0};
        float depth = 1.0F;
        uint32_t stencil = 0;
    };

    struct render_area
    {
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct raster_attachment
    {
        image_handle image = invalid_image;
        image_subresource_range subresource{};
        attachment_load_op load = attachment_load_op::dont_care;
        attachment_store_op store = attachment_store_op::store;
        clear_value clear{};
        image_handle resolve_image = invalid_image;
        image_subresource_range resolve_subresource{};
    };

    struct raster_pass_desc
    {
        std::vector<raster_attachment> colors;
        bool has_depth_stencil = false;
        raster_attachment depth_stencil{};
        render_area area{};
        uint32_t layer_count = 1;

        void clear()
        {
            colors.clear();
            has_depth_stencil = false;
            depth_stencil = {};
            area = {};
            layer_count = 1;
        }
    };
}
