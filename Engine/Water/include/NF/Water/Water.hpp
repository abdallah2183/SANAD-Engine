#pragma once

// NF/Water/Water.hpp — water surface (design doc 62: ocean, lakes, rivers).
//
// The surface is a sum of Gerstner waves evaluated as a pure function of
// position and time. Everything a game needs to reason about the water — the
// height a buoy floats at, the normal a shader perturbs by, the foam a crest
// throws, and a cheap reflection estimate — is that same function, so a CPU
// query (does the wheel dip? does the camera get wet?) agrees with the GPU
// vertex displacement bit-for-bit at the same time.
//
// CPU-only by construction: the wave field is arithmetic, the mesh builder
// shares StaticMesh with every other mesh, and the shore is *injected*
// (ShoreSampler) the way foliage injects its ground, so this module never
// pulls in the RHI or the terrain. A heightfield coast supplies one sampler, a
// flat plane another, and a test a constant depth.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace nf::water {

/// One Gerstner wave (design doc 62: waves). Direction is in the XZ plane and
/// need not be unit — it is normalised once per evaluation, never per vertex.
/// `steepness` in [0, 1] bends a sine into a crest: 0 is a gentle swell and 1
/// is the sharpest a Gerstner wave can be before it loops over itself.
struct Wave {
    f32 amplitude = 0.5f;       // world units, trough to crest is 2x
    f32 wavelength = 16.0f;     // world units between crests
    f32 speed = 1.5f;           // world units per second the crest travels
    Vec2 direction = Vec2{1.0f, 0.0f};
    f32 steepness = 0.6f;       // 0 = sine, 1 = sharpest
};

/// Water depth at (x, z) in world units — the distance from the surface's
/// REST level to the floor. Positive is underwater; a point the sampler
/// reports <= 0 is dry land. Injected, never null-checked: a missing shore is
/// a wiring bug, and the first query says so rather than every vertex.
using ShoreSampler = std::function<f32(f32 x, f32 z)>;

/// A constant-depth shore: the whole plane is `depth` underwater. The default
/// for an open ocean where the floor is far below anything the waves move.
ShoreSampler constant_shore(f32 depth);

/// Where the surface is at rest. Waves displace around it; the shore is
/// measured from it. Kept explicit because a lake at y = 12 and a sea at
/// y = 0 share every wave behaviour and differ only in this.
struct WaterOptions {
    f32 level = 0.0f;           // rest height
    f32 size = 100.0f;          // world extent in X and Z
    u32 resolution = 64;        // vertices per side (>= 2)
    /// Time the field is sampled at. The same time on the CPU and the GPU is
    /// the whole determinism contract; a replay at t and t+dt sees the wave
    /// the shader drew.
    f32 time = 0.0f;
    /// Foam (design doc 62): a crest starts white once it has pinched a given
    /// share of what this wave set *can* pinch. The measure is the determinant
    /// of the horizontal Gerstner map (how much the surface compresses in the
    /// plane — that compression is what a crest *is*), normalised by the wave
    /// set's total sharpness Σ steepness·amplitude·wavenumber, so it is
    /// scale-free: the same number means the same kind of crest for a 0.2u
    /// ripple and a 3u swell, and 0.6 is a crest, 0.0 a trough, for both.
    /// `foam_amount` scales the lot, so a calm lake at 0 renders as glass even
    /// where the geometry is steep.
    f32 foam_steepness = 0.6f;
    /// Width of the band around `foam_steepness` the foam fades over, in the
    /// same crest units. 0.05 is a crisp line drawn at the crest; 0.4 a soft
    /// plume that reaches partway down the face.
    f32 foam_softness = 0.15f;
    /// Foam does not appear without waves: `foam_amount` scales the lot, so a
    /// calm lake at 0 renders as glass even where the geometry is steep.
    f32 foam_amount = 1.0f;
    /// Shoreline foam (design doc 62): depth in world units at which surf
    /// peaks. Shallow than this and the wave has no water to throw; deeper and
    /// it passes over as swell — so surf is a tent over the break depth, not a
    /// ramp, and an ocean floor far below contributes nothing. The shore
    /// sampler decides where the coast is.
    f32 shore_break_depth = 2.0f;
    /// Cheap reflection (design doc 62): the estimate assumes the viewer is
    /// high enough that the reflected ray's vertical spread matters more than
    /// its horizontal, so the reflectance is a function of the normal's tilt
    /// alone. `reflection_strength` scales it; 0 is a flat diffuse plane.
    f32 reflection_strength = 1.0f;
    /// Fresnel at normal incidence, in [0, 1]. Water is ~0.02; a stylised
    /// surface raises it. At grazing angles the estimate saturates to 1
    /// regardless, which is the behaviour the cheap model is for.
    f32 fresnel_normal = 0.02f;
    /// Integer seed for the foam's spatial texture. The wave field itself has
    /// no randomness — a sum of sinusoids is what it is — so the seed only
    /// perturbs where foam *appears* within a crest, never how much there is
    /// on average: the texture multiplier's mean over the lattice is 1, so two
    /// waters with the same waves and different seeds crest identically and
    /// foam the same total in different places, which is what a replay needs.
    u64 seed = 1;
};

