#pragma once

// NF/Foliage/Foliage.hpp — foliage scatter (design doc 61).
//
// Places instances of a set of species on a surface: grass tufts, rocks, trees.
// The scatter is CPU-only and deterministic — the same types, rect, and seed
// produce the same instances bit-for-bit, and a cell's instances do not depend
// on which of its neighbours were scattered, so a chunk generated on demand
// (design doc 60) is identical to the same chunk generated as part of a larger
// rect. Rendering is deliberately absent: the output is an instance buffer a
// GPU instancer binds, and the wind is a pure function the instancer samples.
//
// The ground the foliage stands on is INJECTED (GroundSampler) rather than
// imported, so this module never pulls in the RHI or the terrain: a terrain
// supplies one sampler, a hand-authored height function another, and a test a
// plane. The same injection covers the density mask (DensityMask) — a painted
// mask is just a raster wrapped in the callback.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace nf::foliage {

/// The ground at one world position. `slope` is radians from +Y — the same
/// convention TerrainLayer's gates use — so a band that rejects steep ground
/// rejects the same vertices the terrain's own splat would.
struct GroundSample {
    f32 height = 0.0f;
    f32 slope = 0.0f;
};

/// Height + slope at (x, z). Injected, never null-checked: the scatter calls it
/// per candidate, and a default-constructed `std::function` throws the first
/// time — a missing ground is a wiring bug, not a flat field.
using GroundSampler = std::function<GroundSample(f32 x, f32 z)>;

/// Density multiplier in [0, 1] at (x, z). 0 rejects every candidate there, 1
/// accepts the type's own odds; between is a probability. A painted mask, a
/// noise function, or a constant (`return 1.0f`) all fit the same slot.
using DensityMask = std::function<f32(f32 x, f32 z)>;

/// One species the scatter places (design doc 61: randomization, scale,
/// rotation, density, masking, LOD, wind).
struct FoliageType {
    /// What the instances draw — a mesh name, an asset id. Opaque to the
    /// scatter: copied into the instance untouched so the renderer, not the
    /// placement, decides what a "grass" is.
    std::string mesh;
    /// Instances per square world unit. The cell grid is sized from it, so the
    /// expected count per cell is ~1 and the field really is denser at 2.0 than
    /// at 0.5. Zero or negative never spawns.
    f32 density = 0.1f;
    /// Scale is uniform per instance, drawn between these. Equal values give a
    /// hedge; a wide range gives a meadow.
    f32 min_scale = 1.0f;
    f32 max_scale = 1.0f;
    /// Yaw spread in radians, applied uniformly about 0. 0 faces every instance
    /// the same way; PI (the default) faces it anywhere on the circle.
    f32 yaw_spread = PI;
    /// World-Y band. Defaults to the whole range so a ground cover ignores it.
    f32 min_height = -std::numeric_limits<f32>::infinity();
    f32 max_height = std::numeric_limits<f32>::infinity();
    /// Slope gate in radians from +Y. Defaults to every slope; a rock band that
    /// owns only cliffs or a grass band that owns only flats narrows both.
    f32 min_slope = 0.0f;
    f32 max_slope = PI;
    /// Odds a candidate that passes the gates actually emits, before the mask.
    /// Two species over the same ground at 1.0 each intermix half and half; one
    /// at 0.2 takes a fifth of the surface it could cover.
    f32 weight = 1.0f;
    /// Wind (design doc 61: wind). Amplitude per instance, in world units at
    /// scale 1; 0 makes the species rigid (a rock).
    f32 wind_amount = 0.0f;
    /// Wind cycles per second. Two species sharing a speed and direction sway in
    /// phase; a different one ripples.
    f32 wind_speed = 1.0f;
};

/// A placed instance. 32 bytes / 8 floats on purpose: this is the instance
/// buffer's element, and `type_index`/`lod` ride in float slots the shader reads
/// as floats. `wind_phase` makes the sway spatially coherent instead of a
/// per-vertex flutter — two neighbours share a phase, so a gust crosses a field.
struct FoliageInstance {
    f32 position[3] = {0.0f, 0.0f, 0.0f};
    f32 scale = 1.0f;
    f32 yaw = 0.0f;
    u32 type_index = 0u;
    u32 lod = 0u;
    f32 wind_phase = 0.0f;
};
static_assert(sizeof(FoliageInstance) == 32u, "FoliageInstance is the instance buffer element");

/// Wind at a moment, shared by every species. `direction` need not be unit — it
/// is normalised once per call, not once per instance.
struct WindState {
    f32 time = 0.0f;
    /// Gust envelope in [0, 1]; 1 is a steady breeze. Modulated by time in
    /// `wind_displacement` so a storm builds and ebbs rather than blowing at a
    /// fixed strength.
    f32 gust = 1.0f;
    Vec3 direction = Vec3{1.0f, 0.0f, 0.0f};
};

/// Where an instance is displaced by the wind. Pure: the same instance and the
/// same time give the same offset, so a replay is bit-identical and a CPU-side
/// query (does the grass brush the player?) matches what the shader renders.
/// Bending is proportional to `scale` — the mesh's own height is not known here,
/// and a taller blade swings further.
Vec3 wind_displacement(const FoliageInstance& instance,
                       const FoliageType& type,
                       const WindState& wind);

/// Placement budget the scatter reports. A field that rejects nine of ten
/// candidates at the gates shows it as `rejected`, which is how a density that
/// looks too low turns out to be a mask, not a bug.
struct ScatterStats {
    u32 candidates = 0u; // grid cells visited
    u32 rejected = 0u;   // gate, weight, mask, or fade rejection
    u32 placed = 0u;     // emitted instances
    u32 culled = 0u;     // beyond the last LOD distance
};

/// LOD buckets. An instance lands in the bucket its distance from the centre
/// falls into; past the last it is thinned out rather than cut off at once (see
/// `scatter_foliage`), so a distant field fades instead of vanishing at a
/// radius. Distances must be ascending and positive; the scatter sorts nothing.
struct ScatterOptions {
    /// Integer seed — the hash never sees a float, so the same seed reproduces
    /// on every platform the way the terrain's lattice hash does (design doc
    /// Section 114).
    u64 seed = 1;
    f32 center_x = 0.0f;
    f32 center_z = 0.0f;
    /// Beyond the last LOD distance, an instance's survival odds fall linearly
    /// to zero at `cull_radius`. 0 disables both the fade and the cull — the
    /// whole rect is placed, which is what baking a static field wants.
    f32 cull_radius = 0.0f;
    f32 lod_distances[4] = {20.0f, 60.0f, 150.0f, 400.0f};
};

/// Places instances of every type over the world rect [min_x, max_x] x
/// [min_z, max_z]. Each type gets its own jittered grid whose cell size is
/// 1/sqrt(density), so the expected count is density * area whatever the rect,
/// and a candidate at a cell is accepted from the type's gates, its weight, and
/// the mask. Cells are INTEGER-indexed in each type's own grid, and a cell's
/// outcome depends only on that cell — so overlapping rects agree on shared
/// ground, and streaming a chunk reproduces the bake.
///
/// Degenerate input is empty rather than an error: no types, a zero-density
/// type, or a rect with no area yields no instances and leaves `stats` counting
/// nothing. `ground` must be callable; `mask` may be empty (no mask).
std::vector<FoliageInstance> scatter_foliage(const std::vector<FoliageType>& types,
                                             const GroundSampler& ground,
                                             const DensityMask& mask,
                                             f32 min_x, f32 min_z, f32 max_x, f32 max_z,
                                             const ScatterOptions& opts,
                                             ScatterStats* stats = nullptr);

} // namespace nf::foliage
