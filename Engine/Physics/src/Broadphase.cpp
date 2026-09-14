#include <NF/Physics/Broadphase.hpp>

#include <algorithm>
#include <cmath>

namespace nf::physics {

namespace {

// A body whose AABB spans more cells than this on any axis is treated as
// unbounded. Without the cap, one bad transform (a NaN, or a shape scaled to
// 1e30) turns insert() into a loop over billions of cells and the process never
// returns.
//
// Checked per axis rather than as a product: with half-extents of 1e12 the
// product of the three spans overflows i64 and wraps to something small, so a
// product test silently lets the oversized body through — which is exactly the
// case the cap exists for.
constexpr i64 kMaxSpanPerAxis = 64;

} // namespace

SpatialGrid::SpatialGrid(f32 cell_size) {
    m_cell_size = (cell_size > 1e-3f) ? cell_size : 1e-3f;
    m_inv_cell_size = 1.0f / m_cell_size;
}

void SpatialGrid::clear() {
    m_cells.clear();
    m_occupied.clear();
    m_unbounded.clear();
    m_bounded_count = 0;
}

u64 SpatialGrid::pack(const CellCoord& c) {
    // 21 bits per axis with a bias so negative coordinates pack without sign
    // tricks and the three fields cannot collide.
    constexpr i64 kBias = 1 << 20;
    constexpr u64 kMask = (1ull << 21) - 1ull;
    const u64 x = static_cast<u64>(static_cast<i64>(c.x) + kBias) & kMask;
    const u64 y = static_cast<u64>(static_cast<i64>(c.y) + kBias) & kMask;
    const u64 z = static_cast<u64>(static_cast<i64>(c.z) + kBias) & kMask;
    return (x << 42) | (y << 21) | z;
}

void SpatialGrid::insert(u32 index, const Aabb& box) {
    // Computed in f64 and validated BEFORE any i32 conversion. A coordinate of
    // 1e12 does not fit in i32, and the cast is undefined behaviour — so a check
    // that runs after the conversion is inspecting garbage. That is exactly how
    // an oversized body slipped past the cap the first time this was written.
    const f64 inv = static_cast<f64>(m_inv_cell_size);
    const f64 lo_x = std::floor(static_cast<f64>(box.min.x) * inv);
    const f64 lo_y = std::floor(static_cast<f64>(box.min.y) * inv);
    const f64 lo_z = std::floor(static_cast<f64>(box.min.z) * inv);
    const f64 hi_x = std::floor(static_cast<f64>(box.max.x) * inv);
    const f64 hi_y = std::floor(static_cast<f64>(box.max.y) * inv);
    const f64 hi_z = std::floor(static_cast<f64>(box.max.z) * inv);

    // `!(x >= 1 && x <= kMax)` rather than `x < 1 || x > kMax` so that a NaN
    // span also fails the check instead of passing both comparisons.
    const f64 span_x = hi_x - lo_x + 1.0;
    const f64 span_y = hi_y - lo_y + 1.0;
    const f64 span_z = hi_z - lo_z + 1.0;
    const f64 kMaxSpan = static_cast<f64>(kMaxSpanPerAxis);
    if (!(span_x >= 1.0 && span_x <= kMaxSpan) ||
        !(span_y >= 1.0 && span_y <= kMaxSpan) ||
        !(span_z >= 1.0 && span_z <= kMaxSpan)) {
        add_unbounded(index);
        return;
    }

    const i32 x0 = static_cast<i32>(lo_x), x1 = static_cast<i32>(hi_x);
    const i32 y0 = static_cast<i32>(lo_y), y1 = static_cast<i32>(hi_y);
    const i32 z0 = static_cast<i32>(lo_z), z1 = static_cast<i32>(hi_z);

    ++m_bounded_count;
    for (i32 z = z0; z <= z1; ++z) {
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const CellCoord c{x, y, z};
                const u64 key = pack(c);
                auto it = m_cells.find(key);
                if (it == m_cells.end()) {
                    m_occupied.push_back(c);
                    m_cells.emplace(key, std::vector<u32>{index});
                } else {
                    it->second.push_back(index);
                }
            }
        }
    }
}

void SpatialGrid::add_unbounded(u32 index) {
    m_unbounded.push_back(index);
}

void SpatialGrid::pairs(std::vector<BroadphasePair>& out) const {
    out.clear();

    for (const CellCoord& c : m_occupied) {
        const auto self_it = m_cells.find(pack(c));
        if (self_it == m_cells.end()) {
            continue;
        }
        const std::vector<u32>& here = self_it->second;

        for (size_t i = 0; i < here.size(); ++i) {
            for (size_t j = i + 1; j < here.size(); ++j) {
                const u32 a = here[i] < here[j] ? here[i] : here[j];
                const u32 b = here[i] < here[j] ? here[j] : here[i];
                out.push_back(BroadphasePair{a, b});
            }
        }

        // Cross pairs with the 13 "forward" neighbours. Bodies in adjacent cells
        // can overlap, so the neighbours are not optional — skipping them is the
        // classic grid-broadphase bug. Visiting only forward offsets means each
        // unordered cell pair is handled once, from the lexicographically
        // smaller cell.
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if (dx < 0 || (dx == 0 && dy < 0) || (dx == 0 && dy == 0 && dz <= 0)) {
                        continue;
                    }
                    const auto it = m_cells.find(pack(CellCoord{c.x + dx, c.y + dy, c.z + dz}));
                    if (it == m_cells.end()) {
                        continue;
                    }
                    for (u32 a : here) {
                        for (u32 b : it->second) {
                            if (a == b) continue;
                            out.push_back(a < b ? BroadphasePair{a, b} : BroadphasePair{b, a});
                        }
                    }
                }
            }
        }
    }

    // Planes and any oversized body have no meaningful cell; they are candidates
    // against everything.
    for (u32 u : m_unbounded) {
        for (const auto& entry : m_cells) {
            for (u32 b : entry.second) {
                if (b == u) continue;
                out.push_back(u < b ? BroadphasePair{u, b} : BroadphasePair{b, u});
            }
        }
    }

    // A body whose AABB spans several cells is registered in each of them, so the
    // same pair is emitted from more than one cell, and the unbounded pass above
    // walks a hash map. Sorting and deduplicating is not an optimisation here —
    // it is what makes the output a *set* with a stable order, which is what the
    // determinism guarantee rests on.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void SpatialGrid::brute_force_pairs(const std::vector<Aabb>& boxes,
                                    std::vector<BroadphasePair>& out) {
    out.clear();
    for (size_t i = 0; i < boxes.size(); ++i) {
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (boxes[i].overlaps(boxes[j])) {
                out.push_back(BroadphasePair{static_cast<u32>(i), static_cast<u32>(j)});
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace nf::physics
