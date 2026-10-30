#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/FootPlacementIK.hpp>

#include <cmath>

using namespace nf;
using namespace nf::animation;

NF_TEST(foot_placement_single_foot_ground_detection) {
    FootPlacementConfig cfg;
    cfg.foot_height_offset = 0.05f;
    cfg.interp_speed = 100.0f; // Fast convergence for test
    FootPlacementSolver solver(cfg);

    FootPlacementTarget foot;
    foot.animated_pos = Vec3{0.0f, 0.0f, 0.0f};

    // 1. Ground at y = 0.2m (elevated obstacle / step)
    GroundHit hit;
    hit.point = Vec3{0.0f, 0.2f, 0.0f};
    hit.normal = Vec3{0.0f, 1.0f, 0.0f};
    hit.has_ground = true;

    solver.evaluate_foot(foot, hit, 1.0f); // Converges in 1s

    // Target y should be 0.2m + 0.05m = 0.25m
    NF_CHECK_NEAR(foot.target_pos.y, 0.25f, 1e-3f);
    NF_CHECK_NEAR(foot.height_offset, 0.25f, 1e-3f);

    // 2. No ground -> returns to 0.0f
    hit.has_ground = false;
    solver.evaluate_foot(foot, hit, 1.0f);
    NF_CHECK_NEAR(foot.target_pos.y, 0.0f, 1e-3f);
}

NF_TEST(foot_placement_clamping_max_step) {
    FootPlacementConfig cfg;
    cfg.max_step_height = 0.4f;
    cfg.foot_height_offset = 0.0f;
    cfg.interp_speed = 100.0f;
    FootPlacementSolver solver(cfg);

    FootPlacementTarget foot;
    foot.animated_pos = Vec3{0.0f, 0.0f, 0.0f};

    // Huge cliff step of 2.0m height
    GroundHit hit;
    hit.point = Vec3{0.0f, 2.0f, 0.0f};
    hit.normal = Vec3{0.0f, 1.0f, 0.0f};
    hit.has_ground = true;

    solver.evaluate_foot(foot, hit, 1.0f);

    // Should clamp to max_step_height = 0.4f
    NF_CHECK_NEAR(foot.height_offset, 0.4f, 1e-3f);
    NF_CHECK_NEAR(foot.target_pos.y, 0.4f, 1e-3f);

    // Deep pit of -1.5m
    hit.point = Vec3{0.0f, -1.5f, 0.0f};
    solver.evaluate_foot(foot, hit, 1.0f);
    NF_CHECK_NEAR(foot.height_offset, -0.4f, 1e-3f);
}

NF_TEST(foot_placement_ankle_slope_alignment) {
    FootPlacementConfig cfg;
    cfg.max_ground_angle_deg = 30.0f;
    cfg.interp_speed = 100.0f;
    FootPlacementSolver solver(cfg);

    FootPlacementTarget foot;
    foot.animated_pos = Vec3{0.0f, 0.0f, 0.0f};

    // 1. Flat ground normal (0, 1, 0) -> Ankle rotation remains identity
    GroundHit flat_hit;
    flat_hit.point = Vec3{0.0f, 0.0f, 0.0f};
    flat_hit.normal = Vec3{0.0f, 1.0f, 0.0f};
    flat_hit.has_ground = true;

    solver.evaluate_foot(foot, flat_hit, 1.0f);
    NF_CHECK_NEAR(foot.ankle_rotation_offset.w, 1.0f, 1e-3f);
    NF_CHECK_NEAR(foot.ankle_rotation_offset.x, 0.0f, 1e-3f);

    // 2. Sloped ground at ~20 degrees (normal inclined forward in +Z)
    // normal = (0, cos(20 deg), sin(20 deg))
    const f32 rad20 = 20.0f * (3.14159265f / 180.0f);
    GroundHit slope_hit;
    slope_hit.point = Vec3{0.0f, 0.0f, 0.0f};
    slope_hit.normal = Vec3{0.0f, std::cos(rad20), std::sin(rad20)};
    slope_hit.has_ground = true;

    solver.evaluate_foot(foot, slope_hit, 1.0f);
    // Should have non-identity pitch rotation
    NF_CHECK(std::abs(foot.ankle_rotation_offset.x) > 0.05f || std::abs(foot.ankle_rotation_offset.z) > 0.05f);
    NF_CHECK(foot.ankle_rotation_offset.w < 0.999f);

    // 3. Very steep wall normal (0, 0.1, 0.9) ~ 84 degrees
    // Clamps rotation to max 30 degrees (half angle ~ 15 deg -> w ~ cos(15 deg) ~ 0.9659)
    GroundHit steep_hit;
    steep_hit.point = Vec3{0.0f, 0.0f, 0.0f};
    steep_hit.normal = Vec3{0.0f, 0.1f, 0.9f};
    steep_hit.has_ground = true;

    solver.evaluate_foot(foot, steep_hit, 1.0f);
    const f32 expected_w = std::cos(15.0f * (3.14159265f / 180.0f));
    NF_CHECK_NEAR(foot.ankle_rotation_offset.w, expected_w, 0.05f);
}

