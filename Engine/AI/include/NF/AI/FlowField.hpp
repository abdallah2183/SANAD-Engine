#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <algorithm>
#include <vector>

namespace nf::ai {

/// Cost representation for terrain traversal
inline constexpr u8 kCostImpassable = 255;
inline constexpr u16 kIntegrationMax = 65535;

/// Discrete cell coordinate in the flow field grid
struct FlowCellCoord {
    i32 x = 0;
    i32 y = 0;

    bool operator==(const FlowCellCoord& o) const noexcept {
        return x == o.x && y == o.y;
    }
};

/// High-performance 2D Flow Field navigation grid for massive crowds (10,000+ agents)
class FlowFieldGrid {
public:
    FlowFieldGrid(
        u32 width = 32,
        u32 height = 32,
        f32 cell_size = 1.0f,
        Vec2 origin = Vec2{0.0f, 0.0f}
    );

    /// Resize grid dimensions and reinitialize buffers
    void resize(u32 width, u32 height, f32 cell_size = 1.0f, Vec2 origin = Vec2{0.0f, 0.0f});

    [[nodiscard]] u32 width() const noexcept { return m_width; }
    [[nodiscard]] u32 height() const noexcept { return m_height; }
    [[nodiscard]] f32 cell_size() const noexcept { return m_cell_size; }
    [[nodiscard]] Vec2 origin() const noexcept { return m_origin; }

    /// Cost field access: 1 = standard flat ground, >1 = rough terrain, 255 = impassable wall
    void set_cost(u32 x, u32 y, u8 cost);
    [[nodiscard]] u8 get_cost(u32 x, u32 y) const;
    void fill_cost(u8 cost);
    void set_blocked(u32 x, u32 y, bool blocked);

    /// Compute integration and flow vector field towards a single target cell
    void compute_field(u32 target_x, u32 target_y);

    /// Compute integration and flow vector field towards multiple simultaneous goals
    void compute_field_multi(const std::vector<FlowCellCoord>& goals);

    /// Query discrete flow direction vector at cell (x, y)
    [[nodiscard]] Vec2 get_vector(u32 x, u32 y) const;

    /// Integration cost (cumulative distance) from cell (x, y) to closest destination
    [[nodiscard]] u16 get_integration(u32 x, u32 y) const;

    /// Continuous world-space query: samples flow direction using bilinear interpolation
    [[nodiscard]] Vec2 sample_flow_world(const Vec2& world_pos) const;

    /// Coordinate transforms
    [[nodiscard]] bool world_to_grid(const Vec2& world_pos, i32& out_x, i32& out_y) const;
    [[nodiscard]] Vec2 grid_to_world(i32 x, i32 y) const;

    [[nodiscard]] bool in_bounds(i32 x, i32 y) const noexcept {
        return x >= 0 && y >= 0 && x < static_cast<i32>(m_width) && y < static_cast<i32>(m_height);
    }

private:
    void generate_integration_field(const std::vector<FlowCellCoord>& goals);
    void generate_vector_field();

    u32 m_width = 32;
    u32 m_height = 32;
    f32 m_cell_size = 1.0f;
    Vec2 m_origin{0.0f, 0.0f};

    std::vector<u8> m_cost_field;
    std::vector<u16> m_integration_field;
    std::vector<Vec2> m_vector_field;
};

} // namespace nf::ai
