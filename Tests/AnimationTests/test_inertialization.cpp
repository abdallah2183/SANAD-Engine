#include <NF/Animation/Inertialization.hpp>
#include <NF/Test/TestFramework.hpp>
#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::animation;

NF_TEST(inertial_inactive_by_default) {
    Inertializer inert;
    NF_CHECK(!inert.is_active());
    NF_CHECK_NEAR(inert.duration(), 0.0f, 1e-6f);
    NF_CHECK_NEAR(inert.elapsed_time(), 0.0f, 1e-6f);
    NF_CHECK_NEAR(inert.progress(), 1.0f, 1e-6f);
    NF_CHECK(inert.bone_count() == 0);
}

NF_TEST(inertial_quintic_boundary_conditions) {
    const float x0 = 10.0f;
    const float v0 = 5.0f;
    const float t0 = 0.5f;

    // At t = 0: must return x0
    float val0 = Inertializer::decay_quintic(x0, v0, 0.0f, t0);
    NF_CHECK_NEAR(val0, x0, 1e-5f);

    // At t = t0: must return exactly 0.0
    float valEnd = Inertializer::decay_quintic(x0, v0, t0, t0);
    NF_CHECK_NEAR(valEnd, 0.0f, 1e-6f);

    // Beyond t0: must clamp to 0.0
    float valBeyond = Inertializer::decay_quintic(x0, v0, t0 + 0.1f, t0);
    NF_CHECK_NEAR(valBeyond, 0.0f, 1e-6f);
}

NF_TEST(inertial_quintic_velocity_continuity) {
    const float x0 = 2.0f;
    const float v0 = -4.0f;
    const float t0 = 0.2f;

    // Numerical finite difference derivative at t = 0
    const float eps = 1e-4f;
    float val0 = Inertializer::decay_quintic(x0, v0, 0.0f, t0);
    float valEps = Inertializer::decay_quintic(x0, v0, eps, t0);
    float numDeriv = (valEps - val0) / eps;

    NF_CHECK_NEAR(numDeriv, v0, 0.01f);
}

NF_TEST(inertial_transition_initialization) {
    std::vector<LocalPose> poseA(2);
    poseA[0].translation = Vec3(1, 2, 3);
    poseA[1].translation = Vec3(4, 5, 6);

    std::vector<LocalPose> poseB(2);
    poseB[0].translation = Vec3(0, 0, 0);
    poseB[1].translation = Vec3(1, 1, 1);

    std::vector<LocalPose> zeroVel(2);

    Inertializer inert;
    inert.transition(poseA, zeroVel, poseB, zeroVel, 0.25f);

    NF_CHECK(inert.is_active());
    NF_CHECK_NEAR(inert.duration(), 0.25f, 1e-6f);
    NF_CHECK(inert.bone_count() == 2);
    NF_CHECK_NEAR(inert.progress(), 0.0f, 1e-6f);
}

NF_TEST(inertial_instantaneous_evaluation_at_t0) {
    std::vector<LocalPose> poseA(1);
    poseA[0].translation = Vec3(10, 20, 30);
    poseA[0].rotation = Quat::from_axis_angle(Vec3(0, 1, 0), 0.5f);

    std::vector<LocalPose> poseB(1);
    poseB[0].translation = Vec3(0, 0, 0);
    poseB[0].rotation = Quat::identity();

    std::vector<LocalPose> zeroVel(1);

    Inertializer inert;
    inert.transition(poseA, zeroVel, poseB, zeroVel, 0.3f);

    std::vector<LocalPose> blended;
    bool active = inert.update_and_blend(0.0f, poseB, blended);

    NF_CHECK(active);
    NF_CHECK(blended.size() == 1);

    // At t = 0, blended pose must equal poseA
    NF_CHECK_NEAR(blended[0].translation.x, poseA[0].translation.x, 1e-4f);
    NF_CHECK_NEAR(blended[0].translation.y, poseA[0].translation.y, 1e-4f);
    NF_CHECK_NEAR(blended[0].translation.z, poseA[0].translation.z, 1e-4f);

    NF_CHECK_NEAR(blended[0].rotation.x, poseA[0].rotation.x, 1e-4f);
    NF_CHECK_NEAR(blended[0].rotation.y, poseA[0].rotation.y, 1e-4f);
    NF_CHECK_NEAR(blended[0].rotation.z, poseA[0].rotation.z, 1e-4f);
    NF_CHECK_NEAR(blended[0].rotation.w, poseA[0].rotation.w, 1e-4f);
}

