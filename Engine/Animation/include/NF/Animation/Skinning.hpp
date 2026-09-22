#pragma once

// NF/Animation/Skinning.hpp — CPU skin-matrix preparation.
//
// Produces the per-joint matrices a GPU skinning vertex shader consumes:
// `palette[i] = inverse_bind[i] * world[i]`, applied to a bind-space vertex
// as `v' = v * palette[joint]` (row-vector convention, matching Mat4 — the
// un-bind must happen first, so the product order is the transpose of the
// column-vector form). The
// CPU path here is the reference implementation for tests and for a future
// cook step; the skinned *render* path is render-core work (see
// COORDINATION.md, G4 request).

#include <NF/Animation/Skeleton.hpp>
#include <vector>

namespace nf::animation {

/// Compute the inverse bind (inverse rest-pose world) matrix per bone. The
/// bind pose is the skeleton's rest pose; a palette sampled at the rest pose
/// is therefore the identity for every bone.
std::vector<Mat4> compute_inverse_bind_pose(const Skeleton& skel);

/// Compute the skin palette: one matrix per bone, `world * inverse_bind`.
/// `inverse_bind` must have `skel.bone_count()` elements (as produced by
/// `compute_inverse_bind_pose`); `world` comes from `compute_world_transforms`.
void compute_skin_palette(const Skeleton& skel,
                          const std::vector<WorldPose>& world,
                          const std::vector<Mat4>& inverse_bind,
                          std::vector<Mat4>& out_palette);

} // namespace nf::animation
