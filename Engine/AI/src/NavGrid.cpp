// NF/AI/NavGrid.cpp — deterministic A* with LOS smoothing.

#include <NF/AI/NavGrid.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace nf::ai {

namespace {

// Binary min-heap with explicit tie-breaks: (f, h, sequence). The sequence
// counter makes equal-cost expansions deterministic across platforms and
// standard library versions (std::priority_queue gives no such guarantee).
struct HeapItem {
    float f = 0.0f;
    float h = 0.0f;
    u64 seq = 0;
    i32 x = 0, z = 0;
    bool operator<(const HeapItem& o) const {
        // reversed for min-heap use with std::push_heap (max-heap default):
        // "less" means worse (popped later).
        if (f != o.f) return f > o.f;
        if (h != o.h) return h > o.h;
        return seq > o.seq;
    }
};

constexpr float kStraight = 1.0f;
constexpr float kDiagonal = 1.41421356f;

float octile(i32 dx, i32 dz) {
    const i32 a = dx < 0 ? -dx : dx;
    const i32 b = dz < 0 ? -dz : dz;
    const i32 mn = a < b ? a : b;
    return static_cast<float>(a + b) + (kDiagonal - 2.0f) * static_cast<float>(mn);
}

} // namespace

NavGrid::NavGrid(i32 width, i32 height, float cell_size, Vec3 origin)
    : m_width(width > 0 ? width : 1),
      m_height(height > 0 ? height : 1),
      m_cell(cell_size > 0.0f ? cell_size : 1.0f),
      m_origin(origin),
      m_blocked(static_cast<usize>(m_width) * static_cast<usize>(m_height), 0) {}

void NavGrid::set_walkable(i32 x, i32 z, bool walkable) {
    if (x < 0 || z < 0 || x >= m_width || z >= m_height) return;
    m_blocked[static_cast<usize>(z) * static_cast<usize>(m_width) + static_cast<usize>(x)] =
        walkable ? u8{0} : u8{1};
}

bool NavGrid::is_walkable(i32 x, i32 z) const {
    if (x < 0 || z < 0 || x >= m_width || z >= m_height) return false;
    return m_blocked[static_cast<usize>(z) * static_cast<usize>(m_width) + static_cast<usize>(x)] == 0;
}

void NavGrid::fill_walkable(bool walkable) {
    std::fill(m_blocked.begin(), m_blocked.end(), walkable ? u8{0} : u8{1});
}

Vec3 NavGrid::cell_center_world(i32 x, i32 z) const {
    return Vec3{m_origin.x + (static_cast<float>(x) + 0.5f) * m_cell, 0.0f,
                m_origin.z + (static_cast<float>(z) + 0.5f) * m_cell};
}

bool NavGrid::world_to_cell(Vec3 world, i32& out_x, i32& out_z) const {
    out_x = static_cast<i32>(std::floor((world.x - m_origin.x) / m_cell));
    out_z = static_cast<i32>(std::floor((world.z - m_origin.z) / m_cell));
    return out_x >= 0 && out_z >= 0 && out_x < m_width && out_z < m_height;
}

bool NavGrid::has_line_of_sight(Vec3 a, Vec3 b) const {
    // Sampled walkability along the segment: quarter-cell steps catch every
    // cell the segment can pass through. Blocked cells are treated as
    // slightly INFLATED (2% margin): a segment grazing exactly through a
    // grid corner then requires all four corner cells walkable — the same
    // no-corner-cutting rule the A* expansion enforces. Without the margin,
    // smoothing would reintroduce the cuts A* refused to make.
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-9f) return true;
    const float margin = m_cell * 0.02f;
    const float step = m_cell * 0.25f;
    const i32 samples = static_cast<i32>(len / step) + 1;
    for (i32 i = 0; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const float x = a.x + dx * t;
        const float z = a.z + dz * t;
        i32 cx = 0, cz = 0;
        if (!world_to_cell(Vec3{x, 0.0f, z}, cx, cz)) return false;
        if (!is_walkable(cx, cz)) return false;
        const float lx = x - (m_origin.x + static_cast<float>(cx) * m_cell);
        const float lz = z - (m_origin.z + static_cast<float>(cz) * m_cell);
        if (lx < margin && !is_walkable(cx - 1, cz)) return false;
        if (lx > m_cell - margin && !is_walkable(cx + 1, cz)) return false;
        if (lz < margin && !is_walkable(cx, cz - 1)) return false;
        if (lz > m_cell - margin && !is_walkable(cx, cz + 1)) return false;
    }
    return true;
}

