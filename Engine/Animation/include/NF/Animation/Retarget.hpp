#pragma once

// NF/Animation/Retarget.hpp — pose retargeting across different skeletons.
//
// A locomotion graph (state machine + clips) is authored once against one rig
// and played on any rig whose bones share the source's bone NAMES. The map is
// built by name matching; target bones with no source counterpart keep their
// rest pose, and source bones the target lacks are simply skipped.
//
// Per matched bone the transfer is rest-relative: the source pose's rotation
// is expressed as an offset from the source's rest rotation
// (`d = src_rest^-1 * src_pose`) and reapplied on the target's rest rotation
// (`tgt_rest * d`). This carries the same *local* motion onto a rig whose
// rest orientation differs. Translation keeps the TARGET's bone length
// (proportions are never broken) while transferring the source's animated
// compression/stretch as a ratio; the root bone transfers its full
// translation scaled by `translation_scale`, so a taller rig covers more
// ground per walk cycle than a shorter one from the same clip.

#include <NF/Animation/Skeleton.hpp>
#include <cstdint>
#include <vector>

namespace nf::animation {

/// A name-matched bone correspondence between two skeletons.
struct RetargetMap {
    /// Per TARGET bone index: the source bone index, or -1 when the target
    /// bone has no name match and must keep its rest pose.
    std::vector<int32_t> source_of;
    /// Uniform scale from source-space metres to target-space metres, derived
    /// from the mean rest bone-length ratio over matched non-root bones.
    f32 translation_scale = 1.0f;

    usize matched_bone_count() const;
};

/// Build a retarget map by bone-name matching. Deterministic: the scale is
/// the mean of matched non-root bone length ratios (rest translation length),
/// ignoring bones whose source rest translation is degenerate (< 1e-6).
RetargetMap build_retarget_map(const Skeleton& source, const Skeleton& target);

/// Retarget one sampled local pose from `source` onto `target`.
/// `source_pose` must have `source.bone_count()` elements. The output is
/// resized to `target.bone_count()`; unmatched target bones keep rest.
void retarget_pose(const Skeleton& source,
                   const std::vector<LocalPose>& source_pose,
                   const RetargetMap& map,
                   const Skeleton& target,
                   std::vector<LocalPose>& out_target_pose);

} // namespace nf::animation
