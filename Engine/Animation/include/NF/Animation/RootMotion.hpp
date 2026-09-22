#pragma once

// NF/Animation/RootMotion.hpp — root motion extraction from animation clips.
//
// Locomotion clips usually bake the character's travel into the root bone's
// track. To drive an entity (physics body, controller) from the clip instead
// of animating its transform directly, extract the per-frame delta and apply
// it to the entity: translation is in the root bone's parent space (for the
// usual rig that is entity space), rotation is the local delta that satisfies
// `end_rotation = start_rotation * delta`.

#include <NF/Animation/AnimationClip.hpp>
#include <NF/Animation/Skeleton.hpp>

namespace nf::animation {

/// A root-motion delta between two times of one clip.
struct RootMotionDelta {
    Vec3 translation{0.0f, 0.0f, 0.0f}; ///< Entity-space translation delta.
    Quat rotation = Quat::identity();   ///< Local delta: end = start * delta.
};

/// Extract the root bone's motion between `from_time` and `to_time`.
///
/// The root bone is bone 0 (the convention `make_procedural_clip` uses). The
/// delta is taken from bone 0's track; a clip without a root track yields an
/// identity delta. When `from_time > to_time` and the clip loops, the delta
/// spans the wrap: the segment to the end of the cycle composed with the
/// segment from the start, so a looping walk accumulates correctly.
RootMotionDelta extract_root_motion(const AnimationClip& clip, const Skeleton& skel,
                                    f32 from_time, f32 to_time);

} // namespace nf::animation