bool NavGrid::find_path(Vec3 start, Vec3 goal, std::vector<Vec3>& out_waypoints) const {
    out_waypoints.clear();
    i32 sx = 0, sz = 0, gx = 0, gz = 0;
    if (!world_to_cell(start, sx, sz) || !world_to_cell(goal, gx, gz)) return false;
    if (!is_walkable(sx, sz) || !is_walkable(gx, gz)) return false;
    if (sx == gx && sz == gz) {
        out_waypoints.push_back(cell_center_world(gx, gz));
        return true;
    }

    const usize n = static_cast<usize>(m_width) * static_cast<usize>(m_height);
    constexpr float kInf = std::numeric_limits<float>::infinity();
    std::vector<float> g(n, kInf);
    std::vector<i32> parent(n, -1);
    std::vector<u8> closed(n, 0);
    auto idx = [&](i32 x, i32 z) { return static_cast<usize>(z) * static_cast<usize>(m_width) + static_cast<usize>(x); };

    // Fixed neighbor order (E, W, S, N, then diagonals): expansion order is
    // part of the determinism contract.
    constexpr int kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                 {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

    std::vector<HeapItem> open;
    open.reserve(256);
    u64 seq = 0;
    g[idx(sx, sz)] = 0.0f;
    open.push_back(HeapItem{octile(gx - sx, gz - sz), octile(gx - sx, gz - sz), seq++, sx, sz});
    std::push_heap(open.begin(), open.end());

    bool found = false;
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end());
        const HeapItem cur = open.back();
        open.pop_back();
        const usize ci = idx(cur.x, cur.z);
        if (closed[ci]) continue; // stale heap entry
        closed[ci] = 1;
        if (cur.x == gx && cur.z == gz) {
            found = true;
            break;
        }
        for (const auto& d : kDirs) {
            const i32 nx = cur.x + d[0];
            const i32 nz = cur.z + d[1];
            if (!is_walkable(nx, nz)) continue;
            const bool diagonal = (d[0] != 0 && d[1] != 0);
            if (diagonal && (!is_walkable(cur.x + d[0], cur.z) || !is_walkable(cur.x, cur.z + d[1]))) {
                continue; // no corner cutting
            }
            const usize ni = idx(nx, nz);
            if (closed[ni]) continue;
            const float ng = g[ci] + (diagonal ? kDiagonal : kStraight);
            if (ng < g[ni]) {
                g[ni] = ng;
                parent[ni] = static_cast<i32>(ci);
                const float h = octile(gx - nx, gz - nz);
                open.push_back(HeapItem{ng + h, h, seq++, nx, nz});
                std::push_heap(open.begin(), open.end());
            }
        }
    }
    if (!found) return false;

    // Reconstruct cell path, then greedy LOS smoothing.
    std::vector<NavCell> cells;
    i32 cx = gx, cz = gz;
    while (!(cx == sx && cz == sz)) {
        cells.push_back(NavCell{cx, cz});
        const i32 p = parent[idx(cx, cz)];
        if (p < 0) return false; // unreachable despite `found` (cannot happen)
        cx = p % m_width;
        cz = p / m_width;
    }
    cells.push_back(NavCell{sx, sz});
    std::reverse(cells.begin(), cells.end());

    std::vector<Vec3> centers;
    centers.reserve(cells.size());
    for (const auto& c : cells) centers.push_back(cell_center_world(c.x, c.z));

    // Greedy smoothing: from each kept waypoint jump to the farthest visible.
    std::vector<Vec3> smooth;
    smooth.push_back(centers.front());
    usize anchor = 0;
    while (anchor + 1 < centers.size()) {
        // Farthest visible waypoint; stops at the first invisible one
        // (conservative: never shortcuts around a corner it cannot see).
        usize far = anchor + 1;
        for (usize k = anchor + 2; k < centers.size(); ++k) {
            if (has_line_of_sight(centers[anchor], centers[k])) far = k;
            else break;
        }
        smooth.push_back(centers[far]);
        anchor = far;
    }
    // Pin the exact endpoints (smoothing works on centers; callers track raw
    // start/goal, and a half-cell snap at both ends is noise, not signal).
    if (!smooth.empty()) {
        smooth.front() = start;
        smooth.back() = goal;
    }
    out_waypoints = std::move(smooth);
    return true;
}

} // namespace nf::ai
