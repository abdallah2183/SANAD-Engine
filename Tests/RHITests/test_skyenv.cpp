// RHITests — sky environment bake (SkyEnv): the CPU side of image-based
// lighting.
//
// bake_sky_env() evaluates the procedural sky into an equirect HDR image the
// renderer uploads. These tests pin the bake without a GPU: the mapping
// roundtrip (which must invert the shader's env_equirect_uv or the sun's
// reflection lands on the wrong side of every glossy surface), the half-float
// packing, the baked content (zenith/horizon/sun where they belong), and the
// dirty key the renderer uses to skip re-bakes.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/Sky.hpp>
#include <NF/Rendering/SkyEnv.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace nf;
using namespace nf::rendering;

namespace {

Vec3 dir(float x, float y, float z) {
    return Vec3{x, y, z}.normalized();
}

// Baked texel as float rgb.
Vec3 texel(const std::vector<u16>& px, int w, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                      static_cast<size_t>(x)) *
                     4u;
    return Vec3{half_to_float(px[i]), half_to_float(px[i + 1]),
                half_to_float(px[i + 2])};
}

} // namespace

NF_TEST(skyenv_equirect_roundtrip_inverts_the_shader_mapping) {
    // A grid over the sphere, poles and seam included: uv -> dir -> uv must
    // come home, or the bake and env_equirect_uv disagree about where the
    // sun is.
    for (int yi = 0; yi <= 16; ++yi) {
        for (int xi = 0; xi <= 32; ++xi) {
            const float u = static_cast<float>(xi) / 32.0f;
            const float v = 0.001f + 0.998f * static_cast<float>(yi) / 16.0f;
            const Vec3 d = equirect_direction(u, v);
            NF_CHECK_NEAR(d.length(), 1.0f, 1e-5f);
            float ru = 0.0f, rv = 0.0f;
            equirect_uv(d, ru, rv);
            // u wraps at the seam (0 == 1): compare angularly.
            float du = std::fabs(ru - u);
            du = std::min(du, 1.0f - du);
            NF_CHECK(du < 1e-4f);
            NF_CHECK_NEAR(rv, v, 1e-4f);
        }
    }
}

NF_TEST(skyenv_half_float_roundtrip_is_tight_in_hdr_range) {
    for (float v : {0.0f, 0.001f, 0.055f, 0.5f, 1.0f, 4.72f, 20.0f, 100.0f}) {
        const float back = half_to_float(float_to_half(v));
        NF_CHECK_NEAR(back, v, std::max(v * 0.002f, 1e-4f));
    }
}

NF_TEST(skyenv_half_float_clamps_instead_of_propagating_inf) {
    // Negatives (unphysical here) and NaN must not become payloads the mip
    // blur would smear across the bake.
    NF_CHECK(float_to_half(-1.0f) == 0);
    NF_CHECK(float_to_half(std::numeric_limits<float>::quiet_NaN()) == 0);
    NF_CHECK_NEAR(half_to_float(float_to_half(1e30f)), 65504.0f, 1.0f);
    NF_CHECK_NEAR(half_to_float(float_to_half(
                      std::numeric_limits<float>::infinity())),
                  65504.0f, 1.0f);
}

NF_TEST(skyenv_half_float_survives_subnormals) {
    // Tiny but legitimate HDR values (deep shadow ambient) must roundtrip
    // through the subnormal range instead of landing on garbage via an
    // unsigned exponent wrap. Tolerance is one half-ulp absolute: subnormals
    // quantize in absolute steps (2^-24), so a relative tolerance is
    // meaningless down here.
    for (float v : {1e-5f, 6.0e-8f, 1.0e-7f}) {
        const float back = half_to_float(float_to_half(v));
        NF_CHECK_NEAR(back, v, std::max(v * 0.002f, 6e-8f));
    }
}

NF_TEST(skyenv_bake_puts_zenith_up_and_ground_down) {
    SkyParams sky;
    std::vector<u16> px;
    bake_sky_env(px, kSkyEnvWidth, kSkyEnvHeight, sky, dir(0, 1, 0),
                 Vec3{1, 1, 1}, false);
    NF_CHECK(px.size() == static_cast<size_t>(kSkyEnvWidth) * kSkyEnvHeight * 4u);
    // Top row centre (v ~ 1): zenith. Bottom row centre (v ~ 0): ground.
    const Vec3 up = texel(px, kSkyEnvWidth, kSkyEnvWidth / 2, kSkyEnvHeight - 1);
    NF_CHECK_NEAR(up.x, sky.zenith.x, 0.02f);
    NF_CHECK_NEAR(up.z, sky.zenith.z, 0.02f);
    const Vec3 down = texel(px, kSkyEnvWidth, kSkyEnvWidth / 2, 0);
    NF_CHECK_NEAR(down.x, sky.ground.x, 0.02f);
    // Alpha is always 1.
    NF_CHECK_NEAR(half_to_float(px[3]), 1.0f, 1e-3f);
}

