// AnimationTests — two-bone IK: reach, clamp, pole, degeneracy.

#include <NF/Animation/IK.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>

using namespace nf;
using namespace nf::animation;

namespace {

float dist(Vec3 a, Vec3 b) {
    return (a - b).length();
}

} // namespace

NF_TEST(ik_reaches_target_exactly) {
    const Vec3 root{0, 0, 0}, mid{0, -1, 0}, end{0, -2, 0};
    const Vec3 target{1.0f, -1.0f, 0.0f};
    const auto r = solve_two_bone_ik(root, mid, end, target, Vec3{0, 0, 1});
    NF_CHECK(!r.clamped);
    NF_CHECK(dist(r.new_end, target) < 1e-4f);
    // Bone lengths preserved.
    NF_CHECK_NEAR(dist(root, r.new_mid), 1.0f, 1e-5f);
    NF_CHECK_NEAR(dist(r.new_mid, r.new_end), 1.0f, 1e-5f);
    // Deltas actually move the chain: applying them reproduces the solution.
    const Vec3 moved_mid = root + r.upper_delta.rotate(mid - root);
    NF_CHECK(dist(moved_mid, r.new_mid) < 1e-4f);
    const Vec3 moved_end = moved_mid + (r.lower_delta * r.upper_delta).rotate(end - mid);
    NF_CHECK(dist(moved_end, r.new_end) < 1e-3f);
}

NF_TEST(ik_clamps_beyond_max_reach) {
    const Vec3 root{0, 0, 0}, mid{0, -1, 0}, end{0, -2, 0};
    const auto r = solve_two_bone_ik(root, mid, end, Vec3{0, -10, 0}, Vec3{0, 0, 1});
    NF_CHECK(r.clamped);
    NF_CHECK(dist(r.new_end, Vec3{0, -2, 0}) < 1e-4f); // fully extended down
    NF_CHECK_NEAR(dist(root, r.new_mid), 1.0f, 1e-5f);
}

NF_TEST(ik_pole_hint_chooses_bend_side) {
    const Vec3 root{0, 0, 0}, mid{0, -1, 0}, end{0, -2, 0};
    const Vec3 target{0.0f, -1.5f, 0.0f};
    const auto fwd = solve_two_bone_ik(root, mid, end, target, Vec3{0, 0, 1});
    const auto back = solve_two_bone_ik(root, mid, end, target, Vec3{0, 0, -1});
    NF_CHECK(!fwd.clamped && !back.clamped);
    NF_CHECK(dist(fwd.new_end, target) < 1e-4f);
    NF_CHECK(dist(back.new_end, target) < 1e-4f);
    // Same target, opposite elbows.
    NF_CHECK(fwd.new_mid.z * back.new_mid.z < 0.0f);
    NF_CHECK_NEAR(std::abs(fwd.new_mid.z), std::abs(back.new_mid.z), 1e-4f);
}

NF_TEST(ik_degenerate_chains_hold_still) {
    // Zero-length upper bone.
    auto r1 = solve_two_bone_ik(Vec3{0, 0, 0}, Vec3{0, 0, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0},
                                Vec3{0, 0, 1});
    NF_CHECK(dist(r1.new_end, Vec3{0, -1, 0}) < 1e-6f);
    // Target exactly on the root.
    auto r2 = solve_two_bone_ik(Vec3{1, 2, 3}, Vec3{1, 1, 3}, Vec3{1, 0, 3}, Vec3{1, 2, 3},
                                Vec3{0, 0, 1});
    NF_CHECK(dist(r2.new_end, Vec3{1, 0, 3}) < 1e-6f);
    // Deltas are unit quaternions (safe to compose into a pose).
    NF_CHECK_NEAR(r2.upper_delta.normalized().w, r2.upper_delta.w, 1e-5f);
}
