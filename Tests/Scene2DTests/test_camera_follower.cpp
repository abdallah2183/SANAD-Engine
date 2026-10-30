#include <NF/Test/TestFramework.hpp>
#include <NF/Scene2D/CameraFollower2D.hpp>

#include <cmath>

using namespace nf;
using namespace nf::scene2d;

NF_TEST(camera_follower_snap_and_initial_state) {
    CameraFollower2D follower;
    follower.snap_to(Vec2{50.0f, 25.0f});

    NF_CHECK_NEAR(follower.current_position().x, 50.0f, 1e-4f);
    NF_CHECK_NEAR(follower.current_position().y, 25.0f, 1e-4f);
    NF_CHECK_NEAR(follower.target_focus().x, 50.0f, 1e-4f);
    NF_CHECK_NEAR(follower.target_focus().y, 25.0f, 1e-4f);
    NF_CHECK_NEAR(follower.final_position().x, 50.0f, 1e-4f);
    NF_CHECK_NEAR(follower.shake_roll_deg(), 0.0f, 1e-4f);
    NF_CHECK_NEAR(follower.trauma(), 0.0f, 1e-4f);
}

NF_TEST(camera_follower_deadzone_confinement_and_push) {
    CameraFollower2DConfig cfg;
    cfg.deadzone = Vec2{2.0f, 1.5f};
    cfg.smooth_time = Vec2{0.001f, 0.001f}; // Instant tracking for deadzone test
    CameraFollower2D follower(cfg);
    follower.snap_to(Vec2{0.0f, 0.0f});

    // 1. Move target within deadzone: (+1.5, +1.0) -> inside (+-2.0, +-1.5)
    // Target focus should remain exactly at (0, 0)
    follower.update(Vec2{1.5f, 1.0f}, Vec2{0.0f, 0.0f}, 0.1f);
    NF_CHECK_NEAR(follower.target_focus().x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(follower.target_focus().y, 0.0f, 1e-4f);

    // 2. Move target beyond deadzone to x = 5.0f (exceeds 2.0f by 3.0f)
    // Target focus should be pushed to 5.0 - 2.0 = 3.0f
    follower.update(Vec2{5.0f, 1.0f}, Vec2{0.0f, 0.0f}, 0.1f);
    NF_CHECK_NEAR(follower.target_focus().x, 3.0f, 1e-4f);
    NF_CHECK_NEAR(follower.target_focus().y, 0.0f, 1e-4f);

    // 3. Move target negative along Y: y = -4.0f (exceeds -1.5f by -2.5f)
    // Target focus y should be pushed to -4.0 + 1.5 = -2.5f
    follower.update(Vec2{5.0f, -4.0f}, Vec2{0.0f, 0.0f}, 0.1f);
    NF_CHECK_NEAR(follower.target_focus().x, 3.0f, 1e-4f);
    NF_CHECK_NEAR(follower.target_focus().y, -2.5f, 1e-4f);
}

NF_TEST(camera_follower_velocity_lookahead) {
    CameraFollower2DConfig cfg;
    cfg.deadzone = Vec2{0.0f, 0.0f};
    cfg.lookahead_factor = Vec2{0.5f, 0.5f};
    cfg.lookahead_smooth_time = 0.01f;
    cfg.smooth_time = Vec2{0.01f, 0.01f};
    CameraFollower2D follower(cfg);
    follower.snap_to(Vec2{10.0f, 10.0f});

    // Player running right with velocity (20, 0)
    // Lookahead target = 20 * 0.5 = 10 units ahead of target
    for (int i = 0; i < 20; ++i) {
        follower.update(Vec2{10.0f, 10.0f}, Vec2{20.0f, 0.0f}, 0.05f);
    }

    // Camera position should be ahead of player position (> 10.0f, approaching 20.0f)
    NF_CHECK(follower.current_position().x > 15.0f);
    NF_CHECK_NEAR(follower.current_position().y, 10.0f, 1e-2f);
}

NF_TEST(camera_follower_smooth_damp_interpolation) {
    CameraFollower2DConfig cfg;
    cfg.deadzone = Vec2{0.0f, 0.0f};
    cfg.lookahead_factor = Vec2{0.0f, 0.0f};
    cfg.smooth_time = Vec2{0.2f, 0.2f};
    CameraFollower2D follower(cfg);
    follower.snap_to(Vec2{0.0f, 0.0f});

    // Target jumps to (100.0, 0.0)
    follower.update(Vec2{100.0f, 0.0f}, Vec2{0.0f, 0.0f}, 0.05f);

    // Initial movement should be smooth, not jumping to 100
    NF_CHECK(follower.current_position().x > 0.0f);
    NF_CHECK(follower.current_position().x < 50.0f);

    // Run for 1.0 second (5x smooth_time) -> should reach ~100.0f without overshooting
    for (int i = 0; i < 20; ++i) {
        follower.update(Vec2{100.0f, 0.0f}, Vec2{0.0f, 0.0f}, 0.05f);
    }
    NF_CHECK_NEAR(follower.current_position().x, 100.0f, 0.5f);
    NF_CHECK(follower.current_position().x <= 100.05f); // No significant overshoot
}

NF_TEST(camera_follower_boundary_clamping) {
    CameraFollower2DConfig cfg;
    cfg.deadzone = Vec2{0.0f, 0.0f};
    cfg.smooth_time = Vec2{0.001f, 0.001f};
    cfg.bounds_enabled = true;
    cfg.bounds_min = Vec2{-50.0f, -20.0f};
    cfg.bounds_max = Vec2{50.0f, 20.0f};
    CameraFollower2D follower(cfg);
    follower.snap_to(Vec2{0.0f, 0.0f});

    // Target goes to x = 200 (outside max 50)
    for (int i = 0; i < 10; ++i) {
        follower.update(Vec2{200.0f, 10.0f}, Vec2{0.0f, 0.0f}, 0.02f);
    }
    NF_CHECK_NEAR(follower.current_position().x, 50.0f, 1e-3f);
    NF_CHECK_NEAR(follower.current_position().y, 10.0f, 1e-3f);

    // Target goes to y = -100 (outside min -20)
    for (int i = 0; i < 10; ++i) {
        follower.update(Vec2{0.0f, -100.0f}, Vec2{0.0f, 0.0f}, 0.02f);
    }
    NF_CHECK_NEAR(follower.current_position().x, 0.0f, 1e-3f);
    NF_CHECK_NEAR(follower.current_position().y, -20.0f, 1e-3f);
}

NF_TEST(camera_follower_trauma_shake_decay_and_camera_application) {
    CameraFollower2DConfig cfg;
    cfg.trauma_decay = 2.0f; // 2.0 per second
    cfg.max_shake_offset = Vec2{4.0f, 4.0f};
    cfg.max_shake_roll_deg = 10.0f;
    CameraFollower2D follower(cfg);
    follower.snap_to(Vec2{10.0f, 20.0f});

    // Add trauma = 0.8
    follower.add_trauma(0.8f);
    NF_CHECK_NEAR(follower.trauma(), 0.8f, 1e-4f);

    // Add extra trauma: clamps to 1.0
    follower.add_trauma(0.5f);
    NF_CHECK_NEAR(follower.trauma(), 1.0f, 1e-4f);

    // Update 0.1s
    follower.update(Vec2{10.0f, 20.0f}, Vec2{0.0f, 0.0f}, 0.1f);

    // Trauma decays: 1.0 - 2.0 * 0.1 = 0.8
    NF_CHECK_NEAR(follower.trauma(), 0.8f, 1e-4f);

    // Final position should have shake offset applied
    NF_CHECK((follower.final_position() - follower.current_position()).length_sq() > 1e-4f);

    // Apply to Camera2D
    Camera2D cam;
    cam.position = Vec2{0.0f, 0.0f};
    cam.rotation_deg = 0.0f;
    follower.apply_to_camera(cam);

    NF_CHECK_NEAR(cam.position.x, follower.final_position().x, 1e-4f);
    NF_CHECK_NEAR(cam.position.y, follower.final_position().y, 1e-4f);
    NF_CHECK_NEAR(cam.rotation_deg, follower.shake_roll_deg(), 1e-4f);

    // Update 1.0s more -> trauma should be 0.0
    follower.update(Vec2{10.0f, 20.0f}, Vec2{0.0f, 0.0f}, 1.0f);
    NF_CHECK_NEAR(follower.trauma(), 0.0f, 1e-4f);
    NF_CHECK_NEAR(follower.final_position().x, follower.current_position().x, 1e-4f);
    NF_CHECK_NEAR(follower.final_position().y, follower.current_position().y, 1e-4f);
    NF_CHECK_NEAR(follower.shake_roll_deg(), 0.0f, 1e-4f);
}
