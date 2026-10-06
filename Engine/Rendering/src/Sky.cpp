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

// --- Procedural clouds (the P4 "weather" half) -------------------------------

namespace {

/// The integer hash the shader uses: same constants, same u32 wrapping, so the
/// two agree bit for bit. An integer hash rather than the usual
/// `fract(sin(dot(...)))` trick precisely because `sin` is not required to agree
/// between a driver and the C++ runtime — a sin-based hash could not be
/// mirrored, and an unmirrorable sky is one no test can pin.
u32 cloud_hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// A lattice cell's value in [0, 1). 24 bits of the hash, so the float
/// conversion is exact on both sides.
float cloud_hash_cell(int cx, int cy) {
    const u32 h = cloud_hash_u32(static_cast<u32>(cx) * 0x9e3779b9u ^
                                 cloud_hash_u32(static_cast<u32>(cy) * 0x85ebca6bu));
    return static_cast<float>(h & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float cloud_value_noise(Vec2 uv) {
    const float fx = std::floor(uv.x);
    const float fy = std::floor(uv.y);
    const float tx = uv.x - fx;
    const float ty = uv.y - fy;
    const float ux = tx * tx * (3.0f - 2.0f * tx);
    const float uy = ty * ty * (3.0f - 2.0f * ty);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    const float a = cloud_hash_cell(ix, iy);
    const float b = cloud_hash_cell(ix + 1, iy);
    const float c = cloud_hash_cell(ix, iy + 1);
    const float d = cloud_hash_cell(ix + 1, iy + 1);
    const float p = a + (b - a) * ux;
    const float q = c + (d - c) * ux;
    return p + (q - p) * uy;
}

} // namespace

float cloud_density(Vec2 uv) {
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    for (int i = 0; i < kCloudOctaves; ++i) {
        sum += cloud_value_noise(Vec2{uv.x * freq, uv.y * freq}) * amp;
        freq *= 2.0f;
        amp *= 0.5f;
    }
    return sum / kCloudFbmNorm;
}

float compute_cloud_alpha(const SkyParams& sky, Vec3 ray_dir, Vec3 cam_pos) {
    if (!sky.enabled || sky.cloud_coverage <= 0.0f || ray_dir.y <= 1.0e-4f) {
        return 0.0f;
    }
    // The plane sits `cloud_altitude` above the CAMERA, so a ray that points up
    // at all reaches it: t is positive by construction.
    const float t = sky.cloud_altitude / ray_dir.y;
    const float inv = 1.0f / std::max(sky.cloud_scale, 1.0e-3f);
    const Vec2 uv{(cam_pos.x + ray_dir.x * t) * inv + sky.cloud_time * 0.05f,
                  (cam_pos.z + ray_dir.z * t) * inv};
    const float d = clamp01(cloud_density(uv));
    // Coverage is a threshold with a soft edge: at coverage 0 nothing passes,
    // and the ramp is normalised so coverage 1 is fully overcast.
    const float e = std::max(1.0f - sky.cloud_coverage, 1.0e-3f);
    const float x = clamp01((d - sky.cloud_coverage) / e);
    return x * x * (3.0f - 2.0f * x);
}

} // namespace nf::rendering
