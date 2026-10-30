#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/LookAtIK.hpp>

#include <cmath>

using namespace nf;
using namespace nf::animation;

NF_TEST(look_at_ik_initial_state) {
    LookAtSolver solver;
    NF_CHECK_EQ(solver.bone_count(), 0ULL);
    const auto& res = solver.result();
    NF_CHECK_NEAR(res.current_yaw_deg, 0.0f, 1e-4f);
    NF_CHECK_NEAR(res.current_pitch_deg, 0.0f, 1e-4f);
    NF_CHECK(!res.clamped);
}

NF_TEST(look_at_ik_straight_ahead) {
    LookAtSolver solver;
    Vec3 char_pos{0.0f, 1.5f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};
    Vec3 target{0.0f, 1.5f, 10.0f}; // Directly ahead

    auto res = solver.update(char_pos, char_facing, target, 1.0f);
    NF_CHECK_NEAR(res.target_yaw_deg, 0.0f, 1e-3f);
    NF_CHECK_NEAR(res.target_pitch_deg, 0.0f, 1e-3f);
    NF_CHECK(!res.clamped);
    NF_CHECK(res.in_deadzone);
}

NF_TEST(look_at_ik_yaw_horizontal) {
    LookAtConfig cfg;
    cfg.smoothing_speed = 100.0f; // Fast convergence
    LookAtSolver solver(cfg);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};
    // Target 45 degrees to the right (+X, +Z)
    Vec3 target{10.0f, 0.0f, 10.0f};

    auto res = solver.update(char_pos, char_facing, target, 1.0f);
    NF_CHECK_NEAR(res.target_yaw_deg, 45.0f, 0.1f);
    NF_CHECK_NEAR(res.target_pitch_deg, 0.0f, 0.1f);
    NF_CHECK(!res.clamped);
    NF_CHECK(!res.in_deadzone);
    NF_CHECK_NEAR(res.current_yaw_deg, 45.0f, 0.5f);
}

NF_TEST(look_at_ik_pitch_vertical) {
    LookAtConfig cfg;
    cfg.smoothing_speed = 100.0f;
    LookAtSolver solver(cfg);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};
    // Target elevated upwards at 30 degrees (Z=10, Y=10 * tan(30 deg) = 5.7735)
    Vec3 target{0.0f, 5.7735f, 10.0f};

    auto res = solver.update(char_pos, char_facing, target, 1.0f);
    NF_CHECK_NEAR(res.target_pitch_deg, 30.0f, 0.2f);
    NF_CHECK_NEAR(res.target_yaw_deg, 0.0f, 0.2f);
    NF_CHECK(!res.clamped);
    NF_CHECK_NEAR(res.current_pitch_deg, 30.0f, 0.5f);
}

NF_TEST(look_at_ik_clamp_boundaries) {
    LookAtConfig cfg;
    cfg.limits.yaw_min_deg = -60.0f;
    cfg.limits.yaw_max_deg = 60.0f;
    cfg.limits.pitch_min_deg = -30.0f;
    cfg.limits.pitch_max_deg = 30.0f;
    LookAtSolver solver(cfg);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};

    // Target at 90 degrees right (X=10, Z=0)
    Vec3 target{10.0f, 0.0f, 0.0f};
    auto res = solver.update(char_pos, char_facing, target, 0.0f);
    NF_CHECK_NEAR(res.target_yaw_deg, 90.0f, 0.1f);
    NF_CHECK(res.clamped);
}

NF_TEST(look_at_ik_deadzone_filtering) {
    LookAtConfig cfg;
    cfg.limits.deadzone_angle_deg = 2.0f;
    LookAtSolver solver(cfg);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};

    // Slight deviation: ~0.5 degree
    Vec3 target{0.087f, 0.0f, 10.0f};
    auto res = solver.update(char_pos, char_facing, target, 0.0f);
    NF_CHECK(res.in_deadzone);
}

NF_TEST(look_at_ik_bone_chain_distribution) {
    LookAtConfig cfg;
    cfg.smoothing_speed = 100.0f;
    LookAtSolver solver(cfg);

    solver.add_bone("spine_01", 1.0f);
    solver.add_bone("neck", 1.0f);
    solver.add_bone("head", 2.0f); // 50% of weight

    NF_CHECK_EQ(solver.bone_count(), 3ULL);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};
    Vec3 target{10.0f, 0.0f, 10.0f}; // 45 deg yaw

    solver.update(char_pos, char_facing, target, 1.0f);

    const auto* spine = solver.get_bone(0);
    const auto* neck = solver.get_bone(1);
    const auto* head = solver.get_bone(2);

    NF_CHECK(spine != nullptr);
    NF_CHECK(neck != nullptr);
    NF_CHECK(head != nullptr);

    // Spine and neck have identical weight 1.0f/4.0f = 0.25
    NF_CHECK_NEAR(spine->current_offset.w, neck->current_offset.w, 1e-3f);
    // Head has higher weight (2.0f/4.0f = 0.5), so its rotation deviation from identity is greater (smaller w)
    NF_CHECK(head->current_offset.w < spine->current_offset.w);
}

NF_TEST(look_at_ik_global_weight_blend) {
    LookAtConfig cfg;
    cfg.smoothing_speed = 100.0f;
    cfg.global_weight = 0.0f; // Turn off bone deformation
    LookAtSolver solver(cfg);

    solver.add_bone("head", 1.0f);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};
    Vec3 target{10.0f, 0.0f, 10.0f};

    solver.update(char_pos, char_facing, target, 1.0f);

    const auto* head = solver.get_bone(0);
    // Identity rotation has w = 1.0
    NF_CHECK_NEAR(head->current_offset.w, 1.0f, 1e-4f);
    NF_CHECK_NEAR(head->current_offset.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(head->current_offset.y, 0.0f, 1e-4f);
    NF_CHECK_NEAR(head->current_offset.z, 0.0f, 1e-4f);
}

NF_TEST(look_at_ik_smooth_temporal_tracking) {
    LookAtConfig cfg;
    cfg.smoothing_speed = 5.0f; // Moderate smoothing
    LookAtSolver solver(cfg);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};
    Vec3 target{10.0f, 0.0f, 10.0f}; // 45 deg

    // Frame 1: small dt = 0.016s (60 FPS)
    auto res1 = solver.update(char_pos, char_facing, target, 0.016f);
    NF_CHECK(res1.current_yaw_deg > 0.0f);
    NF_CHECK(res1.current_yaw_deg < 45.0f);

    // Frame 2: advances further
    auto res2 = solver.update(char_pos, char_facing, target, 0.05f);
    NF_CHECK(res2.current_yaw_deg > res1.current_yaw_deg);
}

NF_TEST(look_at_ik_reset) {
    LookAtSolver solver;
    solver.add_bone("head", 1.0f);

    Vec3 char_pos{0.0f, 0.0f, 0.0f};
    Vec3 char_facing{0.0f, 0.0f, 1.0f};
    Vec3 target{10.0f, 0.0f, 10.0f};

    solver.update(char_pos, char_facing, target, 1.0f);
    solver.reset();

    const auto& res = solver.result();
    NF_CHECK_NEAR(res.current_yaw_deg, 0.0f, 1e-4f);
    NF_CHECK_NEAR(res.current_pitch_deg, 0.0f, 1e-4f);
    const auto* head = solver.get_bone(0);
    NF_CHECK_NEAR(head->current_offset.w, 1.0f, 1e-4f);
}
