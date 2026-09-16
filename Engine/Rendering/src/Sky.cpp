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

Vec3 compute_sky_color(const SkyParams& sky, Vec3 ray_dir, Vec3 sun_dir,
                       Vec3 sun_color, bool light_enabled) {
    if (!sky.enabled) return sky.clear;
    const float h = std::clamp(ray_dir.y, -1.0f, 1.0f);
    Vec3 col = (h >= 0.0f) ? mix_vec(sky.horizon, sky.zenith, std::pow(h, 0.6f))
                           : mix_vec(sky.horizon, sky.ground, clamp01(-h * 3.0f));
    if (light_enabled) {
        const float sun_len = sun_dir.length();
        if (sun_len > 1e-6f) {
            const Vec3 s = sun_dir / sun_len;
            const float cos_a = std::max(ray_dir.dot(s), 0.0f);
            const float disk = smoothstep(0.9996f, 0.99985f, cos_a);
            const float glow =
                std::pow(cos_a, 600.0f) * 0.6f + std::pow(cos_a, 24.0f) * 0.12f;
            col += sun_color * (disk * 4.0f * sky.sun_disk + glow * sky.sun_glow);
        }
    }
    return col;
}

} // namespace nf::rendering
