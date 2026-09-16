// RHITests — procedural sky: CPU mirror + renderer state.
//
// compute_sky_color() is the exact CPU twin of the lighting.frag sky branch,
// so these tests pin the look without a GPU. Renderer3D::set_sky is pure
// state (no device), covered here too.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/Sky.hpp>

#include <cmath>

using namespace nf;
using namespace nf::rendering;

namespace {

Vec3 dir(float x, float y, float z) {
    return Vec3{x, y, z}.normalized();
}

} // namespace

NF_TEST(sky_up_ray_is_zenith_without_light) {
    SkyParams sky;
    const Vec3 c = compute_sky_color(sky, dir(0, 1, 0), dir(0, 1, 0), Vec3{1, 1, 1}, false);
    NF_CHECK_NEAR(c.x, sky.zenith.x, 1e-5f);
    NF_CHECK_NEAR(c.y, sky.zenith.y, 1e-5f);
    NF_CHECK_NEAR(c.z, sky.zenith.z, 1e-5f);
}

NF_TEST(sky_horizontal_ray_is_horizon) {
    SkyParams sky;
    const Vec3 c = compute_sky_color(sky, dir(1, 0, 0), dir(0, 1, 0), Vec3{1, 1, 1}, false);
    NF_CHECK_NEAR(c.x, sky.horizon.x, 1e-5f);
    NF_CHECK_NEAR(c.y, sky.horizon.y, 1e-5f);
    NF_CHECK_NEAR(c.z, sky.horizon.z, 1e-5f);
}

NF_TEST(sky_down_ray_is_ground) {
    SkyParams sky;
    const Vec3 c = compute_sky_color(sky, dir(0, -1, 0), dir(0, 1, 0), Vec3{1, 1, 1}, false);
    NF_CHECK_NEAR(c.x, sky.ground.x, 1e-5f);
    NF_CHECK_NEAR(c.y, sky.ground.y, 1e-5f);
    NF_CHECK_NEAR(c.z, sky.ground.z, 1e-5f);
}

NF_TEST(sky_sun_disk_boosts_along_sun_dir) {
    SkyParams sky;
    const Vec3 sun = dir(0, 1, 0);
    const Vec3 with_sun =
        compute_sky_color(sky, sun, sun, Vec3{1, 1, 1}, true);
    const Vec3 without_light =
        compute_sky_color(sky, sun, sun, Vec3{1, 1, 1}, false);
    // disk(1.0) = 1, glow(1.0) = 0.6 + 0.12: boost = 4.0 * 1 + 0.72.
    NF_CHECK_NEAR(with_sun.x - without_light.x, 4.72f, 1e-3f);
    NF_CHECK_NEAR(with_sun.y - without_light.y, 4.72f, 1e-3f);
    NF_CHECK_NEAR(with_sun.z - without_light.z, 4.72f, 1e-3f);
}

NF_TEST(sky_sun_multipliers_scale_the_boost) {
    SkyParams sky;
    sky.sun_disk = 0.0f;
    sky.sun_glow = 0.0f;
    const Vec3 sun = dir(0, 1, 0);
    const Vec3 muted = compute_sky_color(sky, sun, sun, Vec3{2, 2, 2}, true);
    const Vec3 plain = compute_sky_color(sky, sun, sun, Vec3{2, 2, 2}, false);
    NF_CHECK_NEAR(muted.x, plain.x, 1e-5f);
    NF_CHECK_NEAR(muted.y, plain.y, 1e-5f);
    NF_CHECK_NEAR(muted.z, plain.z, 1e-5f);
}

NF_TEST(sky_disabled_returns_clear) {
    SkyParams sky;
    sky.enabled = false;
    const Vec3 c = compute_sky_color(sky, dir(0, 1, 0), dir(0, 1, 0), Vec3{5, 5, 5}, true);
    NF_CHECK_NEAR(c.x, sky.clear.x, 1e-6f);
    NF_CHECK_NEAR(c.y, sky.clear.y, 1e-6f);
    NF_CHECK_NEAR(c.z, sky.clear.z, 1e-6f);
}

NF_TEST(sky_gradient_midpoint_matches_pow_curve) {
    SkyParams sky;
    // 45 degrees up: h = sqrt(0.5), mix factor pow(h, 0.6).
    const Vec3 c = compute_sky_color(sky, dir(1, 1, 0), dir(0, 1, 0), Vec3{1, 1, 1}, false);
    const float h = 0.70710678f;
    const float t = std::pow(h, 0.6f);
    NF_CHECK_NEAR(c.x, sky.horizon.x + (sky.zenith.x - sky.horizon.x) * t, 1e-4f);
    NF_CHECK_NEAR(c.z, sky.horizon.z + (sky.zenith.z - sky.horizon.z) * t, 1e-4f);
}

NF_TEST(renderer_sky_state_roundtrip_needs_no_device) {
    Renderer3D renderer;
    SkyParams custom;
    custom.zenith = Vec3{0.1f, 0.2f, 0.3f};
    custom.sun_disk = 2.5f;
    custom.enabled = false;
    renderer.set_sky(custom);
    const SkyParams& back = renderer.sky();
    NF_CHECK_NEAR(back.zenith.x, 0.1f, 1e-6f);
    NF_CHECK_NEAR(back.sun_disk, 2.5f, 1e-6f);
    NF_CHECK(!back.enabled);
    // Defaults match the long-standing hardcoded look.
    Renderer3D fresh;
    NF_CHECK_NEAR(fresh.sky().zenith.y, 0.42f, 1e-6f);
    NF_CHECK_NEAR(fresh.sky().horizon.x, 0.62f, 1e-6f);
    NF_CHECK(fresh.sky().enabled);
}
