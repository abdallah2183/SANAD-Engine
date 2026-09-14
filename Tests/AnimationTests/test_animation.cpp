// AnimationTests — skeletal animation: skeleton, clips, sampling, blending, player, state machine.
//
// All tests are pure math — no GPU, no audio hardware, no window. They verify
// that the animation data structures produce the correct transforms for known
// inputs, which is the property the runtime and editor depend on.

#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <NF/Animation/AnimationClip.hpp>
#include <NF/Animation/AnimationPlayer.hpp>
#include <NF/Animation/AnimationStateMachine.hpp>

#include <cmath>

using namespace nf;
using namespace nf::animation;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Two-bone skeleton: root at origin, child offset (1,0,0) from root.
static Skeleton make_two_bone_skeleton() {
    Skeleton skel;
    skel.bones.resize(2);
    skel.bones[0].name = "root";
    skel.bones[0].parent = -1;
    skel.bones[0].rest_translation = {0, 0, 0};
    skel.bones[0].rest_rotation = Quat::identity();
    skel.bones[0].rest_scale = {1, 1, 1};

    skel.bones[1].name = "child";
    skel.bones[1].parent = 0;
    skel.bones[1].rest_translation = {1, 0, 0};
    skel.bones[1].rest_rotation = Quat::identity();
    skel.bones[1].rest_scale = {1, 1, 1};

    return skel;
}

/// Three-bone skeleton: root -> spine -> head, like a simple chain.
static Skeleton make_three_bone_chain() {
    Skeleton skel;
    skel.bones.resize(3);
    for (int i = 0; i < 3; ++i) {
        skel.bones[i].parent = (i == 0) ? -1 : i - 1;
        skel.bones[i].rest_translation = (i == 0) ? Vec3{0,0,0} : Vec3{0, 1, 0};
        skel.bones[i].rest_rotation = Quat::identity();
        skel.bones[i].rest_scale = {1, 1, 1};
    }
    skel.bones[0].name = "root";
    skel.bones[1].name = "spine";
    skel.bones[2].name = "head";
    return skel;
}

// ---------------------------------------------------------------------------
// W1: Skeleton, clips, poses
// ---------------------------------------------------------------------------

NF_TEST(skeleton_find_bone_by_name) {
    auto skel = make_two_bone_skeleton();
    NF_CHECK_EQ(skel.find_bone("root"), 0);
    NF_CHECK_EQ(skel.find_bone("child"), 1);
    NF_CHECK_EQ(skel.find_bone("nonexistent"), -1);
    NF_CHECK_EQ(skel.bone_count(), 2u);
}

