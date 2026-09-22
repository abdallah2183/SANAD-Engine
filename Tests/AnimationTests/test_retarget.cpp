// AnimationTests — retargeting + the G4 acceptance demo: two different
// characters sharing ONE locomotion graph.
//
// The locomotion graph (state machine + clip table) is authored once against
// character A's rig. Character B is a different skeleton — taller, different
// bone lengths, an extra bone A does not have — and receives the graph's
// output through retarget_pose each frame. No engine source changes are
// needed by the "developer" of character B: build_retarget_map + retarget_pose
// are the whole integration.

#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/AnimationStateMachine.hpp>
#include <NF/Animation/Retarget.hpp>
#include <NF/Animation/Skeleton.hpp>

#include <cmath>
#include <unordered_map>
#include <vector>

using namespace nf;
using namespace nf::animation;

namespace {

/// Character A — compact rig: root -> hips -> torso -> head, hips -> legs.
/// Legs sit at a lateral offset so bone lengths are non-trivial.
Skeleton make_character_a() {
    Skeleton skel;
    skel.bones.resize(6);
    skel.bones[0] = {"root", -1, Vec3{0, 0, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[1] = {"hips", 0, Vec3{0, 0.8f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[2] = {"torso", 1, Vec3{0, 0.5f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[3] = {"head", 2, Vec3{0, 0.4f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[4] = {"leg_l", 1, Vec3{-0.15f, -0.8f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[5] = {"leg_r", 1, Vec3{0.15f, -0.8f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    return skel;
}

/// Character B — tall rig: 1.5x the limb lengths, different hip height, plus
/// an "antenna" bone that has no counterpart on A.
Skeleton make_character_b() {
    Skeleton skel;
    skel.bones.resize(7);
    skel.bones[0] = {"root", -1, Vec3{0, 0, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[1] = {"hips", 0, Vec3{0, 1.2f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[2] = {"torso", 1, Vec3{0, 0.75f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[3] = {"head", 2, Vec3{0, 0.6f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[4] = {"leg_l", 1, Vec3{-0.2f, -1.2f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[5] = {"leg_r", 1, Vec3{0.2f, -1.2f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    skel.bones[6] = {"antenna", 3, Vec3{0, 0.3f, 0}, Quat::identity(), Vec3{1, 1, 1}};
    return skel;
}

/// A walk clip authored against A: the hips bob +-0.05 around their 0.8 rest
/// height and the root travels 2 m forward per 1 s cycle.
AnimationClip make_walk_clip_a() {
    AnimationClip clip;
    clip.name = "walk";
    clip.duration = 1.0f;
    clip.looping = true;

    AnimationTrack root;
    root.bone_index = 0;
    root.keyframes = {{0.0f, Vec3{0, 0, 0}}, {1.0f, Vec3{0, 0, 2}}};
    clip.tracks.push_back(root);

    AnimationTrack hips;
    hips.bone_index = 1;
    hips.keyframes = {{0.0f, Vec3{0, 0.85f, 0}}, {0.5f, Vec3{0, 0.75f, 0}},
                      {1.0f, Vec3{0, 0.85f, 0}}};
    clip.tracks.push_back(hips);
    return clip;
}

/// An idle clip authored against A: everything pinned at rest.
AnimationClip make_idle_clip_a() {
    AnimationClip clip;
    clip.name = "idle";
    clip.duration = 1.0f;
    clip.looping = true;

    AnimationTrack hips;
    hips.bone_index = 1;
    hips.keyframes = {{0.0f, Vec3{0, 0.8f, 0}}, {1.0f, Vec3{0, 0.8f, 0}}};
    clip.tracks.push_back(hips);
    return clip;
}

} // namespace

NF_TEST(retarget_map_matches_by_name_and_computes_scale) {
    const Skeleton a = make_character_a();
    const Skeleton b = make_character_b();

    const RetargetMap map = build_retarget_map(a, b);

    // Six A-named bones match; B's "antenna" does not.
    NF_CHECK_EQ(map.matched_bone_count(), static_cast<usize>(6));
    NF_CHECK_EQ(map.source_of[1], 1);  // b.hips -> a.hips
    NF_CHECK_EQ(map.source_of[4], 4);  // b.leg_l -> a.leg_l
    NF_CHECK_EQ(map.source_of[6], -1); // b.antenna unmatched

    // Mean matched non-root rest-length ratio over all FIVE matched non-root
    // bones: hips/torso/head are exactly 1.5 each, and both legs carry the
    // same lateral-offset ratio (leg_l and leg_r have equal lengths). The
    // mean is over 5 bones, so the leg ratio enters twice. Deterministic,
    // computed here from the same rig data.
    const f32 leg_a = Vec3{-0.15f, -0.8f, 0}.length();
    const f32 leg_b = Vec3{-0.2f, -1.2f, 0}.length();
    const f32 expected = (1.5f * 3.0f + 2.0f * (leg_b / leg_a)) / 5.0f;
    NF_CHECK_NEAR(map.translation_scale, expected, 1e-5f);
}

NF_TEST(retarget_unmatched_bone_keeps_rest) {
    const Skeleton a = make_character_a();
    const Skeleton b = make_character_b();
    const RetargetMap map = build_retarget_map(a, b);

    std::vector<LocalPose> pose_a(a.bone_count());
    std::vector<LocalPose> pose_b;
    // Garbage on the source side must not leak into the unmatched bone.
    for (auto& lp : pose_a) lp.translation = Vec3{9, 9, 9};

    retarget_pose(a, pose_a, map, b, pose_b);

    NF_CHECK_NEAR(pose_b[6].translation.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(pose_b[6].translation.y, 0.3f, 1e-6f); // antenna rest offset
    NF_CHECK_NEAR(pose_b[6].rotation.w, 1.0f, 1e-6f);
}

NF_TEST(retarget_transfers_hips_motion_at_target_proportions) {
    const Skeleton a = make_character_a();
    const Skeleton b = make_character_b();
    const RetargetMap map = build_retarget_map(a, b);

    const AnimationClip walk = make_walk_clip_a();

    std::vector<LocalPose> pose_a;
    std::vector<LocalPose> pose_b;

    // Walk peak (t = 0): A's hips at 0.85 (rest 0.8). B's hips rest at 1.2,
    // so the same rise must land on 0.85 / 0.8 * 1.2 = 1.275 — B's own
    // proportion, A's motion.
    walk.sample(0.0f, a, pose_a);
    retarget_pose(a, pose_a, map, b, pose_b);
    NF_CHECK_NEAR(pose_b[1].translation.y, 1.275f, 1e-4f);

    // Walk trough (t = 0.5): A at 0.75 -> B at 0.75 / 0.8 * 1.2 = 1.125.
    walk.sample(0.5f, a, pose_a);
    retarget_pose(a, pose_a, map, b, pose_b);
    NF_CHECK_NEAR(pose_b[1].translation.y, 1.125f, 1e-4f);
}

NF_TEST(retarget_root_translation_scales_with_rig) {
    const Skeleton a = make_character_a();
    const Skeleton b = make_character_b();
    const RetargetMap map = build_retarget_map(a, b);
    const AnimationClip walk = make_walk_clip_a();

    std::vector<LocalPose> pose_a;
    std::vector<LocalPose> pose_b;

    // Sample mid-cycle (t = 0.75 -> root at z = 1.5; t = 1.0 would wrap to 0
    // on a looping clip). The taller rig covers proportionally more ground
    // from the SAME clip — stride scales with the rig.
    walk.sample(0.75f, a, pose_a);
    retarget_pose(a, pose_a, map, b, pose_b);
    NF_CHECK_NEAR(pose_b[0].translation.z, 1.5f * map.translation_scale, 1e-4f);
}

// ---------------------------------------------------------------------------
// G4 acceptance demo: two different characters share ONE locomotion graph.
// ---------------------------------------------------------------------------

NF_TEST(demo_two_characters_share_one_locomotion_graph) {
    const Skeleton a = make_character_a();
    const Skeleton b = make_character_b();

    // --- The locomotion graph, authored ONCE, against A's rig. -----------
    AnimationClip idle = make_idle_clip_a();
    AnimationClip walk = make_walk_clip_a();

    std::unordered_map<std::string, const AnimationClip*> clips;
    clips["idle_clip"] = &idle;
    clips["walk_clip"] = &walk;

    AnimationStateMachine locomotion; // THE shared graph
    AnimState st_idle{"idle", "idle_clip", 1.0f, {}};
    st_idle.transitions.push_back(
        {.target_state = "walk", .fade_duration = 0.25f, .conditions = {
             {"speed", ConditionOp::Greater, 0.5f}}});
    AnimState st_walk{"walk", "walk_clip", 1.0f, {}};
    st_walk.transitions.push_back(
        {.target_state = "idle", .fade_duration = 0.25f, .conditions = {
             {"speed", ConditionOp::LessEqual, 0.5f}}});
    locomotion.add_state(st_idle);
    locomotion.add_state(st_walk);
    // ---------------------------------------------------------------------

    const RetargetMap map = build_retarget_map(a, b);

    std::vector<WorldPose> world_a;
    std::vector<WorldPose> world_b;

    // Frame 1: standing (speed 0). Both characters idle.
    locomotion.set_param("speed", 0.0f);
    std::vector<LocalPose> pose_a;
    std::vector<LocalPose> pose_b;
    locomotion.update(1.0f / 30.0f, a, clips, pose_a);
    retarget_pose(a, pose_a, map, b, pose_b);
    compute_world_transforms(a, pose_a, world_a);
    compute_world_transforms(b, pose_b, world_b);

    NF_CHECK_EQ(locomotion.current_state(), std::string("idle"));
    NF_CHECK_NEAR(world_a[1].transform.m[3][1], 0.8f, 1e-4f); // A hips rest height
    NF_CHECK_NEAR(world_b[1].transform.m[3][1], 1.2f, 1e-4f); // B hips rest height

    // Frame 2: start walking (speed 1). The SAME graph instance drives both.
    locomotion.set_param("speed", 1.0f);
    locomotion.update(1.0f / 30.0f, a, clips, pose_a); // fires idle -> walk
    for (int frame = 0; frame < 30; ++frame) {         // fade in fully
        locomotion.update(1.0f / 30.0f, a, clips, pose_a);
        retarget_pose(a, pose_a, map, b, pose_b);
    }
    NF_CHECK_EQ(locomotion.current_state(), std::string("walk"));

    compute_world_transforms(a, pose_a, world_a);
    compute_world_transforms(b, pose_b, world_b);

    // Both characters walk off the one graph: B's hips ride 1.5x higher
    // (its own proportion) while tracking A's bob phase exactly.
    const f32 a_hips = world_a[1].transform.m[3][1];
    const f32 b_hips = world_b[1].transform.m[3][1];
    NF_CHECK_NEAR(b_hips, a_hips * 1.5f, 1e-3f);

    // B's proportions survive the animation: leg bones keep B's rest length.
    const Vec3 hip_b{world_b[1].transform.m[3][0], world_b[1].transform.m[3][1],
                     world_b[1].transform.m[3][2]};
    const Vec3 leg_b{world_b[4].transform.m[3][0], world_b[4].transform.m[3][1],
                     world_b[4].transform.m[3][2]};
    // Hoisted: a braced-init temporary inside the macro would split its args.
    const f32 b_leg_rest_len = Vec3{-0.2f, -1.2f, 0}.length();
    NF_CHECK_NEAR((leg_b - hip_b).length(), b_leg_rest_len, 1e-4f);

    // B's unmatched antenna bone is exactly where B's rest pose puts it:
    // head world position plus the 0.3 m rest offset along +Y.
    const Vec3 head_b{world_b[3].transform.m[3][0], world_b[3].transform.m[3][1],
                      world_b[3].transform.m[3][2]};
    const Vec3 antenna{world_b[6].transform.m[3][0], world_b[6].transform.m[3][1],
                       world_b[6].transform.m[3][2]};
    NF_CHECK_NEAR(antenna.x - head_b.x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(antenna.y - head_b.y, 0.3f, 1e-5f);

    // Determinism: replaying the whole demo reproduces the same B pose
    // bit-for-bit (no unseeded state anywhere in the graph or the retarget).
    AnimationStateMachine replay = locomotion;
    replay.reset();
    replay.set_param("speed", 0.0f);
    std::vector<LocalPose> pose_a2;
    std::vector<LocalPose> pose_b2;
    replay.update(1.0f / 30.0f, a, clips, pose_a2);
    retarget_pose(a, pose_a2, map, b, pose_b2);
    replay.set_param("speed", 1.0f);
    replay.update(1.0f / 30.0f, a, clips, pose_a2);
    for (int frame = 0; frame < 30; ++frame) {
        replay.update(1.0f / 30.0f, a, clips, pose_a2);
        retarget_pose(a, pose_a2, map, b, pose_b2);
    }
    NF_CHECK_EQ(pose_b2.size(), pose_b.size());
    for (usize i = 0; i < pose_b.size(); ++i) {
        NF_CHECK(pose_b2[i].translation.nearly_equals(pose_b[i].translation, 1e-6f));
        NF_CHECK_NEAR(pose_b2[i].rotation.x, pose_b[i].rotation.x, 1e-6f);
        NF_CHECK_NEAR(pose_b2[i].rotation.y, pose_b[i].rotation.y, 1e-6f);
        NF_CHECK_NEAR(pose_b2[i].rotation.z, pose_b[i].rotation.z, 1e-6f);
        NF_CHECK_NEAR(pose_b2[i].rotation.w, pose_b[i].rotation.w, 1e-6f);
    }
}
