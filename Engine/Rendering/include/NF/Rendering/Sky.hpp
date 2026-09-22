#pragma once

// NF/Rendering/Sky.hpp — procedural sky parameters (Phase 13).
//
// The lighting pass paints a gradient sky + sun disk on pixels where the
// depth buffer reads "far" (no geometry). This header owns the parameters
// and their CPU mirror: compute_sky_color() implements EXACTLY the GLSL in
// lighting.frag, so tests pin the look without a GPU.

#include <NF/Core/Math.hpp>

namespace nf::rendering {

// Natural daylight defaults (Phase 21). The old defaults were three greys:
// the zenith barely read as blue after tonemapping and nothing marked the
// horizon, so an empty scene looked like a flat grey card. These values are
// the same shape (zenith -> horizon -> ground) with a saturated blue overhead,
// a pale hazy horizon and a brighter haze band a few degrees above eye level.
struct SkyParams {
    Vec3 zenith{0.055f, 0.195f, 0.600f}; // straight up: deep saturated blue
    Vec3 horizon{0.550f, 0.660f, 0.800f}; // eye level: pale haze
    Vec3 ground{0.135f, 0.125f, 0.110f}; // below the horizon: dry earth
    Vec3 clear{0.03f, 0.03f, 0.07f};     // used when enabled == false
    float sun_disk = 1.0f;              // sun disk intensity multiplier
    float sun_glow = 1.0f;              // halo + forward-scatter multiplier
    bool enabled = true;
};

// Sky shape constants. lighting.frag spells the same numbers out (GLSL has no
// way to include this header), so a change here is a change there; the tests in
// Tests/RHITests/test_sky.cpp pin the curve, the haze band and the ground fade
// on both sides of the boundary.
//
//   up     = max(ray.y, 0)
//   zenith_blend = pow(up, kSkyZenithCurve)          // 0 at the horizon, 1 overhead
//   haze   = exp(-((up - kSkyHazeCenter) * kSkyHazeWidth)^2) * smoothstep(0, 0.015, up)
//   ground_blend = clamp(-ray.y * kSkyGroundFade, 0, 1)  // 1 at 35 degrees down
//   warm   = kSkyWarmScatter * sun_glow * haze * pow(saturate(dot(ray, sun)), 4)
//
// The haze band is exactly zero at the horizon and at the zenith, which is what
// keeps compute_sky_color()'s pinned endpoints (zenith overhead, horizon at eye
// level, ground straight down) exact.
inline constexpr float kSkyZenithCurve = 0.62f;
inline constexpr float kSkyHazeStrength = 0.50f;
inline constexpr float kSkyHazeCenter = 0.06f;
inline constexpr float kSkyHazeWidth = 10.0f;
inline constexpr float kSkyGroundFade = 2.8571428f; // 1 / 0.35
inline constexpr float kSkyWarmScatter = 0.30f;     // scaled by sun_glow

/// Sky radiance for a normalized view ray. sun_dir points TOWARD the sun,
/// sun_color is linear HDR (color * intensity). Matches lighting.frag exactly:
/// the gradient, the haze band, the sun-side warm scatter, the disk and the
/// glow all use the constants above.
Vec3 compute_sky_color(const SkyParams& sky, Vec3 ray_dir, Vec3 sun_dir,
                       Vec3 sun_color, bool light_enabled);

} // namespace nf::rendering
