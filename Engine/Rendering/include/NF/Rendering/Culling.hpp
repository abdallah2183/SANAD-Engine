#pragma once

#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/Camera.hpp>

#include <vector>

namespace nf::rendering {

// Frustum culling for AABB and sphere
bool is_visible(const AABB& aabb, const Frustum& frustum);
bool is_visible(const BoundingSphere& sphere, const Frustum& frustum);

// Transform an AABB / sphere by a translation only. Kept because it is exact
// and branch-free for the many call sites whose transform genuinely has no
// rotation or scale (a root with identity rotation, a test fixture), and
// because removing it would have churned every caller for no gain.
AABB transform_aabb(const AABB& local, float world_x, float world_y, float world_z);
BoundingSphere transform_sphere(const BoundingSphere& local, float world_x, float world_y, float world_z);

// The real ones: transform the volume by a full TRS matrix. The AABB re-fits
// around the 8 transformed corners (the only correct answer, and conservative),
// and the sphere scales by the largest axis (exact for uniform scale). These
// are what extraction, culling, picking and the editor's bounds all use — the
// translation-only overloads under-report the extent of every rotated or scaled
// object, which silently culls things that are on screen and ray-tests the
// wrong volume.
AABB transform_aabb(const AABB& local, const Mat4& m);
BoundingSphere transform_sphere(const BoundingSphere& local, const Mat4& m);

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
