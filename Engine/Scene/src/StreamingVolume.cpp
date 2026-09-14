// NF/Scene/src/StreamingVolume.cpp — world streaming grid (Phase 11, W5)

#include <NF/Scene/StreamingVolume.hpp>

#include <algorithm>

namespace nf::scene {

void chunks_in_radius(const Vec3& center, f32 radius, f32 chunk_size,
                      std::vector<ChunkCoord>& out) {
    out.clear();
    if (!(chunk_size > 0.0f) || !(radius >= 0.0f)) return;

    // The sphere's AABB in grid space. Walking the box and testing each cell is
    // what makes the cost follow the radius rather than a hardcoded neighbourhood
    // size — a streaming volume is allowed to be any size, and a fixed 3x3x3
    // assumption would silently under-load a large one.
    const ChunkCoord lo = chunk_at(Vec3{center.x - radius, center.y - radius, center.z - radius}, chunk_size);
    const ChunkCoord hi = chunk_at(Vec3{center.x + radius, center.y + radius, center.z + radius}, chunk_size);

    const f32 radius_sq = radius * radius;

    for (i32 z = lo.z; z <= hi.z; ++z) {
        for (i32 y = lo.y; y <= hi.y; ++y) {
            for (i32 x = lo.x; x <= hi.x; ++x) {
                const ChunkCoord coord{x, y, z};
                // Distance to the box, then compare squared to avoid a sqrt per cell.
                const Vec3 origin{static_cast<f32>(x) * chunk_size, static_cast<f32>(y) * chunk_size,
                                  static_cast<f32>(z) * chunk_size};
                const auto axis_gap = [](f32 p, f32 a, f32 b) -> f32 {
                    if (p < a) return a - p;
                    if (p > b) return p - b;
                    return 0.0f;
                };
                const Vec3 gap{axis_gap(center.x, origin.x, origin.x + chunk_size),
                               axis_gap(center.y, origin.y, origin.y + chunk_size),
                               axis_gap(center.z, origin.z, origin.z + chunk_size)};
                if (gap.length_sq() <= radius_sq) {
                    out.push_back(coord);
                }
            }
        }
    }

    // Nearest first, tie-broken by coordinate.
    //
    // Order is load-bearing, not cosmetic: a caller that caps how many chunks it
    // loads per frame is choosing which ones to defer, and sorting by coordinate
    // would defer whatever happens to sit at high x — possibly the ground the
    // player is standing on, while the far side of the region loads first. The
    // coordinate tie-break keeps the sequence deterministic for equal distances,
    // which is what makes the behaviour reproducible and testable.
    std::sort(out.begin(), out.end(), [&center, chunk_size](const ChunkCoord& a, const ChunkCoord& b) {
        const f32 da = distance_to_chunk(center, a, chunk_size);
        const f32 db = distance_to_chunk(center, b, chunk_size);
        if (da != db) return da < db;
        return a < b;
    });
}

} // namespace nf::scene
