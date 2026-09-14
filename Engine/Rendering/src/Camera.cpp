#include <NF/Rendering/Camera.hpp>

#include <cmath>
#include <algorithm>

namespace nf::rendering {

Mat4 Mat4::identity() {
    Mat4 r;
    r.m[0]=1; r.m[5]=1; r.m[10]=1; r.m[15]=1;
    return r;
}

Mat4 Mat4::perspective(float fov_y_rad, float aspect, float near_plane, float far_plane) {
    Mat4 r{};
    float f = 1.0f / std::tan(fov_y_rad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
    // r.m[15] = 0 already
    return r;
}

Mat4 Mat4::orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane) {
    Mat4 r = identity();
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = -2.0f / (far_plane - near_plane);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(far_plane + near_plane) / (far_plane - near_plane);
    return r;
}

Mat4 Mat4::look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    Vec3 f{center.x - eye.x, center.y - eye.y, center.z - eye.z};
    float flen = std::sqrt(f.x*f.x + f.y*f.y + f.z*f.z);
    if (flen > 0) { f.x/=flen; f.y/=flen; f.z/=flen; }
    Vec3 s{ f.y*up.z - f.z*up.y, f.z*up.x - f.x*up.z, f.x*up.y - f.y*up.x };
    float slen = std::sqrt(s.x*s.x + s.y*s.y + s.z*s.z);
    if (slen > 0) { s.x/=slen; s.y/=slen; s.z/=slen; }
    Vec3 u{ s.y*f.z - s.z*f.y, s.z*f.x - s.x*f.z, s.x*f.y - s.y*f.x };

    Mat4 r = identity();
    r.m[0]= s.x; r.m[4]= s.y; r.m[8]= s.z;
    r.m[1]= u.x; r.m[5]= u.y; r.m[9]= u.z;
    r.m[2]=-f.x; r.m[6]=-f.y; r.m[10]=-f.z;
    r.m[12]= -(s.x*eye.x + s.y*eye.y + s.z*eye.z);
    r.m[13]= -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
    r.m[14]=  (f.x*eye.x + f.y*eye.y + f.z*eye.z);
    return r;
}