NF_TEST(skeleton_world_transforms_rest_pose) {
    auto skel = make_two_bone_skeleton();
    std::vector<LocalPose> local(2);
    // Rest pose.
    local[0].translation = {0, 0, 0};
    local[0].rotation = Quat::identity();
    local[0].scale = {1, 1, 1};
    local[1].translation = {1, 0, 0};
    local[1].rotation = Quat::identity();
    local[1].scale = {1, 1, 1};

    std::vector<WorldPose> world;
    compute_world_transforms(skel, local, world);

    // Root world transform = identity.
    NF_CHECK_NEAR(world[0].transform.m[3][0], 0.0f, 1e-5f);
    NF_CHECK_NEAR(world[0].transform.m[3][1], 0.0f, 1e-5f);
    NF_CHECK_NEAR(world[0].transform.m[3][2], 0.0f, 1e-5f);

    // Child world transform translation = (1,0,0) because parent is at origin.
    NF_CHECK_NEAR(world[1].transform.m[3][0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(world[1].transform.m[3][1], 0.0f, 1e-5f);
    NF_CHECK_NEAR(world[1].transform.m[3][2], 0.0f, 1e-5f);
}

NF_TEST(skeleton_world_transforms_translated_root) {
    auto skel = make_two_bone_skeleton();
    std::vector<LocalPose> local(2);
    local[0].translation = {5, 0, 0}; // root moved to (5,0,0)
    local[0].rotation = Quat::identity();
    local[0].scale = {1, 1, 1};
    local[1].translation = {1, 0, 0}; // child offset (1,0,0) from root
    local[1].rotation = Quat::identity();
    local[1].scale = {1, 1, 1};

    std::vector<WorldPose> world;
    compute_world_transforms(skel, local, world);

    // Root world = (5,0,0).
    NF_CHECK_NEAR(world[0].transform.m[3][0], 5.0f, 1e-5f);
    // Child world = (5+1, 0, 0) = (6, 0, 0).
    NF_CHECK_NEAR(world[1].transform.m[3][0], 6.0f, 1e-5f);
}

NF_TEST(skeleton_world_transforms_rotated_parent) {
    auto skel = make_two_bone_skeleton();
    std::vector<LocalPose> local(2);
    // Root rotated 90 degrees around Z.
    local[0].translation = {0, 0, 0};
    local[0].rotation = Quat::from_axis_angle({0, 0, 1}, 1.5707963f);
    local[0].scale = {1, 1, 1};
    // Child offset (1,0,0) in root's local space.
    local[1].translation = {1, 0, 0};
    local[1].rotation = Quat::identity();
    local[1].scale = {1, 1, 1};

    std::vector<WorldPose> world;
    compute_world_transforms(skel, local, world);

    // Child world should be at (0, 1, 0) because root rotation mapped (1,0,0) -> (0,1,0).
    NF_CHECK_NEAR(world[1].transform.m[3][0], 0.0f, 1e-4f);
    NF_CHECK_NEAR(world[1].transform.m[3][1], 1.0f, 1e-4f);
}

NF_TEST(skeleton_three_bone_chain_world) {
    auto skel = make_three_bone_chain();
    std::vector<LocalPose> local(3);
    for (int i = 0; i < 3; ++i) {
        local[i].translation = {0, 1, 0};
        local[i].rotation = Quat::identity();
        local[i].scale = {1, 1, 1};
    }

    std::vector<WorldPose> world;
    compute_world_transforms(skel, local, world);

    // root at (0,1,0), spine at (0,2,0), head at (0,3,0).
    NF_CHECK_NEAR(world[0].transform.m[3][1], 1.0f, 1e-5f);
    NF_CHECK_NEAR(world[1].transform.m[3][1], 2.0f, 1e-5f);
    NF_CHECK_NEAR(world[2].transform.m[3][1], 3.0f, 1e-5f);
}

NF_TEST(clip_sample_at_zero_returns_first_keyframe) {
    auto skel = make_two_bone_skeleton();

    AnimationClip clip;
    clip.name = "test";
    clip.duration = 1.0f;
    clip.looping = true;

    AnimationTrack track;
    track.bone_index = 0;
    track.keyframes.resize(2);
    track.keyframes[0].time = 0.0f;
    track.keyframes[0].translation = {0, 0, 0};
    track.keyframes[0].rotation = Quat::identity();
    track.keyframes[0].scale = {1, 1, 1};
    track.keyframes[1].time = 1.0f;
    track.keyframes[1].translation = {2, 0, 0};
    track.keyframes[1].rotation = Quat::identity();
    track.keyframes[1].scale = {1, 1, 1};
    clip.tracks.push_back(track);

    std::vector<LocalPose> local;
    clip.sample(0.0f, skel, local);

    NF_CHECK_NEAR(local[0].translation.x, 0.0f, 1e-5f);
}

NF_TEST(clip_sample_at_end_returns_last_keyframe) {
    auto skel = make_two_bone_skeleton();

    AnimationClip clip;
    clip.name = "test";
    clip.duration = 1.0f;
    clip.looping = false; // No wrapping: t=duration should return the last keyframe.

    AnimationTrack track;
    track.bone_index = 0;
    track.keyframes.resize(2);
    track.keyframes[0].time = 0.0f;
    track.keyframes[0].translation = {0, 0, 0};
    track.keyframes[1].time = 1.0f;
    track.keyframes[1].translation = {2, 0, 0};
    clip.tracks.push_back(track);

    std::vector<LocalPose> local;
    clip.sample(1.0f, skel, local);

    NF_CHECK_NEAR(local[0].translation.x, 2.0f, 1e-5f);
}

NF_TEST(clip_sample_midpoint_interpolates) {
    auto skel = make_two_bone_skeleton();

    AnimationClip clip;
    clip.name = "test";
    clip.duration = 1.0f;
    clip.looping = true;

    AnimationTrack track;
    track.bone_index = 0;
    track.keyframes.resize(2);
    track.keyframes[0].time = 0.0f;
    track.keyframes[0].translation = {0, 0, 0};
    track.keyframes[1].time = 1.0f;
    track.keyframes[1].translation = {4, 0, 0};
    clip.tracks.push_back(track);

    std::vector<LocalPose> local;
    clip.sample(0.5f, skel, local);

    // At t=0.5, translation should be (2, 0, 0).
    NF_CHECK_NEAR(local[0].translation.x, 2.0f, 1e-5f);
}

NF_TEST(clip_sample_quaternion_slerp) {
    auto skel = make_two_bone_skeleton();

    AnimationClip clip;
    clip.name = "test";
    clip.duration = 1.0f;
    clip.looping = true;

    AnimationTrack track;
    track.bone_index = 0;
    track.keyframes.resize(2);
    track.keyframes[0].time = 0.0f;
    track.keyframes[0].rotation = Quat::identity();
    // 90 degree rotation around Y.
    track.keyframes[1].time = 1.0f;
    track.keyframes[1].rotation = Quat::from_axis_angle({0, 1, 0}, 1.5707963f);
    clip.tracks.push_back(track);

    std::vector<LocalPose> local;
    clip.sample(0.5f, skel, local);

    // At t=0.5, rotation should be 45 degrees around Y.
    // The quaternion for 45 degrees around Y: (0, sin(22.5), 0, cos(22.5)).
    f32 half_angle = 0.7853982f / 2.0f; // 45/2 degrees in radians
    f32 expected_w = std::cos(half_angle);
    f32 expected_y = std::sin(half_angle);
    NF_CHECK_NEAR(local[0].rotation.w, expected_w, 1e-3f);
    NF_CHECK_NEAR(local[0].rotation.y, expected_y, 1e-3f);
}

NF_TEST(clip_sample_no_track_uses_rest_pose) {
    auto skel = make_two_bone_skeleton();

    AnimationClip clip;
    clip.name = "test";
    clip.duration = 1.0f;
    clip.looping = true;
    // No tracks — all bones should get rest pose.

    std::vector<LocalPose> local;
    clip.sample(0.5f, skel, local);

    NF_CHECK_NEAR(local[1].translation.x, 1.0f, 1e-5f); // child rest translation
}

NF_TEST(clip_sample_loop_wraps) {
    auto skel = make_two_bone_skeleton();

    AnimationClip clip;
    clip.name = "test";
    clip.duration = 1.0f;
    clip.looping = true;

    AnimationTrack track;
    track.bone_index = 0;
    track.keyframes.resize(2);
    track.keyframes[0].time = 0.0f;
    track.keyframes[0].translation = {0, 0, 0};
    track.keyframes[1].time = 1.0f;
    track.keyframes[1].translation = {4, 0, 0};
    clip.tracks.push_back(track);

    // Sample at t=1.5 with looping — should wrap to t=0.5.
    std::vector<LocalPose> local;
    clip.sample(1.5f, skel, local);
    NF_CHECK_NEAR(local[0].translation.x, 2.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// W2: Blending
// ---------------------------------------------------------------------------

NF_TEST(blend_two_poses_equal_weights) {
    auto skel = make_two_bone_skeleton();
    std::vector<LocalPose> a(2), b(2), out;
    a[0].translation = {0, 0, 0};
    b[0].translation = {4, 0, 0};

    std::vector<const std::vector<LocalPose>*> poses = {&a, &b};
    std::vector<f32> weights = {0.5f, 0.5f};
    blend_poses(poses, weights, out);

    NF_CHECK_NEAR(out[0].translation.x, 2.0f, 1e-5f);
}

NF_TEST(blend_two_poses_unequal_weights) {
    std::vector<LocalPose> a(1), b(1), out;
    a[0].translation = {0, 0, 0};
    b[0].translation = {10, 0, 0};

    std::vector<const std::vector<LocalPose>*> poses = {&a, &b};
    std::vector<f32> weights = {0.75f, 0.25f};
    blend_poses(poses, weights, out);

    NF_CHECK_NEAR(out[0].translation.x, 2.5f, 1e-5f);
}

NF_TEST(blend_three_poses) {
    std::vector<LocalPose> a(1), b(1), c(1), out;
    a[0].translation = {0, 0, 0};
    b[0].translation = {3, 0, 0};
    c[0].translation = {6, 0, 0};

    std::vector<const std::vector<LocalPose>*> poses = {&a, &b, &c};
    std::vector<f32> weights = {1.0f, 1.0f, 1.0f}; // equal -> average
    blend_poses(poses, weights, out);

    NF_CHECK_NEAR(out[0].translation.x, 3.0f, 1e-5f);
}

NF_TEST(blend_zero_weights_returns_first_pose) {
    std::vector<LocalPose> a(1), b(1), out;
    a[0].translation = {5, 0, 0};
    b[0].translation = {10, 0, 0};

    std::vector<const std::vector<LocalPose>*> poses = {&a, &b};
    std::vector<f32> weights = {0.0f, 0.0f};
    blend_poses(poses, weights, out);

    NF_CHECK_NEAR(out[0].translation.x, 5.0f, 1e-5f);
}

NF_TEST(blend_additive_basic) {
    std::vector<LocalPose> base(1), additive(1), additive_base(1), out;
    base[0].translation = {1, 0, 0};
    additive[0].translation = {1, 2, 3};
    additive_base[0].translation = {0, 0, 0};

    blend_additive(base, additive, additive_base, 1.0f, out);

    // base + (additive - additive_base) * 1.0 = (1,0,0) + (1,2,3) = (2,2,3)
    NF_CHECK_NEAR(out[0].translation.x, 2.0f, 1e-5f);
    NF_CHECK_NEAR(out[0].translation.y, 2.0f, 1e-5f);
    NF_CHECK_NEAR(out[0].translation.z, 3.0f, 1e-5f);
}

NF_TEST(blend_additive_half_weight) {
    std::vector<LocalPose> base(1), additive(1), additive_base(1), out;
    base[0].translation = {1, 0, 0};
    additive[0].translation = {1, 2, 3};
    additive_base[0].translation = {0, 0, 0};

    blend_additive(base, additive, additive_base, 0.5f, out);

    // base + (additive - additive_base) * 0.5 = (1,0,0) + (0.5,1,1.5) = (1.5,1,1.5)
    NF_CHECK_NEAR(out[0].translation.x, 1.5f, 1e-5f);
    NF_CHECK_NEAR(out[0].translation.y, 1.0f, 1e-5f);
    NF_CHECK_NEAR(out[0].translation.z, 1.5f, 1e-5f);
}

// ---------------------------------------------------------------------------
// W2: Player
// ---------------------------------------------------------------------------

NF_TEST(player_play_advance_time) {
    AnimationPlayer player;
    player.set_clip("test");
    player.play();

    f32 t = player.update(0.5f, 2.0f);
    NF_CHECK_NEAR(t, 0.5f, 1e-5f);
    NF_CHECK(player.is_playing());
}

NF_TEST(player_loop_wraps) {
    AnimationPlayer player;
    player.set_clip("test");
    player.play();
    player.set_loop_mode(LoopMode::Loop);

    // Advance past the end.
    player.update(0.5f, 1.0f);  // t = 0.5
    f32 t = player.update(0.75f, 1.0f); // t = 1.25 -> wraps to 0.25
    NF_CHECK_NEAR(t, 0.25f, 1e-5f);
    NF_CHECK(player.is_playing());
}

NF_TEST(player_no_loop_stops_at_end) {
    AnimationPlayer player;
    player.set_clip("test");
    player.play();
    player.set_loop_mode(LoopMode::None);

    player.update(0.5f, 1.0f);  // t = 0.5
    f32 t = player.update(0.6f, 1.0f); // t = 1.1 -> clamped to 1.0, stopped
    NF_CHECK_NEAR(t, 1.0f, 1e-5f);
    NF_CHECK(!player.is_playing());
}

NF_TEST(player_speed_scales_time) {
    AnimationPlayer player;
    player.set_clip("test");
    player.play();
    player.set_speed(2.0f);

    f32 t = player.update(0.5f, 2.0f); // dt*speed = 1.0
    NF_CHECK_NEAR(t, 1.0f, 1e-5f);
}

NF_TEST(player_pause_and_resume) {
    AnimationPlayer player;
    player.set_clip("test");
    player.play();

    player.update(0.3f, 2.0f);
    NF_CHECK(player.is_playing());

    player.pause();
    NF_CHECK(!player.is_playing());

    // Update while paused should not advance time.
    f32 t = player.update(0.5f, 2.0f);
    NF_CHECK_NEAR(t, 0.3f, 1e-5f);
}

NF_TEST(player_stop_resets_time) {
    AnimationPlayer player;
    player.set_clip("test");
    player.play();

    player.update(0.5f, 2.0f);
    NF_CHECK_NEAR(player.time(), 0.5f, 1e-5f);

    player.stop();
    NF_CHECK_NEAR(player.time(), 0.0f, 1e-5f);
    NF_CHECK(!player.is_playing());
}

NF_TEST(player_ping_pong_reverses) {
    AnimationPlayer player;
    player.set_clip("test");
    player.play();
    player.set_loop_mode(LoopMode::PingPong);

    player.update(0.5f, 1.0f);  // t = 0.5
    player.update(0.6f, 1.0f);  // t = 1.1 -> reverses to 0.9
    NF_CHECK_NEAR(player.time(), 0.9f, 1e-5f);
    NF_CHECK(player.is_playing());

    // Now going backward.
    f32 t = player.update(0.2f, 1.0f); // t = 0.7
    NF_CHECK_NEAR(t, 0.7f, 1e-5f);
}

// ---------------------------------------------------------------------------
// W3: State machine
// ---------------------------------------------------------------------------

NF_TEST(state_machine_default_state_is_first) {
    AnimationStateMachine sm;
    AnimState s0{"idle", "idle_clip", 1.0f, {}};
    AnimState s1{"walk", "walk_clip", 1.0f, {}};
    sm.add_state(s0);
    sm.add_state(s1);

    NF_CHECK_EQ(sm.current_state(), std::string("idle"));
}

NF_TEST(state_machine_transition_fires) {
    AnimationStateMachine sm;

    AnimState idle{"idle", "idle_clip", 1.0f, {}};
    idle.transitions.push_back({.target_state = "walk", .fade_duration = 0.2f, .conditions = {
        {"speed", ConditionOp::Greater, 0.5f}
    }});
    AnimState walk{"walk", "walk_clip", 1.0f, {}};
    sm.add_state(idle);
    sm.add_state(walk);

    sm.set_param("speed", 0.0f);
    Skeleton skel;
    std::unordered_map<std::string, const AnimationClip*> clips;
    std::vector<LocalPose> out;
    sm.update(0.0f, skel, clips, out);
    NF_CHECK_EQ(sm.current_state(), std::string("idle"));

    // Set speed > 0.5 -> transition should fire.
    sm.set_param("speed", 1.0f);
    sm.update(0.1f, skel, clips, out);
    NF_CHECK_EQ(sm.current_state(), std::string("walk"));
}

NF_TEST(state_machine_no_transition_when_condition_not_met) {
    AnimationStateMachine sm;

    AnimState idle{"idle", "idle_clip", 1.0f, {}};
    idle.transitions.push_back({.target_state = "walk", .fade_duration = 0.2f, .conditions = {
        {"speed", ConditionOp::Greater, 0.5f}
    }});
    AnimState walk{"walk", "walk_clip", 1.0f, {}};
    sm.add_state(idle);
    sm.add_state(walk);

    sm.set_param("speed", 0.3f); // < 0.5, no transition
    Skeleton skel;
    std::unordered_map<std::string, const AnimationClip*> clips;
    std::vector<LocalPose> out;
    sm.update(0.1f, skel, clips, out);

    NF_CHECK_EQ(sm.current_state(), std::string("idle"));
}

NF_TEST(state_machine_cross_fade_blends) {
    AnimationStateMachine sm;

    // Two clips: idle at (0,0,0), walk at (2,0,0).
    auto skel = make_two_bone_skeleton();

    AnimationClip idle_clip;
    idle_clip.name = "idle";
    idle_clip.duration = 1.0f;
    idle_clip.looping = true;
    {
        AnimationTrack t;
        t.bone_index = 0;
        t.keyframes.resize(1);
        t.keyframes[0].time = 0.0f;
        t.keyframes[0].translation = {0, 0, 0};
        idle_clip.tracks.push_back(t);
    }

    AnimationClip walk_clip;
    walk_clip.name = "walk";
    walk_clip.duration = 1.0f;
    walk_clip.looping = true;
    {
        AnimationTrack t;
        t.bone_index = 0;
        t.keyframes.resize(1);
        t.keyframes[0].time = 0.0f;
        t.keyframes[0].translation = {2, 0, 0};
        walk_clip.tracks.push_back(t);
    }

    std::unordered_map<std::string, const AnimationClip*> clips;
    clips["idle_clip"] = &idle_clip;
    clips["walk_clip"] = &walk_clip;

    AnimState idle{"idle", "idle_clip", 1.0f, {}};
    idle.transitions.push_back({.target_state = "walk", .fade_duration = 0.5f, .conditions = {
        {"speed", ConditionOp::Greater, 0.5f}
    }});
    AnimState walk{"walk", "walk_clip", 1.0f, {}};
    sm.add_state(idle);
    sm.add_state(walk);

    sm.set_param("speed", 1.0f);

    std::vector<LocalPose> out;
    // First update: transition fires, cross-fade starts at fade_t=0 (100% previous).
    sm.update(0.1f, skel, clips, out);
    // After transition, current state is "walk".
    NF_CHECK_EQ(sm.current_state(), std::string("walk"));

    // Second update: fade_remaining = 0.5 - 0.1 = 0.4, fade_t = 0.2.
    // Blended: idle * 0.8 + walk * 0.2 = (0*0.8 + 2*0.2, 0, 0) = (0.4, 0, 0).
    sm.update(0.1f, skel, clips, out);
    NF_CHECK(out.size() >= 1);
    NF_CHECK_NEAR(out[0].translation.x, 0.4f, 0.05f);
}

NF_TEST(state_machine_bool_condition) {
    AnimationStateMachine sm;

    AnimState idle{"idle", "idle_clip", 1.0f, {}};
    idle.transitions.push_back({.target_state = "jump", .fade_duration = 0.1f, .conditions = {
        {"jumping", ConditionOp::Equals, true}
    }});
    AnimState jump{"jump", "jump_clip", 1.0f, {}};
    sm.add_state(idle);
    sm.add_state(jump);

    sm.set_param("jumping", false);
    Skeleton skel;
    std::unordered_map<std::string, const AnimationClip*> clips;
    std::vector<LocalPose> out;
    sm.update(0.0f, skel, clips, out);
    NF_CHECK_EQ(sm.current_state(), std::string("idle"));

    sm.set_param("jumping", true);
    sm.update(0.0f, skel, clips, out);
    NF_CHECK_EQ(sm.current_state(), std::string("jump"));
}

NF_TEST(state_machine_reset) {
    AnimationStateMachine sm;

    AnimState s0{"a", "clip_a", 1.0f, {}};
    AnimState s1{"b", "clip_b", 1.0f, {}};
    s0.transitions.push_back({.target_state = "b", .fade_duration = 0.1f, .conditions = {
        {"go", ConditionOp::Greater, 0.0f}
    }});
    sm.add_state(s0);
    sm.add_state(s1);

    sm.set_param("go", 1.0f);
    Skeleton skel;
    std::unordered_map<std::string, const AnimationClip*> clips;
    std::vector<LocalPose> out;
    sm.update(0.1f, skel, clips, out);
    NF_CHECK_EQ(sm.current_state(), std::string("b"));

    sm.reset();
    NF_CHECK_EQ(sm.current_state(), std::string("a"));
    NF_CHECK_EQ(sm.previous_state(), std::string(""));
}

NF_TEST(state_machine_multiple_conditions_all_must_be_true) {
    AnimationStateMachine sm;

    AnimState idle{"idle", "idle_clip", 1.0f, {}};
    idle.transitions.push_back({.target_state = "run", .fade_duration = 0.1f, .conditions = {
        {"speed", ConditionOp::Greater, 2.0f},
        {"stamina", ConditionOp::Greater, 10.0f},
    }});
    AnimState run{"run", "run_clip", 1.0f, {}};
    sm.add_state(idle);
    sm.add_state(run);

    Skeleton skel;
    std::unordered_map<std::string, const AnimationClip*> clips;
    std::vector<LocalPose> out;

    // Only one condition met.
    sm.set_param("speed", 5.0f);
    sm.set_param("stamina", 5.0f);
    sm.update(0.1f, skel, clips, out);
    NF_CHECK_EQ(sm.current_state(), std::string("idle"));

    // Both conditions met.
    sm.set_param("stamina", 15.0f);
    sm.update(0.1f, skel, clips, out);
    NF_CHECK_EQ(sm.current_state(), std::string("run"));
}

// ---------------------------------------------------------------------------
// Procedural clips
//
// The animation import pipeline is a Phase 9 non-goal, so these are the only
// clips a scene can actually carry today. If they are wrong, every animated
// entity in every project is wrong.
// ---------------------------------------------------------------------------

NF_TEST(procedural_clip_first_keyframe_matches_rest) {
    Skeleton skel = make_default_skeleton();
    skel.bones[0].rest_rotation = Quat::from_axis_angle(Vec3(1, 0, 0), 0.5f);

    ProceduralClipSpec spec;
    spec.kind = ProceduralClipSpec::Kind::Spin;
    spec.axis = {0.0f, 1.0f, 0.0f};
    spec.turns = 1.0f;
    spec.duration = 2.0f;

    const AnimationClip clip = make_procedural_clip("spin", skel, spec);
    NF_CHECK_EQ(clip.tracks.size(), static_cast<usize>(1));
    NF_CHECK(!clip.tracks.empty());

    // Sampling at t = 0 must reproduce the rest pose exactly. This is what makes
    // the clip safe to blend against rest and to cross-fade from a rest state;
    // a clip that started mid-rotation would snap on its first frame.
    std::vector<LocalPose> pose;
    clip.sample(0.0f, skel, pose);
    NF_CHECK_EQ(pose.size(), static_cast<usize>(1));
    NF_CHECK_NEAR(pose[0].translation.x, skel.bones[0].rest_translation.x, 1e-5f);
    NF_CHECK_NEAR(pose[0].rotation.x, skel.bones[0].rest_rotation.x, 1e-4f);
    NF_CHECK_NEAR(pose[0].rotation.y, skel.bones[0].rest_rotation.y, 1e-4f);
    NF_CHECK_NEAR(pose[0].rotation.z, skel.bones[0].rest_rotation.z, 1e-4f);
    NF_CHECK_NEAR(pose[0].rotation.w, skel.bones[0].rest_rotation.w, 1e-4f);
}

NF_TEST(procedural_clip_spin_turns_by_a_quarter_at_a_quarter_of_duration) {
    Skeleton skel = make_default_skeleton();

    ProceduralClipSpec spec;
    spec.kind = ProceduralClipSpec::Kind::Spin;
    spec.axis = {0.0f, 1.0f, 0.0f};
    spec.turns = 1.0f;
    spec.duration = 2.0f;

    const AnimationClip clip = make_procedural_clip("spin", skel, spec);
    std::vector<LocalPose> pose;
    clip.sample(0.5f, skel, pose); // a quarter of the way through one full turn

    // A quarter turn about +Y takes the local X axis onto -Z.
    const Vec3 rotated = pose[0].rotation.rotate(Vec3(1.0f, 0.0f, 0.0f));
    NF_CHECK_NEAR(rotated.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(rotated.z, -1.0f, 1e-4f);
}

NF_TEST(procedural_clip_spin_last_keyframe_equals_rest) {
    Skeleton skel = make_default_skeleton();

    ProceduralClipSpec spec;
    spec.kind = ProceduralClipSpec::Kind::Spin;
    spec.axis = {0.0f, 0.0f, 1.0f};
    spec.turns = 1.0f;
    spec.duration = 2.0f;

    const AnimationClip clip = make_procedural_clip("spin", skel, spec);
    std::vector<LocalPose> pose;
    clip.sample(clip.duration, skel, pose);

    // The last keyframe closes the loop, so a looping player wraps without a
    // visible hitch. Comparing rotations rather than quaternion components
    // because q and -q are the same rotation.
    const Vec3 a = pose[0].rotation.rotate(Vec3(1.0f, 0.0f, 0.0f));
    const Vec3 b = skel.bones[0].rest_rotation.rotate(Vec3(1.0f, 0.0f, 0.0f));
    NF_CHECK_NEAR(a.x, b.x, 1e-4f);
    NF_CHECK_NEAR(a.y, b.y, 1e-4f);
    NF_CHECK_NEAR(a.z, b.z, 1e-4f);
}

NF_TEST(procedural_clip_bob_oscillates_along_the_axis) {
    Skeleton skel = make_default_skeleton();

    ProceduralClipSpec spec;
    spec.kind = ProceduralClipSpec::Kind::Bob;
    spec.axis = {0.0f, 1.0f, 0.0f};
    spec.amplitude = 0.5f;
    spec.duration = 2.0f;

    const AnimationClip clip = make_procedural_clip("bob", skel, spec);

    std::vector<LocalPose> pose;
    clip.sample(0.0f, skel, pose);
    NF_CHECK_NEAR(pose[0].translation.y, 0.5f, 1e-4f);

    clip.sample(clip.duration * 0.5f, skel, pose);
    NF_CHECK_NEAR(pose[0].translation.y, -0.5f, 1e-4f);

    // A full cycle returns to where it started, so the loop has no positional
    // discontinuity.
    clip.sample(clip.duration, skel, pose);
    NF_CHECK_NEAR(pose[0].translation.y, 0.5f, 1e-4f);
}

NF_TEST(procedural_clip_keyframe_count_is_clamped) {
    Skeleton skel = make_default_skeleton();

    // Below two there is nothing to interpolate between; an unbounded count
    // would let a scene file allocate without limit.
    ProceduralClipSpec low;
    low.keyframes = 0;
    const AnimationClip few = make_procedural_clip("few", skel, low);
    NF_CHECK_EQ(few.tracks[0].keyframes.size(), static_cast<usize>(3)); // count clamped to 2, +1 closing keyframe

    ProceduralClipSpec high;
    high.keyframes = 100000;
    const AnimationClip many = make_procedural_clip("many", skel, high);
    NF_CHECK_EQ(many.tracks[0].keyframes.size(), static_cast<usize>(257)); // clamped to 256, +1
}

NF_TEST(procedural_clip_without_a_skeleton_is_empty) {
    Skeleton empty;
    ProceduralClipSpec spec;
    const AnimationClip clip = make_procedural_clip("nothing", empty, spec);

    // No rig means no motion. Duration 0 makes that obvious to a caller rather
    // than looking like a clip that plays.
    NF_CHECK(clip.tracks.empty());
    NF_CHECK_NEAR(clip.duration, 0.0f, 1e-6f);
}

NF_TEST(default_skeleton_is_a_single_root_bone) {
    const Skeleton skel = make_default_skeleton();
    NF_CHECK_EQ(skel.bone_count(), static_cast<usize>(1));
    NF_CHECK_EQ(skel.bones[0].name, std::string("root"));
    NF_CHECK_EQ(skel.bones[0].parent, -1);
    NF_CHECK_EQ(skel.find_bone("root"), 0);
}
