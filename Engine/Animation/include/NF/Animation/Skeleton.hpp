#pragma once

#include <NF/Core/Math.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace nf::animation {

/// A single bone in a skeleton hierarchy. Stores the bone name, the parent bone
/// index (or -1 for a root), and the rest-pose local transform (relative to the
/// parent).
struct Bone {
    std::string name;
    int32_t parent = -1;
    Vec3 rest_translation;
    Quat rest_rotation = Quat::identity();
    Vec3 rest_scale = {1.0f, 1.0f, 1.0f};
};

/// A skeleton: a flat array of bones in hierarchical order (parents before
/// children). The order is significant — world transforms are computed by
/// walking the array, and a child must appear after its parent.
struct Skeleton {
    std::vector<Bone> bones;

    usize bone_count() const { return bones.size(); }
    int32_t find_bone(const std::string& name) const;
};

/// A per-bone local transform: the output of sampling an animation clip.
/// Indexed identically to `Skeleton::bones`.
struct LocalPose {
    Vec3 translation;
    Quat rotation = Quat::identity();
    Vec3 scale = {1.0f, 1.0f, 1.0f};
};

/// A per-bone world-space transform: the result of walking the skeleton
/// hierarchy from the local pose.
struct WorldPose {
    Mat4 transform = Mat4::identity();
};

/// Compute world-space transforms from a local pose and a skeleton.
/// `local` must have the same number of elements as `skeleton.bones`.
/// The output `world` is resized to match.
void compute_world_transforms(const Skeleton& skel,
                              const std::vector<LocalPose>& local,
                              std::vector<WorldPose>& world);

/// A one-bone rig rooted at the origin, the bone named "root".
///
/// Used when a scene asks for a procedural clip but carries no skeleton data.
/// The skeleton import pipeline is a Phase 9 non-goal (§7), so without this a
/// scene has no way to name a rig; a single root bone is the smallest rig that
/// can still express "this entity moves", and the runtime logs when it is
/// substituted so the gap stays visible rather than looking like real data.
Skeleton make_default_skeleton();

} // namespace nf::animation
