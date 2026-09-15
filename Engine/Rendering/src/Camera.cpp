#include <NF/Rendering/Camera.hpp>

#include <cmath>
#include <algorithm>

namespace nf::rendering {

static void normalize_plane(Plane& p) {
    float len = std::sqrt(p.a*p.a + p.b*p.b + p.c*p.c);
    if (len > 0) { p.a/=len; p.b/=len; p.c/=len; p.d/=len; }
}

Frustum extract_frustum(const Mat4& vp) {
    // Gribb/Hartmann extraction for ROW-vector clip (clip_j = v . col_j):
    // the planes come from matrix COLUMNS, not rows — col j is
    // (m[0][j], m[1][j], m[2][j], m[3][j]). (For column-vector clip the same
    // derivation reads rows; mixing the two up keeps centered geometry
    // working and breaks exactly the behind/outside rejection cases.)
    // Depth is Vulkan [0, 1], so the near face is zc >= 0 (column 2 alone),
    // not zc >= -wc. Far stays zc <= wc.
    Frustum f;
    // Left: col 3 + col 0
    f.planes[0].a = vp.m[0][3] + vp.m[0][0]; f.planes[0].b = vp.m[1][3] + vp.m[1][0]; f.planes[0].c = vp.m[2][3] + vp.m[2][0]; f.planes[0].d = vp.m[3][3] + vp.m[3][0];
    // Right: col 3 - col 0
    f.planes[1].a = vp.m[0][3] - vp.m[0][0]; f.planes[1].b = vp.m[1][3] - vp.m[1][0]; f.planes[1].c = vp.m[2][3] - vp.m[2][0]; f.planes[1].d = vp.m[3][3] - vp.m[3][0];
    // Bottom: col 3 + col 1
    f.planes[2].a = vp.m[0][3] + vp.m[0][1]; f.planes[2].b = vp.m[1][3] + vp.m[1][1]; f.planes[2].c = vp.m[2][3] + vp.m[2][1]; f.planes[2].d = vp.m[3][3] + vp.m[3][1];
    // Top: col 3 - col 1
    f.planes[3].a = vp.m[0][3] - vp.m[0][1]; f.planes[3].b = vp.m[1][3] - vp.m[1][1]; f.planes[3].c = vp.m[2][3] - vp.m[2][1]; f.planes[3].d = vp.m[3][3] - vp.m[3][1];
    // Near: col 2 (zc >= 0)
    f.planes[4].a = vp.m[0][2]; f.planes[4].b = vp.m[1][2]; f.planes[4].c = vp.m[2][2]; f.planes[4].d = vp.m[3][2];
    // Far: col 3 - col 2 (zc <= wc)
    f.planes[5].a = vp.m[0][3] - vp.m[0][2]; f.planes[5].b = vp.m[1][3] - vp.m[1][2]; f.planes[5].c = vp.m[2][3] - vp.m[2][2]; f.planes[5].d = vp.m[3][3] - vp.m[3][2];
    for (int i=0;i<6;++i) normalize_plane(f.planes[i]);
    return f;
}

bool Frustum::contains_sphere(float x,float y,float z,float radius) const {
    for (int i=0;i<6;++i) {
        if (planes[i].distance(x,y,z) < -radius) return false;
    }
    return true;
}

bool Frustum::contains_aabb(float min_x,float min_y,float min_z,float max_x,float max_y,float max_z) const {
    for (int i=0;i<6;++i){
        const Plane& p = planes[i];
        // Find the positive vertex (most outside)
        float px = p.a > 0 ? max_x : min_x;
        float py = p.b > 0 ? max_y : min_y;
        float pz = p.c > 0 ? max_z : min_z;
        if (p.distance(px,py,pz) < 0) return false;
    }
    return true;
}

void update_camera(Camera& cam) {
    // nf::Mat4 applies row-vector (v' = v * M), so world -> view -> clip
    // composes as view * projection. (The deleted column-major pair applied
    // column-vector and composed projection * view; same transform, and the
    // frustum/upload tests below pin that nothing drifted.)
    cam.view = Mat4::look_at(cam.position, cam.target, cam.up);
    if (cam.type == Camera::ProjectionType::Perspective) {
        cam.projection = Mat4::perspective(cam.fov_y_rad, cam.aspect, cam.near_plane, cam.far_plane);
    } else {
        float hw = cam.ortho_width * 0.5f;
        float hh = cam.ortho_height * 0.5f;
        cam.projection = Mat4::orthographic(-hw, hw, -hh, hh, cam.near_plane, cam.far_plane);
    }
    cam.view_projection = cam.view * cam.projection;
    cam.frustum = extract_frustum(cam.view_projection);
}

} // namespace nf::rendering
