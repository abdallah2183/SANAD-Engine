#include <NF/Rendering/LightProbeVolume.hpp>
#include <NF/Test/TestFramework.hpp>
#include <cmath>

using namespace nf;
using namespace nf::rendering;

NF_TEST(sh9_default_initialization_is_zero) {
    const SH9Color sh{};
    for (size_t i = 0; i < 9; ++i) {
        NF_CHECK_NEAR(sh.coeffs[i].x, 0.0f, 1e-6f);
        NF_CHECK_NEAR(sh.coeffs[i].y, 0.0f, 1e-6f);
        NF_CHECK_NEAR(sh.coeffs[i].z, 0.0f, 1e-6f);
    }
    const Vec3 eval = sh.evaluate(Vec3{0, 1, 0});
    NF_CHECK_NEAR(eval.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(eval.y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(eval.z, 0.0f, 1e-6f);
}

NF_TEST(sh9_constant_ambient_monopole) {
    const Vec3 ambient{0.5f, 0.75f, 1.0f};
    const SH9Color sh(ambient);

    // Constant ambient should evaluate nearly identically in all directions
    const Vec3 up = sh.evaluate(Vec3{0, 1, 0});
    const Vec3 down = sh.evaluate(Vec3{0, -1, 0});
    const Vec3 left = sh.evaluate(Vec3{-1, 0, 0});
    const Vec3 forward = sh.evaluate(Vec3{0, 0, 1});

    NF_CHECK_NEAR(up.x, ambient.x, 0.01f);
    NF_CHECK_NEAR(up.y, ambient.y, 0.01f);
    NF_CHECK_NEAR(up.z, ambient.z, 0.01f);

    NF_CHECK_NEAR(down.x, ambient.x, 0.01f);
    NF_CHECK_NEAR(down.y, ambient.y, 0.01f);
    NF_CHECK_NEAR(down.z, ambient.z, 0.01f);

    NF_CHECK_NEAR(left.x, ambient.x, 0.01f);
    NF_CHECK_NEAR(forward.x, ambient.x, 0.01f);
}

NF_TEST(sh9_directional_light_peak_direction) {
    // Light coming from overhead downwards: light_dir = (0, -1, 0)
    // Thus surface normal facing UP (0, 1, 0) should receive maximum irradiance,
    // while surface facing DOWN (0, -1, 0) should receive zero or minimum.
    const Vec3 light_dir{0, -1, 0};
    const Vec3 light_col{1.0f, 0.8f, 0.6f};
    const SH9Color sh = SH9Color::from_directional_light(light_dir, light_col);

    const Vec3 lit = sh.evaluate(Vec3{0, 1, 0});
    const Vec3 unlit = sh.evaluate(Vec3{0, -1, 0});

    NF_CHECK(lit.x > 0.3f);
    NF_CHECK(lit.y > 0.2f);
    NF_CHECK(lit.x > unlit.x * 2.0f);
}

NF_TEST(sh9_sky_hemisphere_vertical_gradient) {
    const Vec3 zenith{0.2f, 0.4f, 0.9f};   // Blue sky
    const Vec3 horizon{0.6f, 0.7f, 0.8f};  // Haze
    const Vec3 ground{0.1f, 0.1f, 0.05f};  // Dark ground

    const SH9Color sh = SH9Color::from_sky_hemisphere(zenith, horizon, ground);

    const Vec3 up = sh.evaluate(Vec3{0, 1, 0});
    const Vec3 down = sh.evaluate(Vec3{0, -1, 0});

    // Looking up at sky should be noticeably bluer and brighter than looking down at ground
    NF_CHECK(up.z > down.z);
    NF_CHECK(up.y > down.y);
    NF_CHECK(up.x > down.x);
}

NF_TEST(sh9_addition_and_scalar_multiplication) {
    const SH9Color a(Vec3{0.2f, 0.2f, 0.2f});
    const SH9Color b(Vec3{0.3f, 0.3f, 0.3f});

    const SH9Color sum = a + b;
    const Vec3 eval_sum = sum.evaluate(Vec3{0, 1, 0});
    NF_CHECK_NEAR(eval_sum.x, 0.5f, 0.02f);

    const SH9Color scaled = a * 3.0f;
    const Vec3 eval_scaled = scaled.evaluate(Vec3{0, 1, 0});
    NF_CHECK_NEAR(eval_scaled.x, 0.6f, 0.02f);
}

NF_TEST(sh9_lerp_interpolation) {
    const SH9Color a(Vec3{0.0f, 0.0f, 0.0f});
    const SH9Color b(Vec3{1.0f, 1.0f, 1.0f});

    const SH9Color mid = SH9Color::lerp(a, b, 0.5f);
    const Vec3 eval_mid = mid.evaluate(Vec3{1, 0, 0});
    NF_CHECK_NEAR(eval_mid.x, 0.5f, 0.02f);
}

NF_TEST(light_probe_volume_grid_initialization) {
    LightProbeVolume vol;
    NF_CHECK(!vol.is_valid());

    const Vec3 min_b{-10, 0, -10};
    const Vec3 max_b{10, 5, 10};
    vol.initialize_grid(min_b, max_b, 4, 3, 4);

    NF_CHECK(vol.is_valid());
    NF_CHECK_EQ(vol.dim_x(), 4u);
    NF_CHECK_EQ(vol.dim_y(), 3u);
    NF_CHECK_EQ(vol.dim_z(), 4u);
    NF_CHECK_EQ(vol.total_probes(), 4u * 3u * 4u);

    // Check corner positions
    const Vec3 p000 = vol.probe_position(0, 0, 0);
    NF_CHECK_NEAR(p000.x, -10.0f, 1e-4f);
    NF_CHECK_NEAR(p000.y, 0.0f, 1e-4f);
    NF_CHECK_NEAR(p000.z, -10.0f, 1e-4f);

    const Vec3 p323 = vol.probe_position(3, 2, 3);
    NF_CHECK_NEAR(p323.x, 10.0f, 1e-4f);
    NF_CHECK_NEAR(p323.y, 5.0f, 1e-4f);
    NF_CHECK_NEAR(p323.z, 10.0f, 1e-4f);
}

NF_TEST(light_probe_volume_trilinear_sampling) {
    LightProbeVolume vol;
    vol.initialize_grid(Vec3{0, 0, 0}, Vec3{10, 10, 10}, 2, 2, 2);

    // Set corner 0,0,0 to dark and corner 1,1,1 to bright
    vol.get_probe(0, 0, 0).sh = SH9Color(Vec3{0.0f, 0.0f, 0.0f});
    vol.get_probe(1, 1, 1).sh = SH9Color(Vec3{1.0f, 1.0f, 1.0f});

    // Sample center (5, 5, 5)
    const SH9Color center_sh = vol.sample_irradiance(Vec3{5.0f, 5.0f, 5.0f});
    const Vec3 eval = center_sh.evaluate(Vec3{0, 1, 0});

    // Trilinear weight from corner (1,1,1) at center is 0.5 * 0.5 * 0.5 = 0.125
    NF_CHECK(eval.x > 0.05f);
    NF_CHECK(eval.x < 0.25f);

    // Sample near (10, 10, 10)
    const SH9Color near_max = vol.sample_irradiance(Vec3{10.0f, 10.0f, 10.0f});
    const Vec3 eval_max = near_max.evaluate(Vec3{0, 1, 0});
    NF_CHECK_NEAR(eval_max.x, 1.0f, 0.05f);
}

NF_TEST(light_probe_volume_bake_point_light_attenuation) {
    LightProbeVolume vol;
    vol.initialize_grid(Vec3{0, 0, 0}, Vec3{20, 0, 0}, 5, 2, 2); // 1D-like along X

    const Vec3 light_pos{0.0f, 0.0f, 0.0f};
    const Vec3 light_col{10.0f, 5.0f, 1.0f};
    vol.bake_point_light(light_pos, light_col, 15.0f);

    // Probe near light at x=0 should have significantly higher irradiance than probe at x=10
    const Vec3 near_eval = vol.evaluate_irradiance(Vec3{0.5f, 0, 0}, Vec3{1, 0, 0});
    const Vec3 far_eval = vol.evaluate_irradiance(Vec3{10.0f, 0, 0}, Vec3{1, 0, 0});
    const Vec3 beyond_eval = vol.evaluate_irradiance(Vec3{19.0f, 0, 0}, Vec3{1, 0, 0});

    NF_CHECK(near_eval.x > far_eval.x);
    NF_CHECK_NEAR(beyond_eval.x, 0.0f, 1e-4f); // outside radius 15
}

NF_TEST(light_probe_volume_clear_and_reinitialize) {
    LightProbeVolume vol;
    vol.initialize_grid(Vec3{0, 0, 0}, Vec3{10, 10, 10}, 3, 3, 3);
    vol.bake_sky(Vec3{1, 1, 1}, Vec3{1, 1, 1}, Vec3{1, 1, 1});

    Vec3 eval = vol.evaluate_irradiance(Vec3{5, 5, 5}, Vec3{0, 1, 0});
    NF_CHECK(eval.x > 0.5f);

    vol.clear();
    eval = vol.evaluate_irradiance(Vec3{5, 5, 5}, Vec3{0, 1, 0});
    NF_CHECK_NEAR(eval.x, 0.0f, 1e-5f);
}
