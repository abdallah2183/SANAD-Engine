// NF/Rendering/SkyEnv.cpp — sky environment bake for image-based lighting.

#include <NF/Rendering/SkyEnv.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf::rendering {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

float near(float a, float b, float eps) {
    return std::fabs(a - b) <= eps;
}

} // namespace

Vec3 equirect_direction(float u, float v) {
    // Inverse of env_equirect_uv: phi = atan2(z, x), el = asin(y).
    const float phi = (u - 0.5f) * kTwoPi;
    const float el = (v - 0.5f) * kPi;
    const float r = std::cos(el);
    return Vec3{r * std::cos(phi), std::sin(el), r * std::sin(phi)};
}

void equirect_uv(Vec3 dir, float& u, float& v) {
    const float len = dir.length();
    const Vec3 n = (len > 1e-6f) ? dir * (1.0f / len) : Vec3{1.0f, 0.0f, 0.0f};
    u = std::atan2(n.z, n.x) / kTwoPi + 0.5f;
    v = std::asin(std::clamp(n.y, -1.0f, 1.0f)) / kPi + 0.5f;
}

u16 float_to_half(float v) {
    // Clamp first: the sky bake never legitimately exceeds half-max, and a NaN
    // must not become a signalling payload that a mip average then spreads.
    if (!(v > 0.0f)) {
        // Covers v <= 0 AND NaN (comparisons are false for NaN): negatives
        // clamp to +0, NaN to +0 rather than to an Inf the blur would smear.
        return 0;
    }
    if (v >= 65504.0f) {
        return 0x7BFF; // largest finite half, not Inf (see header)
    }
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    const u32 exp = (bits >> 23) & 0xFFu;
    const u32 mant = bits & 0x7FFFFFu;
    const int new_exp = static_cast<int>(exp) - 112; // 127 - 15
    if (new_exp >= 31) {
        return 0x7BFF;
    }
    if (new_exp <= 0) {
        // Subnormal half (or underflow to zero). Add the hidden 1 back, then
        // shift down; the +1 in the shift is the round-to-nearest bit.
        if (new_exp < -10) {
            return 0;
        }
        const u32 full = mant | 0x800000u;
        const int shift = 14 - new_exp;
        return static_cast<u16>((full >> shift) + ((full >> (shift - 1)) & 1u));
    }
    // Normalised: drop 13 mantissa bits with round-to-nearest.
    return static_cast<u16>(((static_cast<u32>(new_exp) << 10) | (mant >> 13)) +
                            ((mant >> 12) & 1u));
}

float half_to_float(u16 h) {
    const u32 sign = (static_cast<u32>(h) & 0x8000u) << 16;
    const u32 exp_bits = (static_cast<u32>(h) >> 10) & 0x1Fu;
    u32 mant = static_cast<u32>(h) & 0x3FFu;
    u32 bits = 0;
    if (exp_bits == 0) {
        if (mant == 0) {
            bits = sign; // +/-0
        } else {
            // Subnormal: renormalise with a SIGNED exponent (an unsigned one
            // wraps below zero and the bias arithmetic then lands on garbage).
            int exp = 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | ((static_cast<u32>(127 - 15 + exp) << 23)) | (mant << 13);
        }
    } else if (exp_bits == 31) {
        bits = sign | 0x7F800000u | (mant << 13); // Inf/NaN
    } else {
        bits = sign | ((exp_bits + 112u) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

SkyEnvKey SkyEnvKey::make(const SkyParams& sky, Vec3 sun_dir, Vec3 sun_color,
                          bool light_enabled) {
    SkyEnvKey k;
    k.zenith[0] = sky.zenith.x;
    k.zenith[1] = sky.zenith.y;
    k.zenith[2] = sky.zenith.z;
    k.horizon[0] = sky.horizon.x;
    k.horizon[1] = sky.horizon.y;
    k.horizon[2] = sky.horizon.z;
    k.ground[0] = sky.ground.x;
    k.ground[1] = sky.ground.y;
    k.ground[2] = sky.ground.z;
    k.clear[0] = sky.clear.x;
    k.clear[1] = sky.clear.y;
    k.clear[2] = sky.clear.z;
    k.sun_disk = sky.sun_disk;
    k.sun_glow = sky.sun_glow;
    k.sky_enabled = sky.enabled ? 1.0f : 0.0f;
    const float slen = sun_dir.length();
    const Vec3 s = (slen > 1e-6f) ? sun_dir * (1.0f / slen) : Vec3{0.0f, 1.0f, 0.0f};
    k.sun_dir[0] = s.x;
    k.sun_dir[1] = s.y;
    k.sun_dir[2] = s.z;
    k.sun_color[0] = sun_color.x;
    k.sun_color[1] = sun_color.y;
    k.sun_color[2] = sun_color.z;
    k.light_enabled = light_enabled ? 1.0f : 0.0f;
    return k;
}

bool SkyEnvKey::matches(const SkyEnvKey& o) const {
    // Colours at 1e-3 (a just-noticeable HDR step after the tonemap is far
    // smaller; re-baking one step early is harmless, re-baking never is what
    // this guards), directions tighter (a 1e-4 dir step is ~0.006 deg).
    constexpr float kColorEps = 1e-3f;
    constexpr float kDirEps = 1e-4f;
    for (int i = 0; i < 3; ++i) {
        if (!near(zenith[i], o.zenith[i], kColorEps)) return false;
        if (!near(horizon[i], o.horizon[i], kColorEps)) return false;
        if (!near(ground[i], o.ground[i], kColorEps)) return false;
        if (!near(clear[i], o.clear[i], kColorEps)) return false;
        if (!near(sun_dir[i], o.sun_dir[i], kDirEps)) return false;
        if (!near(sun_color[i], o.sun_color[i], kColorEps)) return false;
    }
    return near(sun_disk, o.sun_disk, kColorEps) &&
           near(sun_glow, o.sun_glow, kColorEps) &&
           near(sky_enabled, o.sky_enabled, 0.5f) &&
           near(light_enabled, o.light_enabled, 0.5f);
}

void bake_sky_env(std::vector<u16>& out, int w, int h, const SkyParams& sky,
                  Vec3 sun_dir, Vec3 sun_color, bool light_enabled) {
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    const float slen = sun_dir.length();
    const Vec3 s = (slen > 1e-6f) ? sun_dir * (1.0f / slen) : Vec3{0.0f, 1.0f, 0.0f};
    for (int y = 0; y < h; ++y) {
        // v = 0 is the FIRST row: the bottom of the equirect (dir.y = -1).
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
            const Vec3 col = compute_sky_color(sky, equirect_direction(u, v), s,
                                              sun_color, light_enabled);
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                              static_cast<size_t>(x)) *
                             4u;
            out[i + 0] = float_to_half(col.x);
            out[i + 1] = float_to_half(col.y);
            out[i + 2] = float_to_half(col.z);
            out[i + 3] = float_to_half(1.0f);
        }
    }
}

} // namespace nf::rendering