/// Height of the surface at (x, z) at `options.time`, in world units. Pure:
/// the same position, time, and wave list answer the same value everywhere.
/// This is the displaced surface height at the *rest* position — the query a
/// buoy makes ("how high is the water where I am"), which is what physics
/// wants; the visual vertex is displaced horizontally too (see
/// `water_displacement`) and the two are deliberately not the same function.
f32 water_height(f32 x, f32 z, const std::vector<Wave>& waves,
                 const WaterOptions& options);

/// The horizontal Gerstner displacement at (x, z): where the surface point
/// that started there has moved to. Crests pinch inward toward their own
/// direction, which is the visual that makes the sum read as water rather
/// than as rippled paper.
Vec2 water_displacement(f32 x, f32 z, const std::vector<Wave>& waves,
                        const WaterOptions& options);

/// Unit normal of the displaced surface, from the analytic Jacobian of the
/// Gerstner sum rather than finite differences — the derivative of a
/// sinusoid is exact, so a vertex normal and a CPU query at the same time
/// agree to the last bit.
Vec3 water_normal(f32 x, f32 z, const std::vector<Wave>& waves,
                  const WaterOptions& options);

/// Foam intensity in [0, 1] at (x, z): crests break where they pinch (see
/// `foam_steepness`), and shallow water adds surf scaled by how close the
/// depth is to `shore_break_depth`. Pure, and seeded only in *where within a
/// crest* the texture lands. `shore` may be empty (open ocean, no surf).
f32 water_foam(f32 x, f32 z, const std::vector<Wave>& waves,
               const WaterOptions& options, const ShoreSampler& shore);

/// Cheap reflection estimate in [0, 1]: how mirror-like the surface is at
/// (x, z) given the viewer's elevation angle above the horizon. The model is
/// a Schlick Fresnel over the tilt of the normal, which captures the one
/// effect that reads at a distance — water facing you is dark, water facing
/// away is sky — without tracing a reflected ray.
f32 water_reflection(f32 x, f32 z, f32 viewer_elevation,
                     const std::vector<Wave>& waves, const WaterOptions& options);

/// How much of the surface is above the rest level at `options.time`, in
/// [0, 1]. A surging shore (a wave lifting the whole plane) is not the same
/// as a calm one, and the shore foam needs to know.
f32 water_surge(const std::vector<Wave>& waves, const WaterOptions& options);

/// A vertex of the water mesh is a `StaticMesh::Vertex`, exactly as terrain:
/// `uv0` carries world-space tiling coordinates (so a normal map does not swim
/// with the grid), and `uv1` carries `(foam, reflection)` — the painterly and
/// the lighting channel — so water rides the same vertex layout the renderer
/// already uploads, and the one `uv1` declaration the terrain needs is the one
/// the water needs too.

/// Where the mesh's vertices land. The grid is written at `options.time`:
/// the GPU could displace it per frame instead, but a baked grid is what a
/// river in a static scene wants, and the CPU field above stays the source of
/// truth for anything that queries the water live.
struct WaterMeshStats {
    u32 vertices = 0u;
    u32 indices = 0u;
    f32 max_foam = 0.0f;     // the loudest crest in the patch
    f32 max_height = 0.0f;   // bounds sanity: the field actually moved
};

/// Builds the water patch as a StaticMesh: an indexed grid on the XZ plane
/// displaced by the Gerstner sum (vertically by `water_height`, horizontally by
/// `water_displacement` — the visual vertex is *not* the height query, on
/// purpose), with analytic normals from the Gerstner Jacobian, world-space
/// `uv0`, and foam + reflection in `uv1`. One submesh, one material slot
/// ("water"), so the renderer treats it as a single draw. Returns nullptr on
/// degenerate options (resolution < 2, non-positive size, or no waves — a
/// water with no waves is a plane, and a caller who wants one should ask for a
/// plane).
std::unique_ptr<nf::rendering::StaticMesh>
build_water_mesh(const std::vector<Wave>& waves,
                 const WaterOptions& options,
                 const ShoreSampler& shore,
                 WaterMeshStats* stats = nullptr);

} // namespace nf::water
