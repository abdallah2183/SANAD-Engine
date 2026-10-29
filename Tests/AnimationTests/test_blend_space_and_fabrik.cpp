#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/BlendSpace.hpp>
#include <NF/Animation/FABRIK.hpp>
#include <NF/Animation/BoneMask.hpp>
#include <cmath>

using namespace nf;
using namespace nf::animation;

namespace {

Skeleton make_test_humanoid_skeleton() {
    Skeleton skel;
    // 0: Root
    skel.bones.push_back({"Root", -1, Vec3(0, 0, 0)});
    // 1: Pelvis
    skel.bones.push_back({"Pelvis", 0, Vec3(0, 1, 0)});
    // 2: Spine
    skel.bones.push_back({"Spine", 1, Vec3(0, 1.5f, 0)});
    // 3: Head
    skel.bones.push_back({"Head", 2, Vec3(0, 2.0f, 0)});
    // 4: Arm_L
    skel.bones.push_back({"Arm_L", 2, Vec3(-0.5f, 1.5f, 0)});
    // 5: Leg_L
    skel.bones.push_back({"Leg_L", 1, Vec3(-0.3f, 0.5f, 0)});
    return skel;
}

AnimationClip make_constant_clip(const std::string& name, const Skeleton& skel, const Vec3& offset, f32 duration) {
    AnimationClip clip;
    clip.name = name;
    clip.duration = duration;
    clip.looping = true;

    for (size_t i = 0; i < skel.bone_count(); ++i) {
        AnimationTrack track;
        track.bone_index = static_cast<int32_t>(i);
        Keyframe kf0;
        kf0.time = 0.0f;
        kf0.translation = skel.bones[i].rest_translation + offset;
        kf0.rotation = Quat::identity();
        kf0.scale = Vec3(1, 1, 1);

        Keyframe kf1 = kf0;
        kf1.time = duration;

        track.keyframes.push_back(kf0);
        track.keyframes.push_back(kf1);
        clip.tracks.push_back(track);
    }
    return clip;
}

} // namespace

NF_TEST(blend_space_1d_boundary_and_interpolation) {
    auto skel = make_test_humanoid_skeleton();

    AnimationClip idle = make_constant_clip("Idle", skel, Vec3(0, 0, 0), 2.0f);
    AnimationClip walk = make_constant_clip("Walk", skel, Vec3(0, 0, 1.0f), 1.0f);
    AnimationClip run  = make_constant_clip("Run",  skel, Vec3(0, 0, 2.0f), 0.6f);

    std::unordered_map<std::string, const AnimationClip*> clip_map;
    clip_map["Idle"] = &idle;
    clip_map["Walk"] = &walk;
    clip_map["Run"]  = &run;

    BlendSpace1D bs;
    bs.add_sample("Idle", 0.0f);
    bs.add_sample("Walk", 2.0f);
    bs.add_sample("Run", 5.0f);

    NF_CHECK_EQ(bs.sample_count(), 3u);

    // Test exact sample 0.0
    std::vector<std::pair<size_t, f32>> weights;
    bs.get_blend_weights(0.0f, weights);
    NF_CHECK_EQ(weights.size(), 1u);
    NF_CHECK_EQ(weights[0].first, 0u);
    NF_CHECK_NEAR(weights[0].second, 1.0f, 0.001f);

    // Test midpoint at 1.0 (50% Idle, 50% Walk)
    bs.get_blend_weights(1.0f, weights);
    NF_CHECK_EQ(weights.size(), 2u);
    NF_CHECK_NEAR(weights[0].second, 0.5f, 0.001f);
    NF_CHECK_NEAR(weights[1].second, 0.5f, 0.001f);

    f32 synced_dur = bs.get_synced_duration(1.0f, clip_map);
    NF_CHECK_NEAR(synced_dur, 1.5f, 0.001f); // (2.0 + 1.0) / 2

    // Evaluate pose at 1.0
    std::vector<LocalPose> pose;
    bs.evaluate(1.0f, 0.5f, skel, clip_map, pose);
    NF_CHECK_EQ(pose.size(), skel.bone_count());
    // Translation Z offset should be midpoint of 0 and 1.0 -> 0.5
    NF_CHECK_NEAR(pose[0].translation.z, 0.5f, 0.001f);
}