NF_TEST(inertial_completion_at_duration) {
    std::vector<LocalPose> poseA(1);
    poseA[0].translation = Vec3(5, 5, 5);

    std::vector<LocalPose> poseB(1);
    poseB[0].translation = Vec3(1, 2, 3);

    std::vector<LocalPose> zeroVel(1);

    Inertializer inert;
    inert.transition(poseA, zeroVel, poseB, zeroVel, 0.2f);

    std::vector<LocalPose> blended;
    // Advance full duration 0.2s
    bool active = inert.update_and_blend(0.2f, poseB, blended);

    NF_CHECK(!active);
    NF_CHECK(!inert.is_active());

    // Blended pose must now equal incoming target poseB exactly
    NF_CHECK_NEAR(blended[0].translation.x, poseB[0].translation.x, 1e-5f);
    NF_CHECK_NEAR(blended[0].translation.y, poseB[0].translation.y, 1e-5f);
    NF_CHECK_NEAR(blended[0].translation.z, poseB[0].translation.z, 1e-5f);
}

NF_TEST(inertial_smooth_translation_midpoint) {
    std::vector<LocalPose> poseA(1);
    poseA[0].translation = Vec3(0, 0, 0);

    std::vector<LocalPose> poseB(1);
    poseB[0].translation = Vec3(10, 0, 0);

    std::vector<LocalPose> zeroVel(1);

    Inertializer inert;
    inert.transition(poseA, zeroVel, poseB, zeroVel, 1.0f);

    std::vector<LocalPose> blended;
    inert.update_and_blend(0.5f, poseB, blended);

    // At t = 0.5 with t0 = 1.0 and zero velocities:
    // u = 0.5, u^3 = 0.125
    // invT0 = 1
    // a = -10, b = -30, c = -60
    // poly = -10 + -30*(0.5) + -60*(0.25) = -10 - 15 - 15 = -40
    // offset = -40 * 0.125 = -5.0
    // blended = target (10) + offset (-5) = 5.0
    NF_CHECK_NEAR(blended[0].translation.x, 5.0f, 1e-4f);
}

NF_TEST(inertial_rotation_decay) {
    std::vector<LocalPose> poseA(1);
    poseA[0].rotation = Quat::from_axis_angle(Vec3(0, 1, 0), 1.0f);

    std::vector<LocalPose> poseB(1);
    poseB[0].rotation = Quat::identity();

    std::vector<LocalPose> zeroVel(1);

    Inertializer inert;
    inert.transition(poseA, zeroVel, poseB, zeroVel, 0.4f);

    std::vector<LocalPose> blended;
    inert.update_and_blend(0.2f, poseB, blended);

    // Intermediate rotation should be between 0 and 1.0 radians along Y
    float dot = blended[0].rotation.dot(Quat::identity());
    NF_CHECK(dot > 0.0f && dot < 1.0f);
}

NF_TEST(inertial_transition_from_previous_velocity_estimate) {
    std::vector<LocalPose> prev(1);
    prev[0].translation = Vec3(0, 0, 0);

    std::vector<LocalPose> curr(1);
    curr[0].translation = Vec3(1, 0, 0);

    std::vector<LocalPose> targ(1);
    targ[0].translation = Vec3(10, 0, 0);

    Inertializer inert;
    inert.transition_from_previous(curr, prev, targ, 0.016f, 0.2f);

    NF_CHECK(inert.is_active());
    NF_CHECK(inert.bone_count() == 1);

    std::vector<LocalPose> blended;
    bool active = inert.update_and_blend(0.001f, targ, blended);
    NF_CHECK(active);
    NF_CHECK(blended[0].translation.x > 0.9f && blended[0].translation.x < 1.5f);
}

NF_TEST(inertial_reset_clears_active) {
    std::vector<LocalPose> poseA(1);
    std::vector<LocalPose> poseB(1);
    std::vector<LocalPose> zero(1);

    Inertializer inert;
    inert.transition(poseA, zero, poseB, zero, 0.5f);
    NF_CHECK(inert.is_active());

    inert.reset();
    NF_CHECK(!inert.is_active());
    NF_CHECK(inert.bone_count() == 0);
    NF_CHECK_NEAR(inert.duration(), 0.0f, 1e-6f);
}
