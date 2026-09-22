// Samples/AnimationRetarget/main.cpp — G4 acceptance demo: TWO DIFFERENT
// CHARACTERS SHARE ONE LOCOMOTION GRAPH.
//
// This is the whole integration a game developer needs, and it is all public
// API — no engine source is modified:
//
//   1. Author the locomotion graph (clips + state machine) ONCE, against
//      character A's rig.
//   2. Build a retarget map from A to character B by bone name.
//   3. Each frame: step the graph for A, retarget A's pose onto B, and
//      compute world poses for both.
//
// Character B is a genuinely different rig: taller (1.5x limbs), a different
// hip height, and an extra "antenna" bone that A does not have. It is driven
// by the SAME AnimationStateMachine instance as A.
//
// Headless and deterministic: no window, no GPU, no RNG. Exits 0 when every
// check holds, 1 otherwise, so it doubles as a smoke test.
//
// Usage: NFSampleAnimationRetarget [--frames N] [--fps N]

#include <NF/Animation/AnimationStateMachine.hpp>
#include <NF/Animation/Retarget.hpp>
#include <NF/Animation/RootMotion.hpp>
#include <NF/Animation/Skeleton.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using namespace nf;
using namespace nf::animation;

namespace {

/// Character A — compact rig: root -> hips -> torso -> head, hips -> legs.
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

/// Character B — tall rig: 1.5x limb lengths, higher hips, plus an "antenna"
/// bone with no counterpart on A (it must keep B's rest pose).
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

/// A walk clip authored against A: the hips bob ±0.05 around their 0.8 rest
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

struct Args {
    int frames = 90;
    float fps = 30.0f;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            a.frames = std::atoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            a.fps = static_cast<float>(std::atof(argv[++i]));
        }
    }
    if (a.frames < 1) a.frames = 1;
    if (a.fps <= 0.0f) a.fps = 30.0f;
    return a;
}

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_failures;
}

float world_y(const std::vector<WorldPose>& world, usize bone) {
    return world[bone].transform.m[3][1];
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const float dt = 1.0f / args.fps;

    const Skeleton a = make_character_a();
    const Skeleton b = make_character_b();

    // ---- 1. Author the locomotion graph ONCE, against A's rig. -----------
    AnimationClip idle = make_idle_clip_a();
    AnimationClip walk = make_walk_clip_a();

    std::unordered_map<std::string, const AnimationClip*> clips;
    clips["idle_clip"] = &idle;
    clips["walk_clip"] = &walk;

    AnimationStateMachine locomotion; // THE shared graph — one instance
    AnimState st_idle{"idle", "idle_clip", 1.0f, {}};
    st_idle.transitions.push_back({.target_state = "walk",
                                   .fade_duration = 0.25f,
                                   .conditions = {{"speed", ConditionOp::Greater, 0.5f}}});
    AnimState st_walk{"walk", "walk_clip", 1.0f, {}};
    st_walk.transitions.push_back({.target_state = "idle",
                                   .fade_duration = 0.25f,
                                   .conditions = {{"speed", ConditionOp::LessEqual, 0.5f}}});
    locomotion.add_state(st_idle);
    locomotion.add_state(st_walk);

    // ---- 2. Retarget A -> B by bone name (B keeps its own proportions). --
    const RetargetMap map = build_retarget_map(a, b);

    std::printf("NOVAForge G4 demo — two characters, one locomotion graph\n");
    std::printf("  character A: %zu bones, character B: %zu bones\n", a.bone_count(),
                b.bone_count());
    std::printf("  shared graph: %zu states, %zu matched bones, scale %.4f\n\n",
                locomotion.state_count(), map.matched_bone_count(),
                static_cast<double>(map.translation_scale));

    std::vector<LocalPose> pose_a;
    std::vector<LocalPose> pose_b;
    std::vector<WorldPose> world_a;
    std::vector<WorldPose> world_b;

    // ---- 3. Drive BOTH characters from the one graph, frame by frame. ----
    const int walk_start = args.frames / 3;
    float a_hips_last = 0.0f;
    float b_hips_last = 0.0f;
    float b_ratio_last = 0.0f;
    bool saw_walk = false;
    bool b_proportions_held = true;

    for (int frame = 0; frame < args.frames; ++frame) {
        locomotion.set_param("speed", frame >= walk_start ? 1.0f : 0.0f);
        locomotion.update(dt, a, clips, pose_a);       // graph step, A's rig
        retarget_pose(a, pose_a, map, b, pose_b);      // same pose, B's rig
        compute_world_transforms(a, pose_a, world_a);
        compute_world_transforms(b, pose_b, world_b);

        if (locomotion.current_state() == "walk") saw_walk = true;

        a_hips_last = world_y(world_a, 1);
        b_hips_last = world_y(world_b, 1);
        b_ratio_last = (std::abs(a_hips_last) > 1e-6f) ? b_hips_last / a_hips_last : 0.0f;

        // B's leg bones must keep B's rest length no matter what A does.
        const Vec3 hip_b{world_b[1].transform.m[3][0], world_b[1].transform.m[3][1],
                         world_b[1].transform.m[3][2]};
        const Vec3 leg_b{world_b[4].transform.m[3][0], world_b[4].transform.m[3][1],
                         world_b[4].transform.m[3][2]};
        const float b_leg_rest = Vec3{-0.2f, -1.2f, 0}.length();
        if (std::abs((leg_b - hip_b).length() - b_leg_rest) > 1e-3f) {
            b_proportions_held = false;
        }
    }

    std::printf("after %d frames @ %.0f fps:\n", args.frames, static_cast<double>(args.fps));
    std::printf("  state            : %s\n", locomotion.current_state().c_str());
    std::printf("  A hips height    : %.4f m\n", static_cast<double>(a_hips_last));
    std::printf("  B hips height    : %.4f m  (A x %.4f)\n",
                static_cast<double>(b_hips_last), static_cast<double>(b_ratio_last));

    // ---- Root motion: the same clip moves B proportionally further. ------
    const RootMotionDelta a_step = extract_root_motion(walk, a, 0.0f, 0.5f);
    const float b_step_z = a_step.translation.z * map.translation_scale;
    std::printf("  root travel (0.5s cycle): A %.4f m, B %.4f m\n",
                static_cast<double>(a_step.translation.z), static_cast<double>(b_step_z));
    std::printf("\nchecks:\n");

    check(locomotion.state_count() == 2, "one graph instance holds both states");
    check(saw_walk, "the shared graph reached the walk state");
    check(map.matched_bone_count() == 6, "six bones matched by name");
    check(map.source_of[6] == -1, "B's antenna bone is unmatched (keeps rest)");
    check(b_proportions_held, "B's leg keeps B's rest length throughout");
    check(std::abs(b_ratio_last - 1.5f) < 1e-3f, "B's hips ride at its own 1.5x proportion");
    check(a_step.translation.z > 0.9f && a_step.translation.z < 1.1f,
          "root motion extracts ~1 m over half the 2 m cycle");
    check(b_step_z > a_step.translation.z, "the taller rig covers more ground from the same clip");

    std::printf("\n%s\n", g_failures == 0 ? "DEMO PASSED" : "DEMO FAILED");
    return g_failures == 0 ? 0 : 1;
}