NF_TEST(blend_space_2d_directional_interpolation) {
    auto skel = make_test_humanoid_skeleton();

    AnimationClip idle    = make_constant_clip("Idle", skel, Vec3(0, 0, 0), 1.0f);
    AnimationClip forward = make_constant_clip("Fwd",  skel, Vec3(0, 0, 1), 1.0f);
    AnimationClip right   = make_constant_clip("Right", skel, Vec3(1, 0, 0), 1.0f);

    std::unordered_map<std::string, const AnimationClip*> clip_map;
    clip_map["Idle"]  = &idle;
    clip_map["Fwd"]   = &forward;
    clip_map["Right"] = &right;

    BlendSpace2D bs;
    bs.add_sample("Idle",  Vec2(0.0f, 0.0f));
    bs.add_sample("Fwd",   Vec2(0.0f, 1.0f));
    bs.add_sample("Right", Vec2(1.0f, 0.0f));

    // Exact match
    std::vector<std::pair<size_t, f32>> weights;
    bs.get_blend_weights(Vec2(0.0f, 0.0f), weights);
    NF_CHECK_EQ(weights.size(), 1u);
    NF_CHECK_NEAR(weights[0].second, 1.0f, 0.001f);

    // Diagonal match
    bs.get_blend_weights(Vec2(0.5f, 0.5f), weights);
    NF_CHECK(!weights.empty());

    std::vector<LocalPose> pose;
    bs.evaluate(Vec2(0.0f, 1.0f), 0.0f, skel, clip_map, pose);
    NF_CHECK_EQ(pose.size(), skel.bone_count());
    NF_CHECK_NEAR(pose[0].translation.z, 1.0f, 0.001f);
}

NF_TEST(fabrik_chain_multi_joint_convergence) {
    // 4-joint chain: (0,0,0) -> (1,0,0) -> (2,0,0) -> (3,0,0)
    std::vector<Vec3> joints = {
        Vec3(0, 0, 0),
        Vec3(1, 0, 0),
        Vec3(2, 0, 0),
        Vec3(3, 0, 0)
    };

    Vec3 target(2.0f, 1.5f, 0.0f);
    auto res = solve_fabrik(joints, target, 0.001f, 30);

    NF_CHECK(res.reached);
    NF_CHECK(res.final_error <= 0.002f);
    NF_CHECK_EQ(res.solved_positions.size(), 4u);

    // Base joint stays fixed at origin
    NF_CHECK_NEAR(res.solved_positions[0].x, 0.0f, 0.001f);
    NF_CHECK_NEAR(res.solved_positions[0].y, 0.0f, 0.001f);
    NF_CHECK_NEAR(res.solved_positions[0].z, 0.0f, 0.001f);

    // End effector reaches target
    Vec3 end_diff = res.solved_positions[3] - target;
    NF_CHECK(end_diff.length() <= 0.002f);

    // Verify bone lengths are strictly preserved
    for (size_t i = 0; i < 3; ++i) {
        f32 len = (res.solved_positions[i + 1] - res.solved_positions[i]).length();
        NF_CHECK(std::abs(len - 1.0f) < 0.001f);
    }
}

