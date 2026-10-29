#include <NF/Scene2D/Parallax.hpp>
#include <cmath>
#include <algorithm>

namespace nf::scene2d {

void ParallaxManager::add_layer(ParallaxLayer layer) {
    if (layer.tile_size.x <= 0.0f) layer.tile_size.x = 1.0f;
    if (layer.tile_size.y <= 0.0f) layer.tile_size.y = 1.0f;
    m_layers.push_back(std::move(layer));
}

ParallaxLayer* ParallaxManager::get_layer(size_t index) {
    return (index < m_layers.size()) ? &m_layers[index] : nullptr;
}

const ParallaxLayer* ParallaxManager::get_layer(size_t index) const {
    return (index < m_layers.size()) ? &m_layers[index] : nullptr;
}

Vec2 ParallaxManager::calculate_layer_offset(size_t layer_index, const Vec2& camera_pos, f32 total_time) const {
    const auto* layer = get_layer(layer_index);
    if (!layer) {
        return Vec2{0.0f, 0.0f};
    }

    // Scroll offset = initial offset + camera_pos * (1 - scroll_ratio) + auto_scroll * time
    // When scroll_ratio is 1.0, background moves 1:1 with camera (stays static in world space)
    // When scroll_ratio is 0.0, background stays completely locked to screen
    const f32 ox = layer->offset.x + camera_pos.x * (1.0f - layer->scroll_ratio.x) + layer->auto_scroll.x * total_time;
    const f32 oy = layer->offset.y + camera_pos.y * (1.0f - layer->scroll_ratio.y) + layer->auto_scroll.y * total_time;

    return Vec2{ox, oy};
}

void ParallaxManager::calculate_visible_tiles(
    size_t layer_index,
    const Vec2& camera_pos,
    const Vec2& viewport_size,
    f32 total_time,
    i32& out_min_tile_x, i32& out_max_tile_x,
    i32& out_min_tile_y, i32& out_max_tile_y
) const {
    const auto* layer = get_layer(layer_index);
    if (!layer) {
        out_min_tile_x = out_max_tile_x = 0;
        out_min_tile_y = out_max_tile_y = 0;
        return;
    }

    const Vec2 offset = calculate_layer_offset(layer_index, camera_pos, total_time);
    const f32 half_w = viewport_size.x * 0.5f;
    const f32 half_h = viewport_size.y * 0.5f;

    const f32 view_left = camera_pos.x - half_w;
    const f32 view_right = camera_pos.x + half_w;
    const f32 view_bottom = camera_pos.y - half_h;
    const f32 view_top = camera_pos.y + half_h;

    if (layer->repeat_x && layer->tile_size.x > 0.0f) {
        out_min_tile_x = static_cast<i32>(std::floor((view_left - offset.x) / layer->tile_size.x));
        out_max_tile_x = static_cast<i32>(std::ceil((view_right - offset.x) / layer->tile_size.x));
    } else {
        out_min_tile_x = out_max_tile_x = 0;
    }

    if (layer->repeat_y && layer->tile_size.y > 0.0f) {
        out_min_tile_y = static_cast<i32>(std::floor((view_bottom - offset.y) / layer->tile_size.y));
        out_max_tile_y = static_cast<i32>(std::ceil((view_top - offset.y) / layer->tile_size.y));
    } else {
        out_min_tile_y = out_max_tile_y = 0;
    }
}

} // namespace nf::scene2d
