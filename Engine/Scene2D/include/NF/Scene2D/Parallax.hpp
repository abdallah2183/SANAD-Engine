#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>
#include <vector>
#include <string>

namespace nf::scene2d {

struct ParallaxLayer {
    std::string name;
    Vec2 scroll_ratio{1.0f, 1.0f};      // Multiplier relative to camera motion (e.g. 0.2 for distant mountains)
    Vec2 auto_scroll{0.0f, 0.0f};        // Constant scrolling velocity (units/sec, for moving clouds/sky)
    Vec2 tile_size{10.0f, 10.0f};        // Size of repeating tile in world units
    Vec2 offset{0.0f, 0.0f};             // Initial position offset
    f32 depth = 0.0f;                    // Draw depth / sorting layer
    bool repeat_x = true;
    bool repeat_y = false;
};

class ParallaxManager {
public:
    ParallaxManager() = default;

    void add_layer(ParallaxLayer layer);
    [[nodiscard]] size_t layer_count() const noexcept { return m_layers.size(); }
    [[nodiscard]] ParallaxLayer* get_layer(size_t index);
    [[nodiscard]] const ParallaxLayer* get_layer(size_t index) const;

    /// Calculate the effective wrapped position of a layer given camera position and time
    [[nodiscard]] Vec2 calculate_layer_offset(size_t layer_index, const Vec2& camera_pos, f32 total_time) const;

    /// Calculate visible tile indices [min_tile, max_tile] covering camera viewport
    void calculate_visible_tiles(
        size_t layer_index,
        const Vec2& camera_pos,
        const Vec2& viewport_size,
        f32 total_time,
        i32& out_min_tile_x, i32& out_max_tile_x,
        i32& out_min_tile_y, i32& out_max_tile_y
    ) const;

    void clear() { m_layers.clear(); }

private:
    std::vector<ParallaxLayer> m_layers;
};

} // namespace nf::scene2d
