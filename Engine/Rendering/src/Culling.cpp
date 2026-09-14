#include <NF/Rendering/Culling.hpp>

namespace nf::rendering {

bool is_visible(const AABB& aabb, const Frustum& frustum) {
    return frustum.contains_aabb(aabb.min_x, aabb.min_y, aabb.min_z, aabb.max_x, aabb.max_y, aabb.max_z);
}

bool is_visible(const BoundingSphere& sphere, const Frustum& frustum) {
    return frustum.contains_sphere(sphere.cx, sphere.cy, sphere.cz, sphere.radius);
}

AABB transform_aabb(const AABB& local, float world_x, float world_y, float world_z) {
    AABB w;
    w.min_x = local.min_x + world_x;
    w.min_y = local.min_y + world_y;
    w.min_z = local.min_z + world_z;
    w.max_x = local.max_x + world_x;
    w.max_y = local.max_y + world_y;
    w.max_z = local.max_z + world_z;
    return w;
}

BoundingSphere transform_sphere(const BoundingSphere& local, float world_x, float world_y, float world_z) {
    BoundingSphere w = local;
    w.cx += world_x;
    w.cy += world_y;
    w.cz += world_z;
    return w;
}

void cull_render_world(const RenderWorld& rw, const Frustum& frustum, std::vector<u32>& out_visible) {
    out_visible.clear();
    out_visible.reserve(rw.objects.size());
    for (size_t i = 0; i < rw.objects.size(); ++i) {
        const RenderObject& ro = rw.objects[i];
        if (!ro.visible) continue;
        // Sphere first (cheap, conservative); a passing AABB rescues objects
        // whose sphere over-covers a frustum corner. Anything that fails both
        // is outside the view and never reaches draw submission.
        if (is_visible(ro.sphere, frustum) || is_visible(ro.bounds, frustum)) {
            out_visible.push_back(static_cast<u32>(i));
        }
    }
}

void cull_render_world(const RenderWorld& rw, const Camera& cam, std::vector<u32>& out_visible) {
    cull_render_world(rw, cam.frustum, out_visible);
}

} // namespace nf::rendering
