#include <NF/Test/TestFramework.hpp>
#include <NF/Water/WaterRipple.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::water;

NF_TEST(ripple_grid_initialization_and_bounds) {
    WaterRippleOptions options;
    options.resolution = 64;
    options.world_size = 32.0f;
    options.world_center = Vec2{0.0f, 0.0f};

    WaterRippleGrid grid(options);
    NF_CHECK_EQ(grid.resolution(), 64u);
    NF_CHECK_NEAR(grid.cell_size(), 32.0f / 63.0f, 1e-4f);
    NF_CHECK_NEAR(grid.total_energy(), 0.0f, 1e-6f);

    // Sampling outside the grid returns 0.0
    NF_CHECK_NEAR(grid.sample_height(50.0f, 50.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(grid.sample_height(-50.0f, -50.0f), 0.0f, 1e-6f);
}

NF_TEST(ripple_impulse_injection_and_energy) {
    WaterRippleOptions options;
    options.resolution = 64;
    options.world_size = 32.0f;
    WaterRippleGrid grid(options);

    grid.add_ripple(Vec2{0.0f, 0.0f}, 2.0f, 1.0f);

    const f32 center_h = grid.sample_height(0.0f, 0.0f);
    NF_CHECK(center_h > 0.8f); // Center received peak cosine impulse
    NF_CHECK(grid.total_energy() > 0.1f);

    // Far point untouched
    NF_CHECK_NEAR(grid.sample_height(10.0f, 10.0f), 0.0f, 1e-4f);
}

NF_TEST(ripple_wave_equation_propagation) {
    WaterRippleOptions options;
    options.resolution = 64;
    options.world_size = 32.0f;
    options.wave_speed = 6.0f;
    WaterRippleGrid grid(options);

    grid.add_ripple(Vec2{0.0f, 0.0f}, 1.5f, 1.0f);
    const f32 init_center = grid.sample_height(0.0f, 0.0f);

    // Advance 0.25 seconds: wave propagates outward
    grid.step(0.25f);

    // Center height has dropped as energy expands in a ring
    const f32 post_center = grid.sample_height(0.0f, 0.0f);
    NF_CHECK(post_center < init_center);

    // Ring at radius ~1.5m should now be perturbed
    const f32 ring_h = grid.sample_height(1.5f, 0.0f);
    NF_CHECK(std::abs(ring_h) > 0.01f);
}

NF_TEST(ripple_energy_damping_dissipation) {
    WaterRippleOptions options;
    options.resolution = 64;
    options.world_size = 32.0f;
    options.damping = 0.95f; // Faster dissipation for test
    WaterRippleGrid grid(options);

    grid.add_ripple(Vec2{0.0f, 0.0f}, 2.0f, 1.0f);
    const f32 energy_t0 = grid.total_energy();

    grid.step(0.5f);
    const f32 energy_t1 = grid.total_energy();
    NF_CHECK(energy_t1 < energy_t0);

    grid.step(1.5f);
    const f32 energy_t2 = grid.total_energy();
    NF_CHECK(energy_t2 < energy_t1);
}

NF_TEST(ripple_normal_perturbation_and_central_difference) {
    WaterRippleOptions options;
    options.resolution = 64;
    options.world_size = 32.0f;
    WaterRippleGrid grid(options);

    // Initially flat surface has normal pointing up (0, 1, 0)
    const Vec3 init_normal = grid.sample_normal(0.0f, 0.0f);
    NF_CHECK_NEAR(init_normal.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(init_normal.y, 1.0f, 1e-4f);
    NF_CHECK_NEAR(init_normal.z, 0.0f, 1e-4f);

    // Add ripple at center
    grid.add_ripple(Vec2{0.0f, 0.0f}, 2.0f, 1.0f);

    // On the slope (x = 1.0, z = 0), normal should tilt
    const Vec3 slope_normal = grid.sample_normal(1.0f, 0.0f);
    NF_CHECK(slope_normal.x > 0.0f || slope_normal.x < 0.0f); // Tilted
    NF_CHECK(slope_normal.y > 0.5f);
    NF_CHECK_NEAR(slope_normal.length(), 1.0f, 1e-4f); // Normalized unit vector
}

NF_TEST(wake_injection_by_moving_body) {
    WaterRippleOptions options;
    options.resolution = 64;
    options.world_size = 32.0f;
    WaterRippleGrid grid(options);

    // Stationary body generates no wake
    grid.add_wake(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 1.5f, 1.0f, 0.016f);
    NF_CHECK_NEAR(grid.total_energy(), 0.0f, 1e-6f);

    // Moving boat / wading character
    grid.add_wake(Vec3{0.0f, 0.0f, 0.0f}, Vec3{4.0f, 0.0f, 0.0f}, 1.5f, 1.0f, 0.016f);
    NF_CHECK(grid.total_energy() > 0.0f);
    NF_CHECK(grid.sample_height(0.0f, 0.0f) > 0.0f);
}

NF_TEST(water_combined_evaluation_gerstner_and_ripples) {
    WaterRippleOptions ripple_opts;
    ripple_opts.resolution = 64;
    ripple_opts.world_size = 40.0f;
    WaterRippleGrid grid(ripple_opts);

    // Setup Gerstner waves
    std::vector<Wave> waves{
        Wave{0.5f, 16.0f, 1.5f, Vec2{1.0f, 0.0f}, 0.5f}
    };
    WaterOptions water_opts;
    water_opts.level = 0.0f;

    // Sample pure Gerstner
    const f32 pure_h = water_height(0.0f, 0.0f, waves, water_opts);

    // Add local ripple at (0, 0)
    grid.add_ripple(Vec2{0.0f, 0.0f}, 2.0f, 0.8f);
    const f32 ripple_delta = grid.sample_height(0.0f, 0.0f);

    // Combined evaluation
    CombinedWaterSample combined = grid.evaluate_combined(0.0f, 0.0f, waves, water_opts);

    NF_CHECK_NEAR(combined.height, pure_h + ripple_delta, 1e-3f);
    NF_CHECK_NEAR(combined.normal.length(), 1.0f, 1e-3f);
}