NF_TEST(foot_placement_smoothing_interpolation) {
    FootPlacementConfig cfg;
    cfg.interp_speed = 5.0f; // Moderate smoothing
    cfg.foot_height_offset = 0.0f;
    FootPlacementSolver solver(cfg);

    FootPlacementTarget foot;
    foot.animated_pos = Vec3{0.0f, 0.0f, 0.0f};

    GroundHit hit;
    hit.point = Vec3{0.0f, 1.0f, 0.0f};
    hit.normal = Vec3{0.0f, 1.0f, 0.0f};
    hit.has_ground = true;

    // Small step dt = 0.05s -> alpha = 0.25
    solver.evaluate_foot(foot, hit, 0.05f);
    // 0.0 + (0.5 max_step - 0.0) * 0.25 = 0.125
    NF_CHECK(foot.height_offset > 0.05f && foot.height_offset < 0.3f);

    // Another step
    f32 prev_offset = foot.height_offset;
    solver.evaluate_foot(foot, hit, 0.05f);
    NF_CHECK(foot.height_offset > prev_offset);
}

NF_TEST(foot_placement_biped_pelvis_drop_and_leg_ik) {
    FootPlacementConfig cfg;
    cfg.enable_pelvis_drop = true;
    cfg.pelvis_drop_scale = 1.0f;
    cfg.interp_speed = 100.0f; // Fast convergence
    cfg.foot_height_offset = 0.0f;
    FootPlacementSolver solver(cfg);

    // Left leg and right leg chains
    // Hip at y = 1.0, Knee at y = 0.5 (bent forward in +Z = 0.2), Ankle at y = 0.0
    LegChain left_leg;
    left_leg.hip_pos = Vec3{-0.2f, 1.0f, 0.0f};
    left_leg.knee_pos = Vec3{-0.2f, 0.5f, 0.2f};
    left_leg.ankle_pos = Vec3{-0.2f, 0.0f, 0.0f};
    left_leg.pole_hint = Vec3{0.0f, 0.0f, 1.0f};

    LegChain right_leg;
    right_leg.hip_pos = Vec3{0.2f, 1.0f, 0.0f};
    right_leg.knee_pos = Vec3{0.2f, 0.5f, 0.2f};
    right_leg.ankle_pos = Vec3{0.2f, 0.0f, 0.0f};
    right_leg.pole_hint = Vec3{0.0f, 0.0f, 1.0f};

    FootPlacementTarget left_foot;
    left_foot.animated_pos = left_leg.ankle_pos;

    FootPlacementTarget right_foot;
    right_foot.animated_pos = right_leg.ankle_pos;

    // Left foot on flat ground (y = 0.0)
    GroundHit left_hit;
    left_hit.point = Vec3{-0.2f, 0.0f, 0.0f};
    left_hit.normal = Vec3{0.0f, 1.0f, 0.0f};
    left_hit.has_ground = true;

    // Right foot stepping down into depression at y = -0.15m
    GroundHit right_hit;
    right_hit.point = Vec3{0.2f, -0.15f, 0.0f};
    right_hit.normal = Vec3{0.0f, 1.0f, 0.0f};
    right_hit.has_ground = true;

    f32 pelvis_drop = 0.0f;
    solver.solve_biped(left_leg, right_leg, left_foot, right_foot, left_hit, right_hit, 1.0f, &pelvis_drop);

    // Pelvis should drop by lowest foot offset (-0.15m)
    NF_CHECK_NEAR(pelvis_drop, -0.15f, 1e-3f);
    NF_CHECK_NEAR(left_leg.hip_pos.y, 0.85f, 1e-3f);
    NF_CHECK_NEAR(right_leg.hip_pos.y, 0.85f, 1e-3f);

    // IK results should be solved without clamping
    NF_CHECK(!left_leg.ik_result.clamped);
    NF_CHECK(!right_leg.ik_result.clamped);
    NF_CHECK_NEAR(left_leg.ik_result.new_end.y, 0.0f, 1e-3f);
    NF_CHECK_NEAR(right_leg.ik_result.new_end.y, -0.15f, 1e-3f);
}
