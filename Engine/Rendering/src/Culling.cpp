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

AABB transform_aabb(const AABB& local, const Mat4& m) {
    // Transform all 8 corners and re-fit the box around them. An AABB under a
    // rotation is NOT the rotated AABB — a cube turned 45 degrees has an
    // axis-aligned box sqrt(2) times wider on each diagonal. Re-fitting is the
    // only correct answer, and it is the CONSERVATIVE one: the box grows, never
    // shrinks, so nothing is ever wrongly culled.
    //
    // The previous translation-only form under-reported the extent of every
    // rotated or scaled object, which meant culling threw away objects that were
    // plainly on screen (a 45-degree plane vanishing at the screen edge) and
    // picking ray-tested the wrong volume.
    AABB w;
    bool first = true;
    for (int c = 0; c < 8; ++c) {
        const Vec3 corner{(c & 1) ? local.max_x : local.min_x,
                          (c & 2) ? local.max_y : local.min_y,
                          (c & 4) ? local.max_z : local.min_z};
        // nf::Mat4 is row-vector: v' = v * M.
        const Vec3 p{m.m[0][0] * corner.x + m.m[1][0] * corner.y + m.m[2][0] * corner.z + m.m[3][0],
                     m.m[0][1] * corner.x + m.m[1][1] * corner.y + m.m[2][1] * corner.z + m.m[3][1],
                     m.m[0][2] * corner.x + m.m[1][2] * corner.y + m.m[2][2] * corner.z + m.m[3][2]};
        if (first) {
            w.min_x = p.x;
            w.min_y = p.y;
            w.min_z = p.z;
            w.max_x = p.x;
            w.max_y = p.y;
            w.max_z = p.z;
            first = false;
            continue;
        }
        w.min_x = std::min(w.min_x, p.x);
        w.min_y = std::min(w.min_y, p.y);
        w.min_z = std::min(w.min_z, p.z);
        w.max_x = std::max(w.max_x, p.x);
        w.max_y = std::max(w.max_y, p.y);
        w.max_z = std::max(w.max_z, p.z);
    }
    return w;
}

BoundingSphere transform_sphere(const BoundingSphere& local, const Mat4& m) {
    // Centre through the full matrix; radius by the LARGEST axis scale. A
    // uniform scale is exact, a non-uniform one is conservative (the sphere has
    // to contain the box, and the box's largest axis is the tight bound), and
    // the rotation contributes nothing because it cannot lengthen a vector.
    BoundingSphere w;
    const Vec3 c{m.m[0][0] * local.cx + m.m[1][0] * local.cy + m.m[2][0] * local.cz + m.m[3][0],
                 m.m[0][1] * local.cx + m.m[1][1] * local.cy + m.m[2][1] * local.cz + m.m[3][1],
                 m.m[0][2] * local.cx + m.m[1][2] * local.cy + m.m[2][2] * local.cz + m.m[3][2]};
    const Vec3 axis_x{m.m[0][0], m.m[0][1], m.m[0][2]};
    const Vec3 axis_y{m.m[1][0], m.m[1][1], m.m[1][2]};
    const Vec3 axis_z{m.m[2][0], m.m[2][1], m.m[2][2]};
    const float sx = std::sqrt(axis_x.x * axis_x.x + axis_x.y * axis_x.y + axis_x.z * axis_x.z);
    const float sy = std::sqrt(axis_y.x * axis_y.x + axis_y.y * axis_y.y + axis_y.z * axis_y.z);
    const float sz = std::sqrt(axis_z.x * axis_z.x + axis_z.y * axis_z.y + axis_z.z * axis_z.z);
    w.cx = c.x;
    w.cy = c.y;
    w.cz = c.z;
    w.radius = local.radius * std::max(sx, std::max(sy, sz));
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