NF_TEST(fabrik_unreachable_target_stretches) {
    std::vector<Vec3> joints = {
        Vec3(0, 0, 0),
        Vec3(1, 0, 0),
        Vec3(2, 0, 0)
    }; // Total length = 2.0

    Vec3 target(10.0f, 0.0f, 0.0f); // Beyond reach of 2.0
    auto res = solve_fabrik(joints, target, 0.001f, 10);

    NF_CHECK(!res.reached);
    NF_CHECK_NEAR(res.final_error, 8.0f, 0.001f);

    // End effector should be stretched to max reach at (2, 0, 0)
    NF_CHECK_NEAR(res.solved_positions[2].x, 2.0f, 0.001f);
    NF_CHECK_NEAR(res.solved_positions[2].y, 0.0f, 0.001f);
    NF_CHECK_NEAR(res.solved_positions[2].z, 0.0f, 0.001f);
}

NF_TEST(look_at_ik_aiming_and_clamp) {
    Vec3 head_pos(0, 1.7f, 0);
    Quat head_rot = Quat::identity(); // Facing +Z
    Vec3 target(5.0f, 1.7f, 5.0f);   // 45 degrees to the right

    auto res = solve_look_at(head_pos, head_rot, target, Vec3(0, 0, 1), Vec3(0, 1, 0), 80.0f, 60.0f, 1.0f);
    NF_CHECK(!res.clamped);
    NF_CHECK(res.delta_rotation.w < 0.999f || std::abs(res.delta_rotation.y) > 0.01f);

    // Target far behind: 160 degrees yaw
    Vec3 back_target(-5.0f, 1.7f, -10.0f);
    auto res_clamped = solve_look_at(head_pos, head_rot, back_target, Vec3(0, 0, 1), Vec3(0, 1, 0), 45.0f, 45.0f, 1.0f);
    NF_CHECK(res_clamped.clamped);
}

NF_TEST(bone_mask_and_layered_blend) {
    auto skel = make_test_humanoid_skeleton();

    BoneMask mask(skel.bone_count(), 0.0f);
    // Recursively set spine and all its descendants to weight 1.0
    int32_t spine_idx = skel.find_bone("Spine");
    NF_CHECK(spine_idx >= 0);
    mask.set_weight_recursive(skel, spine_idx, 1.0f);

    // Spine, Head, Arm_L should be 1.0
    NF_CHECK_NEAR(mask.get_weight(skel.find_bone("Spine")), 1.0f, 0.001f);
    NF_CHECK_NEAR(mask.get_weight(skel.find_bone("Head")), 1.0f, 0.001f);
    NF_CHECK_NEAR(mask.get_weight(skel.find_bone("Arm_L")), 1.0f, 0.001f);

    // Root, Pelvis, Leg_L should remain 0.0
    NF_CHECK_NEAR(mask.get_weight(skel.find_bone("Root")), 0.0f, 0.001f);
    NF_CHECK_NEAR(mask.get_weight(skel.find_bone("Pelvis")), 0.0f, 0.001f);
    NF_CHECK_NEAR(mask.get_weight(skel.find_bone("Leg_L")), 0.0f, 0.001f);

    // Create base pose (running) and overlay pose (aiming)
    std::vector<LocalPose> base(skel.bone_count());
    std::vector<LocalPose> overlay(skel.bone_count());

    for (size_t i = 0; i < skel.bone_count(); ++i) {
        base[i].translation = skel.bones[i].rest_translation + Vec3(0, 0, 1.0f); // Leg / body run
        overlay[i].translation = skel.bones[i].rest_translation + Vec3(0, 5.0f, 0); // Upper body aim
    }

    std::vector<LocalPose> blended;
    blend_layered(base, overlay, mask, blended);

    // Leg_L has mask 0 -> keeps base translation
    int32_t leg_idx = skel.find_bone("Leg_L");
    NF_CHECK_NEAR(blended[leg_idx].translation.z, base[leg_idx].translation.z, 0.001f);

    // Arm_L has mask 1 -> takes overlay translation
    int32_t arm_idx = skel.find_bone("Arm_L");
    NF_CHECK_NEAR(blended[arm_idx].translation.y, overlay[arm_idx].translation.y, 0.001f);
}
