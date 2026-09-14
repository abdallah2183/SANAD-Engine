// NF/Core/Math.cpp — Math implementation

#include <NF/Core/Math.hpp>

#include <cstring>

namespace nf {

// --- Mat4 ---

Mat4 Mat4::translate(const Vec3& v) {
    Mat4 r = identity();
    r.m[3][0] = v.x;
    r.m[3][1] = v.y;
    r.m[3][2] = v.z;
    return r;
}

Mat4 Mat4::scale(const Vec3& v) {
    Mat4 r = identity();
    r.m[0][0] = v.x;
    r.m[1][1] = v.y;
    r.m[2][2] = v.z;
    return r;
}

Mat4 Mat4::rotate_x(f32 rad) {
    f32 c = std::cos(rad), s = std::sin(rad);
    Mat4 r = identity();
    r.m[1][1] = c;  r.m[1][2] = s;
    r.m[2][1] = -s; r.m[2][2] = c;
    return r;
}

Mat4 Mat4::rotate_y(f32 rad) {
    f32 c = std::cos(rad), s = std::sin(rad);
    Mat4 r = identity();
    r.m[0][0] = c;  r.m[0][2] = -s;
    r.m[2][0] = s;  r.m[2][2] = c;
    return r;
}

Mat4 Mat4::rotate_z(f32 rad) {
    f32 c = std::cos(rad), s = std::sin(rad);
    Mat4 r = identity();
    r.m[0][0] = c;  r.m[0][1] = s;
    r.m[1][0] = -s; r.m[1][1] = c;
    return r;
}

Mat4 Mat4::rotation(const Vec3& axis, f32 angle) {
    Vec3 n = axis.normalized();
    f32 c = std::cos(angle);
    f32 s = std::sin(angle);
    f32 t = 1.0f - c;

    Mat4 r = identity();
    r.m[0][0] = t * n.x * n.x + c;
    r.m[0][1] = t * n.x * n.y + s * n.z;
    r.m[0][2] = t * n.x * n.z - s * n.y;

    r.m[1][0] = t * n.x * n.y - s * n.z;
    r.m[1][1] = t * n.y * n.y + c;
    r.m[1][2] = t * n.y * n.z + s * n.x;

    r.m[2][0] = t * n.x * n.z + s * n.y;
    r.m[2][1] = t * n.y * n.z - s * n.x;
    r.m[2][2] = t * n.z * n.z + c;
    return r;
}

Mat4 Mat4::perspective(f32 fovy, f32 aspect, f32 near_z, f32 far_z) {
    // Right-handed, Y up, depth range [0, 1]
    f32 f = 1.0f / std::tan(fovy * 0.5f);
    Mat4 r{};

    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = far_z / (far_z - near_z);
    r.m[2][3] = 1.0f;
    r.m[3][2] = -(near_z * far_z) / (far_z - near_z);
    return r;
}

Mat4 Mat4::orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z) {
    Mat4 r = identity();
    r.m[0][0] = 2.0f / (right - left);
    r.m[1][1] = 2.0f / (top - bottom);
    r.m[2][2] = 1.0f / (far_z - near_z);
    r.m[3][0] = -(left + right) / (right - left);
    r.m[3][1] = -(top + bottom) / (top - bottom);
    r.m[3][2] = -near_z / (far_z - near_z);
    return r;
}

Mat4 Mat4::look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    Vec3 f = (center - eye).normalized();
    Vec3 s = f.cross(up).normalized();
    Vec3 u = s.cross(f);

    Mat4 r = identity();
    r.m[0][0] = s.x;  r.m[0][1] = s.y;  r.m[0][2] = s.z;
    r.m[1][0] = u.x;  r.m[1][1] = u.y;  r.m[2][2] = u.z;
    r.m[2][0] = -f.x; r.m[2][1] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -s.dot(eye);
    r.m[3][1] = -u.dot(eye);
    r.m[3][2] = f.dot(eye);
    return r;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = m[i][0] * o.m[0][j] +
                        m[i][1] * o.m[1][j] +
                        m[i][2] * o.m[2][j] +
                        m[i][3] * o.m[3][j];
        }
    }
    return r;
}

Vec4 Mat4::operator*(const Vec4& v) const {
    return {
        v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + v.w * m[3][0],
        v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + v.w * m[3][1],
        v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + v.w * m[3][2],
        v.x * m[0][3] + v.y * m[1][3] + v.z * m[2][3] + v.w * m[3][3]
    };
}

Vec3 Mat4::transform_point(const Vec3& v) const {
    Vec4 r = *this * Vec4(v, 1.0f);
    if (std::abs(r.w) > EPSILON) {
        return Vec3(r.x / r.w, r.y / r.w, r.z / r.w);
    }
    return Vec3(r.x, r.y, r.z);
}

