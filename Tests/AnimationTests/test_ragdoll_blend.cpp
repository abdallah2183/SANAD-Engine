// Tests/AnimationTests/test_ragdoll_blend.cpp — Ragdoll blending & get-up transitions (Phase 116)

#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/RagdollBlend.hpp>

#include <cmath>

using namespace nf;
using namespace nf::animation;

NF_TEST(ragdoll_initial_state) {
    RagdollBlendController controller(4);
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::Kinematic));
    NF_CHECK_NEAR(controller.global_weight(), 0.0f, 1e-4f);
    NF_CHECK_NEAR(controller.bone_weight(0), 1.0f, 1e-4f);
    NF_CHECK_NEAR(controller.bone_weight(3), 1.0f, 1e-4f);
}

NF_TEST(ragdoll_start_transition) {
    RagdollBlendController controller(2);
    controller.start_ragdoll(0.5f);
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::BlendingToRagdoll));
    NF_CHECK_NEAR(controller.global_weight(), 0.0f, 1e-4f);
}

NF_TEST(ragdoll_instant_transition) {
    RagdollBlendController controller(2);
    controller.start_ragdoll(0.0f);
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::Ragdoll));
    NF_CHECK_NEAR(controller.global_weight(), 1.0f, 1e-4f);
}

NF_TEST(ragdoll_blending_interpolation) {
    RagdollBlendController controller(2);
    controller.start_ragdoll(1.0f);

    controller.update(0.5f); // 50%
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::BlendingToRagdoll));
    // Smoothstep(0, 1, 0.5) = 0.5
    NF_CHECK_NEAR(controller.global_weight(), 0.5f, 1e-3f);

    controller.update(0.6f); // Complete
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::Ragdoll));
    NF_CHECK_NEAR(controller.global_weight(), 1.0f, 1e-4f);
}

NF_TEST(ragdoll_get_up_transition) {
    RagdollBlendController controller(2);
    controller.start_ragdoll(0.0f); // Now in Ragdoll
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::Ragdoll));

    controller.start_get_up(1.0f);
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::BlendingToAnimation));

    controller.update(0.5f);
    NF_CHECK_NEAR(controller.global_weight(), 0.5f, 1e-3f);

    controller.update(0.6f);
    NF_CHECK_EQ(static_cast<u8>(controller.state()), static_cast<u8>(RagdollState::Kinematic));
    NF_CHECK_NEAR(controller.global_weight(), 0.0f, 1e-4f);
}

NF_TEST(ragdoll_pose_evaluation_blending) {
    RagdollBlendController controller(1);

    LocalPose anim_pose;
    anim_pose.translation = Vec3{0.0f, 1.0f, 0.0f};
    anim_pose.rotation = Quat::identity();
    anim_pose.scale = Vec3{1.0f, 1.0f, 1.0f};

    LocalPose ragdoll_pose;
    ragdoll_pose.translation = Vec3{0.0f, 0.2f, 2.0f};
    ragdoll_pose.rotation = Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, 1.5707963f); // 90 deg pitch
    ragdoll_pose.scale = Vec3{1.0f, 1.0f, 1.0f};

    const LocalPose anim_arr[] = {anim_pose};
    const LocalPose rag_arr[]  = {ragdoll_pose};
    controller.set_animated_pose(anim_arr);
    controller.set_ragdoll_pose(rag_arr);

    // At Kinematic (weight 0.0)
    std::vector<LocalPose> out_pose;
    controller.evaluate(out_pose);
    NF_CHECK_NEAR(out_pose[0].translation.y, 1.0f, 1e-4f);

    // Halfway blend
    controller.start_ragdoll(1.0f);
    controller.update(0.5f);
    controller.evaluate(out_pose);

    // Translation should be midway (0.6f)
    NF_CHECK_NEAR(out_pose[0].translation.y, 0.6f, 0.05f);
    NF_CHECK_NEAR(out_pose[0].translation.z, 1.0f, 0.05f);
}

NF_TEST(ragdoll_per_bone_masking) {
    RagdollBlendController controller(2);

    LocalPose anim0, anim1;
    anim0.translation = Vec3{0.0f, 1.0f, 0.0f};
    anim1.translation = Vec3{0.0f, 1.5f, 0.0f};

    LocalPose rag0, rag1;
    rag0.translation = Vec3{10.0f, 0.0f, 0.0f};
    rag1.translation = Vec3{10.0f, 0.0f, 0.0f};

    const LocalPose anim_arr[] = {anim0, anim1};
    const LocalPose rag_arr[]  = {rag0, rag1};
    controller.set_animated_pose(anim_arr);
    controller.set_ragdoll_pose(rag_arr);

    controller.start_ragdoll(0.0f); // full ragdoll mode
    // Lock bone 0 to animation by masking it out
    controller.set_bone_weight(0, 0.0f);
    controller.set_bone_weight(1, 1.0f);

    std::vector<LocalPose> out_pose;
    controller.evaluate(out_pose);

    // Bone 0 stays animated
    NF_CHECK_NEAR(out_pose[0].translation.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(out_pose[0].translation.y, 1.0f, 1e-4f);

    // Bone 1 is full ragdoll
    NF_CHECK_NEAR(out_pose[1].translation.x, 10.0f, 1e-4f);
}

NF_TEST(ragdoll_ground_orientation_face_up) {
    // Pitch -90 deg: chest points upward (+Y)
    const Quat rot_up = Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963f);
    GroundOrientation orient = RagdollBlendController::detect_ground_orientation(rot_up);
    NF_CHECK_EQ(static_cast<u8>(orient), static_cast<u8>(GroundOrientation::FaceUp));
}

NF_TEST(ragdoll_ground_orientation_face_down) {
    // Pitch +90 deg: chest points downward (-Y)
    const Quat rot_down = Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, 1.5707963f);
    GroundOrientation orient = RagdollBlendController::detect_ground_orientation(rot_down);
    NF_CHECK_EQ(static_cast<u8>(orient), static_cast<u8>(GroundOrientation::FaceDown));
}

NF_TEST(ragdoll_root_alignment_offset) {
    const Vec3 ragdoll_hip{12.5f, 0.15f, -8.0f};
    const Vec3 anim_root{0.0f, 0.0f, 0.0f};

    Vec3 offset = RagdollBlendController::compute_root_alignment_offset(ragdoll_hip, anim_root);
    NF_CHECK_NEAR(offset.x, 12.5f, 1e-4f);
    NF_CHECK_NEAR(offset.y, 0.0f, 1e-4f); // Y pinned to ground plane
    NF_CHECK_NEAR(offset.z, -8.0f, 1e-4f);
}
