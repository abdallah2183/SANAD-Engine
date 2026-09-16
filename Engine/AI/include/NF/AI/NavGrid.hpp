#pragma once

// NF/AI/NavGrid.hpp — grid pathfinding (design doc Section 105-106).
//
// A* over an 8-connected walkability grid with octile heuristics, no
// corner-cutting, and line-of-sight smoothing. Deterministic: a binary heap
// with explicit (f, h, insertion-sequence) tie-breaks, fixed neighbor order,
// so the same grid and query always yield the same waypoints (design 114).
//
// Coordinates: cells are (ix, iz) with ix in [0, width), iz in [0, height).
// Cell centers map to world via origin + (ix + 0.5) * cell_size on X/Z, y = 0.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <cstdint>
#include <vector>

namespace nf::ai {

struct NavCell {
    i32 x = 0, z = 0;
    bool operator==(const NavCell& o) const { return x == o.x && z == o.z; }
};

class NavGrid {
public:
    NavGrid(i32 width, i32 height, float cell_size, Vec3 origin = Vec3{});

    i32 width() const { return m_width; }
    i32 height() const { return m_height; }
    float cell_size() const { return m_cell; }

    void set_walkable(i32 x, i32 z, bool walkable);
    bool is_walkable(i32 x, i32 z) const; // out of bounds = blocked
    void fill_walkable(bool walkable);

    Vec3 cell_center_world(i32 x, i32 z) const;
    bool world_to_cell(Vec3 world, i32& out_x, i32& out_z) const;

    /// A* from start to goal (world space). Returns true with smoothed
    /// world-space waypoints (start first, goal last) — excluding the exact
    /// start point? No: includes both endpoints so followers can track
    /// waypoints[0] immediately. Empty path (start == goal cell) still
    /// returns true with one waypoint.
    bool find_path(Vec3 start, Vec3 goal, std::vector<Vec3>& out_waypoints) const;

    /// Straight-line walkability between two world points (smoothing probe).
    bool has_line_of_sight(Vec3 a, Vec3 b) const;

private:
    i32 m_width = 0, m_height = 0;
    float m_cell = 1.0f;
    Vec3 m_origin;
    std::vector<u8> m_blocked; // 1 = blocked, row-major (z * width + x)
};

} // namespace nf::ai