Vec3 Mat4::transform_direction(const Vec3& v) const {
    Vec4 r = *this * Vec4(v, 0.0f);
    return Vec3(r.x, r.y, r.z);
}

Mat4 Mat4::transposed() const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[j][i];
    return r;
}

Mat4 Mat4::inverse() const {
    // General 4x4 inverse via cofactors
    f32 inv[16];
    f32 det = 0;

    const f32* m_flat = &m[0][0];

    inv[0] = m_flat[5]  * m_flat[10] * m_flat[15] -
             m_flat[5]  * m_flat[11] * m_flat[14] -
             m_flat[9]  * m_flat[6]  * m_flat[15] +
             m_flat[9]  * m_flat[7]  * m_flat[14] +
             m_flat[13] * m_flat[6]  * m_flat[11] -
             m_flat[13] * m_flat[7]  * m_flat[10];

    inv[4] = -m_flat[4]  * m_flat[10] * m_flat[15] +
              m_flat[4]  * m_flat[11] * m_flat[14] +
              m_flat[8]  * m_flat[6]  * m_flat[15] -
              m_flat[8]  * m_flat[7]  * m_flat[14] -
              m_flat[12] * m_flat[6]  * m_flat[11] +
              m_flat[12] * m_flat[7]  * m_flat[10];

    inv[8] = m_flat[4]  * m_flat[9] * m_flat[15] -
             m_flat[4]  * m_flat[11] * m_flat[13] -
             m_flat[8]  * m_flat[5] * m_flat[15] +
             m_flat[8]  * m_flat[7] * m_flat[13] +
             m_flat[12] * m_flat[5] * m_flat[11] -
             m_flat[12] * m_flat[7] * m_flat[9];

    inv[12] = -m_flat[4]  * m_flat[9] * m_flat[14] +
               m_flat[4]  * m_flat[10] * m_flat[13] +
               m_flat[8]  * m_flat[5] * m_flat[14] -
               m_flat[8]  * m_flat[6] * m_flat[13] -
               m_flat[12] * m_flat[5] * m_flat[10] +
               m_flat[12] * m_flat[6] * m_flat[9];

    det = m_flat[0] * inv[0] + m_flat[1] * inv[4] + m_flat[2] * inv[8] + m_flat[3] * inv[12];

    if (std::abs(det) < EPSILON) {
        return identity();
    }

    f32 inv_det = 1.0f / det;

    inv[1] = -m_flat[1]  * m_flat[10] * m_flat[15] +
              m_flat[1]  * m_flat[11] * m_flat[14] +
              m_flat[9]  * m_flat[2] * m_flat[15] -
              m_flat[9]  * m_flat[3] * m_flat[14] -
              m_flat[13] * m_flat[2] * m_flat[11] +
              m_flat[13] * m_flat[3] * m_flat[10];

    inv[5] = m_flat[0]  * m_flat[10] * m_flat[15] -
             m_flat[0]  * m_flat[11] * m_flat[14] -
             m_flat[8]  * m_flat[2] * m_flat[15] +
             m_flat[8]  * m_flat[3] * m_flat[14] +
             m_flat[12] * m_flat[2] * m_flat[11] -
             m_flat[12] * m_flat[3] * m_flat[10];

    inv[9] = -m_flat[0]  * m_flat[9] * m_flat[15] +
              m_flat[0]  * m_flat[11] * m_flat[13] +
              m_flat[8]  * m_flat[1] * m_flat[15] -
              m_flat[8]  * m_flat[3] * m_flat[13] -
              m_flat[12] * m_flat[1] * m_flat[11] +
              m_flat[12] * m_flat[3] * m_flat[9];

    inv[13] = m_flat[0]  * m_flat[9] * m_flat[14] -
              m_flat[0]  * m_flat[10] * m_flat[13] -
              m_flat[8]  * m_flat[1] * m_flat[14] +
              m_flat[8]  * m_flat[2] * m_flat[13] +
              m_flat[12] * m_flat[1] * m_flat[10] -
              m_flat[12] * m_flat[2] * m_flat[9];

    inv[2] = m_flat[1]  * m_flat[6] * m_flat[15] -
             m_flat[1]  * m_flat[7] * m_flat[14] -
             m_flat[5]  * m_flat[2] * m_flat[15] +
             m_flat[5]  * m_flat[3] * m_flat[14] +
             m_flat[13] * m_flat[2] * m_flat[7] -
             m_flat[13] * m_flat[3] * m_flat[6];

    inv[6] = -m_flat[0]  * m_flat[6] * m_flat[15] +
              m_flat[0]  * m_flat[7] * m_flat[14] +
              m_flat[4]  * m_flat[2] * m_flat[15] -
              m_flat[4]  * m_flat[3] * m_flat[14] -
              m_flat[12] * m_flat[2] * m_flat[7] +
              m_flat[12] * m_flat[3] * m_flat[6];

    inv[10] = m_flat[0]  * m_flat[5] * m_flat[15] -
              m_flat[0]  * m_flat[7] * m_flat[13] -
              m_flat[4]  * m_flat[1] * m_flat[15] +
              m_flat[4]  * m_flat[3] * m_flat[13] +
              m_flat[12] * m_flat[1] * m_flat[7] -
              m_flat[12] * m_flat[3] * m_flat[5];

    inv[14] = -m_flat[0]  * m_flat[5] * m_flat[14] +
               m_flat[0]  * m_flat[6] * m_flat[13] +
               m_flat[4]  * m_flat[1] * m_flat[14] -
               m_flat[4]  * m_flat[2] * m_flat[13] -
               m_flat[12] * m_flat[1] * m_flat[6] +
               m_flat[12] * m_flat[2] * m_flat[5];

    inv[3] = -m_flat[1] * m_flat[6] * m_flat[11] +
              m_flat[1] * m_flat[7] * m_flat[10] +
              m_flat[5] * m_flat[2] * m_flat[11] -
              m_flat[5] * m_flat[3] * m_flat[10] -
              m_flat[9] * m_flat[2] * m_flat[7] +
              m_flat[9] * m_flat[3] * m_flat[6];

    inv[7] = m_flat[0] * m_flat[6] * m_flat[11] -
             m_flat[0] * m_flat[7] * m_flat[10] -
             m_flat[4] * m_flat[2] * m_flat[11] +
             m_flat[4] * m_flat[3] * m_flat[10] +
             m_flat[8] * m_flat[2] * m_flat[7] -
             m_flat[8] * m_flat[3] * m_flat[6];

    inv[11] = -m_flat[0] * m_flat[5] * m_flat[11] +
               m_flat[0] * m_flat[7] * m_flat[9] +
               m_flat[4] * m_flat[1] * m_flat[11] -
               m_flat[4] * m_flat[3] * m_flat[9] -
               m_flat[8] * m_flat[1] * m_flat[7] +
               m_flat[8] * m_flat[3] * m_flat[5];

    inv[15] = m_flat[0] * m_flat[5] * m_flat[10] -
              m_flat[0] * m_flat[6] * m_flat[9] -
              m_flat[4] * m_flat[1] * m_flat[10] +
              m_flat[4] * m_flat[2] * m_flat[9] +
              m_flat[8] * m_flat[1] * m_flat[6] -
              m_flat[8] * m_flat[2] * m_flat[5];

    Mat4 r;
    for (int i = 0; i < 16; ++i) {
        (&r.m[0][0])[i] = inv[i] * inv_det;
    }
    return r;
}