NF_TEST(skyenv_bake_puts_the_sun_hotspot_along_the_sun) {
    SkyParams sky;
    const Vec3 sun = dir(0.3f, 0.5f, 0.2f);
    std::vector<u16> px;
    bake_sky_env(px, kSkyEnvWidth, kSkyEnvHeight, sky, sun, Vec3{3, 3, 3}, true);
    // The brightest texel must sit within a few texels of the sun direction:
    // the disk is what gives glossy surfaces their glint.
    float su = 0.0f, sv = 0.0f;
    equirect_uv(sun, su, sv);
    const int sx = static_cast<int>(su * kSkyEnvWidth);
    const int sy = static_cast<int>(sv * kSkyEnvHeight);
    float best = -1.0f;
    int bx = -1, by = -1;
    for (int y = 0; y < kSkyEnvHeight; ++y) {
        for (int x = 0; x < kSkyEnvWidth; ++x) {
            const Vec3 c = texel(px, kSkyEnvWidth, x, y);
            const float lum = c.x + c.y + c.z;
            if (lum > best) {
                best = lum;
                bx = x;
                by = y;
            }
        }
    }
    NF_CHECK(best > 3.0f); // the disk boost survived the bake
    const int dx = std::abs(bx - sx);
    const int wrapped = std::min(dx, kSkyEnvWidth - dx);
    NF_CHECK(wrapped <= 2);
    NF_CHECK(std::abs(by - sy) <= 2);
}

NF_TEST(skyenv_bake_is_deterministic) {
    SkyParams sky;
    sky.cloud_coverage = 0.7f; // must not matter: clouds are never baked
    std::vector<u16> a, b;
    bake_sky_env(a, 32, 16, sky, dir(0, 1, 0), Vec3{2, 2, 2}, true);
    bake_sky_env(b, 32, 16, sky, dir(0, 1, 0), Vec3{2, 2, 2}, true);
    NF_CHECK(a.size() == b.size());
    NF_CHECK(a == b);
}

NF_TEST(skyenv_disabled_sky_bakes_flat_clear) {
    SkyParams sky;
    sky.enabled = false;
    std::vector<u16> px;
    bake_sky_env(px, 16, 8, sky, dir(0, 1, 0), Vec3{5, 5, 5}, true);
    for (size_t i = 0; i < px.size(); i += 4) {
        NF_CHECK_NEAR(half_to_float(px[i]), sky.clear.x, 1e-2f);
        NF_CHECK_NEAR(half_to_float(px[i + 2]), sky.clear.z, 1e-2f);
    }
}

NF_TEST(skyenv_key_matches_itself_and_spots_movement) {
    SkyParams sky;
    const SkyEnvKey k0 = SkyEnvKey::make(sky, dir(0, 1, 0), Vec3{1, 1, 1}, true);
    NF_CHECK(k0.matches(k0));
    // Sub-epsilon jitter: no re-bake.
    const SkyEnvKey k1 = SkyEnvKey::make(sky, dir(0, 1, 0), Vec3{1.0001f, 1, 1}, true);
    NF_CHECK(k0.matches(k1));
    // A moved sun: re-bake.
    const SkyEnvKey k2 = SkyEnvKey::make(sky, dir(0.1f, 1, 0), Vec3{1, 1, 1}, true);
    NF_CHECK(!k0.matches(k2));
    // A regraded sky: re-bake.
    SkyParams changed = sky;
    changed.horizon = Vec3{0.9f, 0.5f, 0.4f};
    const SkyEnvKey k3 = SkyEnvKey::make(changed, dir(0, 1, 0), Vec3{1, 1, 1}, true);
    NF_CHECK(!k0.matches(k3));
}

NF_TEST(skyenv_renderer_iblis_pure_state) {
    Renderer3D renderer;
    NF_CHECK(renderer.ibl_enabled()); // on by default: the quality goal
    renderer.set_ibl_enabled(false);
    NF_CHECK(!renderer.ibl_enabled());
    renderer.set_ibl_enabled(true);
    renderer.set_ibl_intensity(0.5f, 0.25f);
    NF_CHECK(renderer.ibl_enabled());
}
