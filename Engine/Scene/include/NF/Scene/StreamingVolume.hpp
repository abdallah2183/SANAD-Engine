#pragma once

// NF/Scene/StreamingVolume.hpp — world streaming: the volume and the grid (Phase 11, W5)
//
// Design doc §246 R6, and the "from day one" principle: an open world is a set of
// chunks that are loaded near the player and released behind them, and an engine
// that retrofits that ends up rewriting its asset and scene layers.
//
// This header is the *data* half: what a streaming volume is, and how a world
// position maps to a chunk. The policy half — which chunks are loaded, and when
// they are released — is `NF/Runtime/WorldStreamer.hpp`, and the IO half is the
// Runtime's.
//
// Scope note. What ships here is the seam plus one working implementation:
// distance-based load/unload on a uniform grid, with hysteresis so a volume
// parked on a boundary does not thrash. Explicitly NOT here, and deferred on
// purpose: LOD, load priority, a memory budget, background/async refinement, and
// non-uniform or nested grids. Those are additions to the policy, not rewrites
// of this data.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <cmath>
#include <vector>

namespace nf::scene {

/// Uniform grid coordinates. Signed, because a world is not obliged to start at
/// the origin and a player walking west must not wrap to the far edge of the map.
struct ChunkCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    constexpr bool operator==(const ChunkCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
    constexpr bool operator!=(const ChunkCoord& other) const { return !(*this == other); }
    constexpr bool operator<(const ChunkCoord& other) const {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }
};

/// An axis-aligned streaming region, plus the two radii that decide which chunks
/// belong to it.
///
/// Two radii rather than one is the whole point: with a single radius, a volume
/// that moves back and forth across a chunk boundary loads and unloads that chunk
/// on every frame. Loading a chunk is file IO and entity creation, so thrashing
/// is not a cosmetic problem. The gap between `load_radius` and `unload_radius`
/// is the hysteresis band.
struct StreamingVolume {
    Vec3 center{0.0f, 0.0f, 0.0f};
    /// Chunks whose nearest point is within this distance are requested.
    f32 load_radius = 64.0f;
    /// A loaded chunk is released only once it is beyond this. Must be >= load_radius;
    /// a smaller value is clamped up, because an inverted band would unload the
    /// chunk that was just loaded, forever.
    f32 unload_radius = 80.0f;
    /// Edge length of one chunk, in world units. Must be > 0.
    f32 chunk_size = 32.0f;

    [[nodiscard]] bool valid() const { return chunk_size > 0.0f && load_radius > 0.0f; }

    /// The hysteresis band, normalised so callers cannot act on an inverted one.
    [[nodiscard]] f32 effective_unload_radius() const {
        return unload_radius < load_radius ? load_radius : unload_radius;
    }
};

/// Grid cell containing a world position. Floor, not truncation: truncation maps
/// both -0.5 and +0.5 into cell 0, which makes a chunk boundary behave
/// asymmetrically about the origin.
[[nodiscard]] inline ChunkCoord chunk_at(const Vec3& position, f32 chunk_size) {
    if (!(chunk_size > 0.0f)) return ChunkCoord{};
    const auto floor_div = [chunk_size](f32 v) -> i32 {
        return static_cast<i32>(std::floor(v / chunk_size));
    };
    return ChunkCoord{floor_div(position.x), floor_div(position.y), floor_div(position.z)};
}

/// World-space centre of a chunk.
[[nodiscard]] inline Vec3 chunk_center(const ChunkCoord& coord, f32 chunk_size) {
    return Vec3{(static_cast<f32>(coord.x) + 0.5f) * chunk_size,
                (static_cast<f32>(coord.y) + 0.5f) * chunk_size,
                (static_cast<f32>(coord.z) + 0.5f) * chunk_size};
}

/// Shortest distance from `point` to the chunk's AABB — 0 when the point is
/// inside it.
///
/// Distance to the box, not to the centre. Using the centre makes a chunk's
/// effective range depend on where inside it the player is standing, so a chunk
/// can fall out of range while the player is still standing on it.
[[nodiscard]] inline f32 distance_to_chunk(const Vec3& point, const ChunkCoord& coord, f32 chunk_size) {
    if (!(chunk_size > 0.0f)) return 0.0f;

    const Vec3 lo{static_cast<f32>(coord.x) * chunk_size,
                  static_cast<f32>(coord.y) * chunk_size,
                  static_cast<f32>(coord.z) * chunk_size};
    const Vec3 hi{lo.x + chunk_size, lo.y + chunk_size, lo.z + chunk_size};

    const auto axis_gap = [](f32 p, f32 a, f32 b) -> f32 {
        if (p < a) return a - p;
        if (p > b) return p - b;
        return 0.0f;
    };

    const Vec3 gap{axis_gap(point.x, lo.x, hi.x), axis_gap(point.y, lo.y, hi.y),
                   axis_gap(point.z, lo.z, hi.z)};
    return gap.length();
}

/// Every chunk whose AABB intersects the sphere of `radius` around `center`,
/// nearest first, ties broken by coordinate.
///
/// Walks the grid the sphere spans rather than scanning a fixed neighbourhood, so
/// the cost follows the requested radius instead of a hardcoded assumption about
/// how big a streaming volume is allowed to be.
///
/// The ordering matters. A caller that caps loads per frame is choosing what to
/// defer, and a coordinate order would defer whatever sits at high x — which may
/// be the ground under the player, while the far side of the region loads first.
void chunks_in_radius(const Vec3& center, f32 radius, f32 chunk_size, std::vector<ChunkCoord>& out);

} // namespace nf::scene

namespace std {

/// Hash for the grid coordinate, so the streamer can keep its loaded set in an
/// unordered_map. Packed rather than combined with the usual prime-multiply
/// dance: a chunk coord is three small integers, and packing three 21-bit fields
/// into one 64-bit key is collision-free for any world a float can express,
/// which a hash that merely *spreads* cannot promise.
template <>
struct hash<nf::scene::ChunkCoord> {
    size_t operator()(const nf::scene::ChunkCoord& coord) const noexcept {
        const auto mask = [](::nf::i32 v) -> ::nf::u64 {
            return static_cast<::nf::u64>(static_cast<::nf::u32>(v) & 0x1FFFFFu);
        };
        return static_cast<size_t>((mask(coord.x) << 42) ^ (mask(coord.y) << 21) ^ mask(coord.z));
    }
};

} // namespace std
