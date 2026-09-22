#pragma once

// NF/Assets/AnimationImport.hpp — bridge from imported glTF skin/animation
// data to the engine's runtime animation types (nf::animation::Skeleton /
// AnimationClip). This is the import/cook half of the character pipeline:
// Blender → glTF → GltfImport → *these functions* → runtime sampling.
//
// Rules:
//   - Bones come from the skin's joint nodes in glTF joint order; rest pose
//     is each joint node's local TRS. A joint whose parent is outside the
//     joint set becomes a root (parent -1).
//   - Partial channels are merged with the skeleton's rest pose: a clip that
//     animates only translation keeps the bone's rest rotation/scale, so a
//     track never clobbers components the file does not animate.
//   - Channels that target nodes outside the skin's joints fail loudly with
//     the offending node named. No silent substitution, no default rig.

#include <NF/Assets/GltfImport.hpp>
#include <NF/Animation/AnimationClip.hpp>
#include <NF/Animation/Skeleton.hpp>

#include <string>
#include <vector>

namespace nf::assets {

/// Node index → bone index for the skin's joints. Size is
/// `result.nodes.size()`; -1 for every node that is not a joint of that skin.
/// Returns an empty vector when `skin_index` is out of range.
std::vector<int> joint_node_to_bone(const GltfImportResult& result, usize skin_index);

/// Builds a runtime skeleton from the skin's joint nodes and their local TRS.
bool make_skeleton(const GltfImportResult& result, usize skin_index,
                   nf::animation::Skeleton& out, std::string& out_error);

/// Builds a runtime clip from an imported animation, mapping channels onto
/// skeleton bones via `node_to_bone` (as returned by joint_node_to_bone).
/// Channel keyframe times are merged across the bone's curves; components a
/// curve does not animate fall back to the bone's rest pose.
bool make_clip(const GltfImportResult& result, const GltfAnimationInfo& anim,
               const nf::animation::Skeleton& skeleton,
               const std::vector<int>& node_to_bone,
               nf::animation::AnimationClip& out, std::string& out_error);

} // namespace nf::assets
