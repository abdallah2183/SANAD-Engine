#pragma once

// NF/Rendering/SkyEnv.hpp — sky environment bake for image-based lighting.
//
// The lighting pass used to light every surface with one scalar ambient, so a
// scene's fill light had no direction, no sky colour and no sun in it: shaded
// sides went flat grey and metals had nothing to reflect. This header owns the
// CPU side of the fix: bake_sky_env() evaluates the procedural sky
// (compute_sky_color, the exact twin of the lighting.frag sky branch) into a
// small equirectangular HDR image the renderer uploads once and samples with
// roughness-driven mip levels (split-sum IBL, see ibl_contrib in brdf.glsl).
//
// What is baked, and what is deliberately NOT:
//   BAKED: the sky gradient, the haze band, the sun disk + halo. The sun disk
//          is what puts sharp glints on glossy surfaces; dropping it would
//          leave metals lit by a sunless sky.
//   NOT BAKED: the cloud layer. It vanishes under mip blur anyway (a 128-wide
//          map cannot hold cloud detail past the first mip), and skipping the
//          4-octave FBM keeps a bake under a millisecond instead of ~20x that.
//          The clouds still draw in the background pixels; they just do not
//          light surfaces, which no blurred reflection could resolve either.
//
// Mapping (MUST match env_equirect_uv in brdf.glsl — the roundtrip test pins
// both sides, because a mirrored pair that disagrees puts the sun's reflection
// on the wrong side of every glossy surface):
//   u = atan2(dir.z, dir.x) / 2PI + 0.5,   v = asin(dir.y) / PI + 0.5.

#include <NF/Core/Math.hpp>
#include <NF/Rendering/Sky.hpp>

#include <cstdint>
#include <vector>

namespace nf::rendering {

// Bake resolution. 128x64: the sun disk (~2.5 deg) lands on roughly one texel
// so its energy survives for glossy reflections, while the upload stays 64 KiB
// (RGBA half) and a bake stays sub-millisecond. Higher buys sharper sun
// glints; lower smears the sun into the general glow.
inline constexpr int kSkyEnvWidth = 128;
inline constexpr int kSkyEnvHeight = 64;
// Mip levels for a 128x64 image down to 1x1: log2(128) + 1. The diffuse lobe
// samples near the top (whole-scene average), the specular lobe walks the
// chain by roughness. Must match the texture the renderer creates.
inline constexpr int kSkyEnvMips = 8;

// Inverse of the shader's env_equirect_uv: the view direction through texel
// (u, v), u/v in [0, 1]. Used by the baker; the roundtrip test proves it
// inverts the GLSL mapping.
Vec3 equirect_direction(float u, float v);

// Forward mapping (the shader's twin, CPU-side for tests): equirect uv of a
// direction. Must satisfy equirect_uv(equirect_direction(u, v)) == (u, v).
void equirect_uv(Vec3 dir, float& u, float& v);

// IEEE-754 float -> binary16 bits, round-to-nearest, clamp-to-max. Inf/NaN
// saturate to the largest finite half (0x7BFF) rather than propagating
// infinity through the mip chain, where one Inf texel would poison every
// blurrier level that averages it in.
u16 float_to_half(float v);
float half_to_float(u16 h);

// The inputs a bake depends on, with an epsilon comparison so the renderer
// only re-bakes when the sky actually moved (a dusk timelapse moves the sun
// every frame; an epsilon-free check would re-bake identically-rounded values
// forever, while a too-loose one would freeze the reflections mid-timelapse).
struct SkyEnvKey {
    float zenith[3]{};
    float horizon[3]{};
    float ground[3]{};
    float clear[3]{};
    float sun_disk = 1.0f;
    float sun_glow = 1.0f;
    float sky_enabled = 1.0f;
    float sun_dir[3]{};
    float sun_color[3]{};
    float light_enabled = 1.0f;

    static SkyEnvKey make(const SkyParams& sky, Vec3 sun_dir, Vec3 sun_color,
                          bool light_enabled);
    bool matches(const SkyEnvKey& other) const;
};

// Bake the sky into RGBA half-float texels (alpha = 1), row-major, v = 0 at
// the bottom row... NOTE: v = 0 is the FIRST row written and maps to the
// bottom of the equirect (dir.y = -1); the uploader must not flip rows or the
// sky ends up upside down in every reflection.
//
// `out` is resized to w*h*4 on return. No clouds (see above). Deterministic:
// the same inputs produce bit-identical bytes, so a re-bake of an unchanged
// sky is a no-op the dirty check already skipped.
void bake_sky_env(std::vector<u16>& out, int w, int h, const SkyParams& sky,
                  Vec3 sun_dir, Vec3 sun_color, bool light_enabled);

} // namespace nf::rendering
