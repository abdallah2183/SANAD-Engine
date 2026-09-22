// NF/Rendering/Sky.cpp — CPU mirror of the lighting.frag sky branch.

#include <NF/Rendering/Sky.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

namespace {

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

Vec3 mix_vec(const Vec3& a, const Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// GLSL smoothstep(edge0, edge1, x).
float smoothstep(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

/// Chromaticity of an HDR sun colour: normalize by the brightest channel so a
/// bright sun cannot blow the haze band out, only tint it.
Vec3 tint_of(const Vec3& color) {
    const float peak = std::max(std::max(color.x, color.y), color.z);
    if (peak <= 1e-6f) {
        return {1.0f, 1.0f, 1.0f};
    }
    return {color.x / peak, color.y / peak, color.z / peak};
}

Vec3 compute_sky_color(const SkyParams& sky, Vec3 ray_dir, Vec3 sun_dir,
                       Vec3 sun_color, bool light_enabled) {
    if (!sky.enabled) return sky.clear;
    const float h = std::clamp(ray_dir.y, -1.0f, 1.0f);
    if (h < 0.0f) {
        // Below the horizon the sky fades into the ground colour over 35
        // degrees, so an empty scene ends on a horizon instead of a void.
        return mix_vec(sky.horizon, sky.ground, clamp01(-h * kSkyGroundFade));
    }
    Vec3 col = mix_vec(sky.horizon, sky.zenith, std::pow(h, kSkyZenithCurve));
    // Haze band just above eye level: exactly zero AT the horizon and at the
    // zenith, which is what keeps the endpoints in the pinned tests exact.
    // Written as d*d rather than pow(d, 2) so the GLSL twin (pow of a negative
    // base is undefined there) is the same expression bit for bit.
    const float d = (h - kSkyHazeCenter) * kSkyHazeWidth;
    const float band = std::exp(-(d * d)) * smoothstep(0.0f, 0.015f, h);
    col = col + sky.horizon * (kSkyHazeStrength * band);
    if (light_enabled) {
        const float sun_len = sun_dir.length();
        if (sun_len > 1e-6f) {
            const Vec3 s = sun_dir / sun_len;
            const float cos_a = std::max(ray_dir.dot(s), 0.0f);
            // Sun-forward scattering inside the haze: the warm patch a low sun
            // paints on the horizon. Scaled by sun_glow, so muting the glow
            // mutes the tint with it (pinned by sky_sun_multipliers_...).
            col = col + tint_of(sun_color) *
                            (kSkyWarmScatter * sky.sun_glow * band * std::pow(cos_a, 4.0f));
            // Disk + halo. The disk is ~2.5 degrees across (a real sun is
            // 0.53) so it reads at viewport resolution instead of as one pixel.
            const float disk = smoothstep(0.9990f, 0.9997f, cos_a);
            const float glow =
                std::pow(cos_a, 600.0f) * 0.6f + std::pow(cos_a, 24.0f) * 0.12f;
            col = col + sun_color * (disk * 4.0f * sky.sun_disk + glow * sky.sun_glow);
        }
    }
    return col;
}

} // namespace nf::rendering
