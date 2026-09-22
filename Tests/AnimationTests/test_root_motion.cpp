// AnimationTests — root motion extraction from clips: linear travel,
// rotation, loop-wrap accumulation, clamping.

#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/RootMotion.hpp>
#include <NF/Animation/Skeleton.hpp>

#include <cmath>

using namespace nf;
using namespace nf::animation;

namespace {

Skeleton make_one_bone() {
    return make_default_skeleton();
}

/// A 1 s looping walk: the root travels 4 m along +Z and ends where it
/// started in rotation.
AnimationClip make_walk_forward() {
    AnimationClip clip;
    clip.name = "walk";
    clip.duration = 1.0f;
    clip.looping = true;

    AnimationTrack root;
    root.bone_index = 0;
    root.keyframes = {{0.0f, Vec3{0, 0, 0}}, {1.0f, Vec3{0, 0, 4}}};
    clip.tracks.push_back(root);
    return clip;
}

} // namespace

NF_TEST(root_motion_translation_delta) {
    const Skeleton skel = make_one_bone();
    const AnimationClip walk = make_walk_forward();

    const RootMotionDelta d = extract_root_motion(walk, skel, 0.0f, 1.0f);
    NF_CHECK_NEAR(d.translation.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(d.translation.y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(d.translation.z, 4.0f, 1e-5f);
    // No rotation baked into this clip.
    NF_CHECK_NEAR(d.rotation.w, 1.0f, 1e-6f);
}

NF_TEST(root_motion_interpolates_mid_segment) {
    const Skeleton skel = make_one_bone();
    const AnimationClip walk = make_walk_forward();

    const RootMotionDelta d = extract_root_motion(walk, skel, 0.25f, 0.75f);
    NF_CHECK_NEAR(d.translation.z, 2.0f, 1e-5f); // half of the 4 m cycle
}

NF_TEST(root_motion_rotation_delta) {
    const Skeleton skel = make_one_bone();

    AnimationClip turn;
    turn.name = "turn";
    turn.duration = 1.0f;
    turn.looping = false;
    AnimationTrack root;
    root.bone_index = 0;
    root.keyframes = {{0.0f, Vec3{0, 0, 0}, Quat::identity()},
                      {1.0f, Vec3{0, 0, 0},
                       Quat::from_axis_angle(Vec3{0, 1, 0}, HALF_PI)}};
    turn.tracks.push_back(root);

    const RootMotionDelta d = extract_root_motion(turn, skel, 0.0f, 1.0f);
    // The delta must compose onto the start rotation to give the end: local
    // convention is end = start * delta.
    const Quat composed = Quat::identity() * d.rotation;
    const Vec3 rotated = composed.rotate(Vec3{1, 0, 0});
    NF_CHECK_NEAR(rotated.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(rotated.z, -1.0f, 1e-4f); // +X turned onto -Z: a 90 deg turn
}

NF_TEST(root_motion_loop_wrap_accumulates) {
    const Skeleton skel = make_one_bone();
    const AnimationClip walk = make_walk_forward();

    // From t = 0.75 to t = 0.25 across the wrap: 1 m to the end of the cycle
    // plus 1 m from its start = 2 m, not -0.5 * 4.
    const RootMotionDelta d = extract_root_motion(walk, skel, 0.75f, 0.25f);
    NF_CHECK_NEAR(d.translation.z, 2.0f, 1e-5f);

    // Walking N cycles by composing per-cycle deltas covers N * 4 m exactly
    // (each cycle's delta is the same, so accumulation is trivially stable).
    f32 travelled = 0.0f;
    for (int i = 0; i < 10; ++i) {
        travelled += extract_root_motion(walk, skel, 0.0f, 1.0f).translation.z;
    }
    NF_CHECK_NEAR(travelled, 40.0f, 1e-4f);
}

NF_TEST(root_motion_without_root_track_is_identity) {
    const Skeleton skel = make_one_bone();

    AnimationClip static_clip;
    static_clip.name = "static";
    static_clip.duration = 1.0f;
    // No tracks at all.

    const RootMotionDelta d = extract_root_motion(static_clip, skel, 0.0f, 1.0f);
    NF_CHECK_NEAR(d.translation.x, 0.0f, 1e-7f);
    NF_CHECK_NEAR(d.translation.y, 0.0f, 1e-7f);
    NF_CHECK_NEAR(d.translation.z, 0.0f, 1e-7f);
    NF_CHECK_NEAR(d.rotation.w, 1.0f, 1e-7f);
}