// --- Quat ---

Quat Quat::from_axis_angle(const Vec3& axis, f32 angle) {
    Vec3 n = axis.normalized();
    f32 half = angle * 0.5f;
    f32 s = std::sin(half);
    return {n.x * s, n.y * s, n.z * s, std::cos(half)};
}

Quat Quat::from_euler(f32 pitch, f32 yaw, f32 roll) {
    f32 cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    f32 cy = std::cos(yaw * 0.5f),   sy = std::sin(yaw * 0.5f);
    f32 cr = std::cos(roll * 0.5f),  sr = std::sin(roll * 0.5f);

    return {
        sp * cy * cr - cp * sy * sr,
        cp * sy * cr + sp * cy * sr,
        cp * cy * sr - sp * sy * cr,
        cp * sy * sr + sp * cy * cr
    };
}

Quat Quat::operator*(const Quat& o) const {
    return {
        w * o.x + x * o.w + y * o.z - z * o.y,
        w * o.y - x * o.z + y * o.w + z * o.x,
        w * o.z + x * o.y - y * o.x + z * o.w,
        w * o.w - x * o.x - y * o.y - z * o.z
    };
}

Quat Quat::normalized() const {
    f32 len = std::sqrt(dot(*this));
    return len > EPSILON ? Quat{x/len, y/len, z/len, w/len} : *this;
}

f32 Quat::dot(const Quat& o) const {
    return x*o.x + y*o.y + z*o.z + w*o.w;
}

Mat4 Quat::to_matrix() const {
    Quat n = normalized();
    f32 xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
    f32 xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
    f32 wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;

    Mat4 r = Mat4::identity();
    r.m[0][0] = 1 - 2*(yy+zz); r.m[0][1] = 2*(xy+wz);  r.m[0][2] = 2*(xz-wy);
    r.m[1][0] = 2*(xy-wz);     r.m[1][1] = 1-2*(xx+zz); r.m[1][2] = 2*(yz+wx);
    r.m[2][0] = 2*(xz+wy);     r.m[2][1] = 2*(yz-wx);  r.m[2][2] = 1-2*(xx+yy);
    return r;
}

} // namespace nf
