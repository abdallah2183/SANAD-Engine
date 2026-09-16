#pragma once

// NF/Animation/IK.hpp — analytic two-bone IK (design doc Section 44).
//
// Given a chain (root -> mid -> end) in world space and a target, computes
// the delta rotations that swing the end onto the target with a pole-vector
// hint controlling the bend direction (elbows/knees). Pure math, no rig:
// callers map the deltas onto their bone local rotations.
//
// Unreachable targets clamp along the reach direction (clamped=true); the
// chain then points at the target fully extended. Degenerate chains
// (zero-length bones, coincident joints) return identity deltas, never NaN.

#include <NF/Core/Math.hpp>

namespace nf::animation {

struct TwoBoneIKResult {
    Quat upper_delta = Quat::identity(); // rotates (mid - root) onto the solved upper dir
    Quat lower_delta = Quat::identity(); // rotates upper*(end - mid) onto the solved lower dir
    Vec3 new_mid{0, 0, 0}; // solved joint positions (world)
    Vec3 new_end{0, 0, 0};
    bool clamped = false; // target beyond max reach
};

TwoBoneIKResult solve_two_bone_ik(Vec3 root_pos, Vec3 mid_pos, Vec3 end_pos, Vec3 target,
                                  Vec3 pole_hint);

} // namespace nf::animation
