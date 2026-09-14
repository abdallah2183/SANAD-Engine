#pragma once

#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/Camera.hpp>

#include <vector>

namespace nf::rendering {

// Frustum culling for AABB and sphere
bool is_visible(const AABB& aabb, const Frustum& frustum);
bool is_visible(const BoundingSphere& sphere, const Frustum& frustum);

// Transform an AABB by a translation (for now, we only handle translation, not rotation/scale)
// For a full engine, this would take a Mat4, but for the test we just translate by world position.
AABB transform_aabb(const AABB& local, float world_x, float world_y, float world_z);
BoundingSphere transform_sphere(const BoundingSphere& local, float world_x, float world_y, float world_z);

/// Culls a RenderWorld against a frustum.
///
/// RenderWorld → Frustum Culling → Visible Objects → Draw Submission
///
/// Fills `out_visible` (cleared first) with indices into rw.objects for every
/// visible, renderable object (valid mesh handle). The bounding sphere is
/// tested first — it is conservative but branch-cheap; objects that pass it
/// are kept without an extra AABB test. Replacement culling implementations
/// (hierarchical, SIMD, GPU occlusion) can drop in behind this signature.
void cull_render_world(const RenderWorld& rw, const Frustum& frustum, std::vector<u32>& out_visible);

/// Convenience overload taking the camera's cached frustum.
void cull_render_world(const RenderWorld& rw, const Camera& cam, std::vector<u32>& out_visible);

} // namespace nf::rendering
