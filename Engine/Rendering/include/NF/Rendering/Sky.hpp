#pragma once

// NF/Rendering/Sky.hpp — procedural sky parameters (Phase 13).
//
// The lighting pass paints a gradient sky + sun disk on pixels where the
// depth buffer reads "far" (no geometry). This header owns the parameters
// and their CPU mirror: compute_sky_color() implements EXACTLY the GLSL in
// lighting.frag, so tests pin the look without a GPU.

#include <NF/Core/Math.hpp>

namespace nf::rendering {

struct SkyParams {
    Vec3 zenith{0.20f, 0.42f, 0.85f};   // straight up
    Vec3 horizon{0.62f, 0.72f, 0.82f};  // eye level
    Vec3 ground{0.09f, 0.09f, 0.11f};   // below the horizon
    Vec3 clear{0.03f, 0.03f, 0.07f};    // used when enabled == false
    float sun_disk = 1.0f;              // sun disk intensity multiplier
    float sun_glow = 1.0f;              // halo intensity multiplier
    bool enabled = true;
};

/// Sky radiance for a normalized view ray. sun_dir points TOWARD the sun,
/// sun_color is linear HDR (color * intensity). Matches lighting.frag.
Vec3 compute_sky_color(const SkyParams& sky, Vec3 ray_dir, Vec3 sun_dir,
                       Vec3 sun_color, bool light_enabled);

} // namespace nf::rendering