Mat4 Mat4::translation(float x, float y, float z) {
    Mat4 r = identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 Mat4::inverse() const {
    // General cofactor inverse. Indexing is row-major on the math convention
    // used throughout this file (m[col*4 + row]).
    const float* e = m;
    float inv[16];

    inv[0] =  e[5]*e[10]*e[15] - e[5]*e[11]*e[14] - e[9]*e[6]*e[15] + e[9]*e[7]*e[14] + e[13]*e[6]*e[11] - e[13]*e[7]*e[10];
    inv[4] = -e[4]*e[10]*e[15] + e[4]*e[11]*e[14] + e[8]*e[6]*e[15] - e[8]*e[7]*e[14] - e[12]*e[6]*e[11] + e[12]*e[7]*e[10];
    inv[8] =  e[4]*e[9]*e[15] - e[4]*e[11]*e[13] - e[8]*e[5]*e[15] + e[8]*e[7]*e[13] + e[12]*e[5]*e[11] - e[12]*e[7]*e[9];
    inv[12] = -e[4]*e[9]*e[14] + e[4]*e[10]*e[13] + e[8]*e[5]*e[14] - e[8]*e[6]*e[13] - e[12]*e[5]*e[10] + e[12]*e[6]*e[9];

    inv[1] = -e[1]*e[10]*e[15] + e[1]*e[11]*e[14] + e[9]*e[2]*e[15] - e[9]*e[3]*e[14] - e[13]*e[2]*e[11] + e[13]*e[3]*e[10];
    inv[5] =  e[0]*e[10]*e[15] - e[0]*e[11]*e[14] - e[8]*e[2]*e[15] + e[8]*e[3]*e[14] + e[12]*e[2]*e[11] - e[12]*e[3]*e[10];
    inv[9] = -e[0]*e[9]*e[15] + e[0]*e[11]*e[13] + e[8]*e[1]*e[15] - e[8]*e[3]*e[13] - e[12]*e[1]*e[11] + e[12]*e[3]*e[9];
    inv[13] = e[0]*e[9]*e[14] - e[0]*e[10]*e[13] - e[8]*e[1]*e[14] + e[8]*e[2]*e[13] + e[12]*e[1]*e[10] - e[12]*e[2]*e[9];

    inv[2] =  e[1]*e[6]*e[15] - e[1]*e[7]*e[14] - e[5]*e[2]*e[15] + e[5]*e[3]*e[14] + e[13]*e[2]*e[7] - e[13]*e[3]*e[6];
    inv[6] = -e[0]*e[6]*e[15] + e[0]*e[7]*e[14] + e[4]*e[2]*e[15] - e[4]*e[3]*e[14] - e[12]*e[2]*e[7] + e[12]*e[3]*e[6];
    inv[10] = e[0]*e[5]*e[15] - e[0]*e[7]*e[13] - e[4]*e[1]*e[15] + e[4]*e[3]*e[13] + e[12]*e[1]*e[7] - e[12]*e[3]*e[5];
    inv[14] = -e[0]*e[5]*e[14] + e[0]*e[6]*e[13] + e[4]*e[1]*e[14] - e[4]*e[2]*e[13] - e[12]*e[1]*e[6] + e[12]*e[2]*e[5];

    inv[3] = -e[1]*e[6]*e[11] + e[1]*e[7]*e[10] + e[5]*e[2]*e[11] - e[5]*e[3]*e[10] - e[9]*e[2]*e[7] + e[9]*e[3]*e[6];
    inv[7] =  e[0]*e[6]*e[11] - e[0]*e[7]*e[10] - e[4]*e[2]*e[11] + e[4]*e[3]*e[10] + e[8]*e[2]*e[7] - e[8]*e[3]*e[6];
    inv[11] = -e[0]*e[5]*e[11] + e[0]*e[7]*e[9] + e[4]*e[1]*e[11] - e[4]*e[3]*e[9] - e[8]*e[1]*e[7] + e[8]*e[3]*e[5];
    inv[15] = e[0]*e[5]*e[10] - e[0]*e[6]*e[9] - e[4]*e[1]*e[10] + e[4]*e[2]*e[9] + e[8]*e[1]*e[6] - e[8]*e[2]*e[5];

    float det = e[0]*inv[0] + e[1]*inv[4] + e[2]*inv[8] + e[3]*inv[12];
    if (std::fabs(det) < 1e-12f) {
        // Singular matrix — return identity rather than NaNs poisoning the
        // frame; callers reconstruct positions with it.
        return identity();
    }
    const float inv_det = 1.0f / det;
    Mat4 r{};
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * inv_det;
    return r;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r{};
    for (int c=0;c<4;++c) for (int r_ =0;r_<4;++r_) {
        float sum=0;
        for (int k=0;k<4;++k) sum += m[k*4 + r_] * o.m[c*4 + k];
        r.m[c*4 + r_] = sum;
    }
    return r;
}

static void normalize_plane(Plane& p) {
    float len = std::sqrt(p.a*p.a + p.b*p.b + p.c*p.c);
    if (len > 0) { p.a/=len; p.b/=len; p.c/=len; p.d/=len; }
}

Frustum extract_frustum(const Mat4& vp) {
    Frustum f;
    // Left: row 3 + row 0
    f.planes[0].a = vp.m[3] + vp.m[0]; f.planes[0].b = vp.m[7] + vp.m[4]; f.planes[0].c = vp.m[11] + vp.m[8]; f.planes[0].d = vp.m[15] + vp.m[12];
    // Right: row 3 - row 0
    f.planes[1].a = vp.m[3] - vp.m[0]; f.planes[1].b = vp.m[7] - vp.m[4]; f.planes[1].c = vp.m[11] - vp.m[8]; f.planes[1].d = vp.m[15] - vp.m[12];
    // Bottom: row 3 + row 1
    f.planes[2].a = vp.m[3] + vp.m[1]; f.planes[2].b = vp.m[7] + vp.m[5]; f.planes[2].c = vp.m[11] + vp.m[9]; f.planes[2].d = vp.m[15] + vp.m[13];
    // Top: row 3 - row 1
    f.planes[3].a = vp.m[3] - vp.m[1]; f.planes[3].b = vp.m[7] - vp.m[5]; f.planes[3].c = vp.m[11] - vp.m[9]; f.planes[3].d = vp.m[15] - vp.m[13];
    // Near: row 3 + row 2
    f.planes[4].a = vp.m[3] + vp.m[2]; f.planes[4].b = vp.m[7] + vp.m[6]; f.planes[4].c = vp.m[11] + vp.m[10]; f.planes[4].d = vp.m[15] + vp.m[14];
    // Far: row 3 - row 2
    f.planes[5].a = vp.m[3] - vp.m[2]; f.planes[5].b = vp.m[7] - vp.m[6]; f.planes[5].c = vp.m[11] - vp.m[10]; f.planes[5].d = vp.m[15] - vp.m[14];
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
    cam.view = Mat4::look_at(cam.position, cam.target, cam.up);
    if (cam.type == Camera::ProjectionType::Perspective) {
        cam.projection = Mat4::perspective(cam.fov_y_rad, cam.aspect, cam.near_plane, cam.far_plane);
    } else {
        float hw = cam.ortho_width * 0.5f;
        float hh = cam.ortho_height * 0.5f;
        cam.projection = Mat4::orthographic(-hw, hw, -hh, hh, cam.near_plane, cam.far_plane);
    }
    cam.view_projection = cam.projection * cam.view;
    cam.frustum = extract_frustum(cam.view_projection);
}

} // namespace nf::rendering
