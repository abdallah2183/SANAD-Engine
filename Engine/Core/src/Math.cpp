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

Quat Quat::inverse() const {
    const f32 n = length_sq();
    if (n <= EPSILON) {
        return identity();
    }
    const f32 inv = 1.0f / n;
    return {-x * inv, -y * inv, -z * inv, w * inv};
}

Vec3 Quat::rotate(const Vec3& v) const {
    // v' = q v q*, expanded as  v + w*t + qv x t  with  t = 2*(qv x v).
    // Algebraically identical to two quaternion multiplications for a unit
    // quaternion, at roughly half the cost.
    const Vec3 qv{x, y, z};
    const Vec3 t = qv.cross(v) * 2.0f;
    return v + t * w + qv.cross(t);
}

Quat Quat::from_matrix(const Mat4& m) {
    // Mat4 is column-major: m[col][row]. Reading the rotation out of it needs
    // the row/column indices flipped, which is the easiest thing to get wrong
    // here — a transposed read produces the inverse rotation and looks almost
    // right on a symmetric test case.
    const f32 r00 = m.m[0][0], r01 = m.m[1][0], r02 = m.m[2][0];
    const f32 r10 = m.m[0][1], r11 = m.m[1][1], r12 = m.m[2][1];
    const f32 r20 = m.m[0][2], r21 = m.m[1][2], r22 = m.m[2][2];

    // Shepperd's method: pick the branch with the largest denominator so the
    // division never amplifies rounding error. The naive trace-only form is
    // accurate near identity and badly wrong near 180 degrees, which is exactly
    // where a physics body that flipped over ends up.
    const f32 trace = r00 + r11 + r22;
    Quat q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f; // 4w
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const f32 s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f; // 4x
        q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const f32 s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f; // 4y
        q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f; // 4z
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    return q.normalized();
}

Quat Quat::slerp(const Quat& a, const Quat& b, f32 t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    f32 cos_theta = a.dot(b);
    Quat b_adj = b;
    // Take the shortest path: if the dot is negative, negate one quaternion.
    if (cos_theta < 0.0f) {
        b_adj = {-b.x, -b.y, -b.z, -b.w};
        cos_theta = -cos_theta;
    }

    // If the quaternions are very close, slerp degenerates — fall back to nlerp
    // to avoid dividing by a near-zero sin(angle).
    if (cos_theta > 0.9995f) {
        return Quat{
            a.x + (b_adj.x - a.x) * t,
            a.y + (b_adj.y - a.y) * t,
            a.z + (b_adj.z - a.z) * t,
            a.w + (b_adj.w - a.w) * t,
        }.normalized();
    }

    f32 theta = std::acos(cos_theta);
    f32 sin_theta = std::sin(theta);
    f32 w0 = std::sin((1.0f - t) * theta) / sin_theta;
    f32 w1 = std::sin(t * theta) / sin_theta;

    return {
        a.x * w0 + b_adj.x * w1,
        a.y * w0 + b_adj.y * w1,
        a.z * w0 + b_adj.z * w1,
        a.w * w0 + b_adj.w * w1,
    };
}

Quat Quat::nlerp(const Quat& a, const Quat& b, f32 t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    Quat b_adj = b;
    if (a.dot(b) < 0.0f) {
        b_adj = {-b.x, -b.y, -b.z, -b.w};
    }

    return Quat{
        a.x + (b_adj.x - a.x) * t,
        a.y + (b_adj.y - a.y) * t,
        a.z + (b_adj.z - a.z) * t,
        a.w + (b_adj.w - a.w) * t,
    }.normalized();
}

} // namespace nf
