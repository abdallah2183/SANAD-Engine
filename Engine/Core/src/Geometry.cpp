#include <NF/Core/Geometry.hpp>

#include <cmath>
#include <limits>

namespace nf {

// --- Plane ---

Plane Plane::from_point_and_normal(const Vec3& pt, const Vec3& n) {
    Vec3 norm = n.normalized();
    return Plane(norm, -norm.dot(pt));
}

void Plane::normalize() {
    f32 len = normal.length();
    if (len > 1e-6f) {
        f32 inv = 1.0f / len;
        normal = normal * inv;
        distance *= inv;
    }
}

Plane Plane::normalized() const {
    Plane p = *this;
    p.normalize();
    return p;
}

// --- AABB ---

AABB AABB::from_center_extents(const Vec3& center, const Vec3& extents) {
    return AABB(center - extents, center + extents);
}

AABB AABB::transformed(const Mat4& m) const {
    if (!is_valid()) return *this;

    const Vec3 corners[8] = {
        Vec3(min.x, min.y, min.z),
        Vec3(max.x, min.y, min.z),
        Vec3(min.x, max.y, min.z),
        Vec3(max.x, max.y, min.z),
        Vec3(min.x, min.y, max.z),
        Vec3(max.x, min.y, max.z),
        Vec3(min.x, max.y, max.z),
        Vec3(max.x, max.y, max.z),
    };

    AABB result;
    for (int i = 0; i < 8; ++i) {
        result.expand(m.transform_point(corners[i]));
    }
    return result;
}

// --- BoundingSphere ---

bool BoundingSphere::intersects(const AABB& box) const {
    if (!box.is_valid()) return false;
    const f32 cx = std::clamp(center.x, box.min.x, box.max.x);
    const f32 cy = std::clamp(center.y, box.min.y, box.max.y);
    const f32 cz = std::clamp(center.z, box.min.z, box.max.z);
    const Vec3 closest(cx, cy, cz);
    return (closest - center).length_sq() <= (radius * radius);
}

// --- Ray ---

Ray::Ray(const Vec3& orig, const Vec3& dir)
    : origin(orig), direction(dir.normalized()) {}

bool Ray::intersects(const AABB& box, f32& out_t) const {
    if (!box.is_valid()) return false;

    f32 t_min = 0.0f;
    f32 t_max = std::numeric_limits<f32>::infinity();

    for (int i = 0; i < 3; ++i) {
        f32 o = (i == 0) ? origin.x : ((i == 1) ? origin.y : origin.z);
        f32 d = (i == 0) ? direction.x : ((i == 1) ? direction.y : direction.z);
        f32 b_min = (i == 0) ? box.min.x : ((i == 1) ? box.min.y : box.min.z);
        f32 b_max = (i == 0) ? box.max.x : ((i == 1) ? box.max.y : box.max.z);

        if (std::abs(d) < 1e-7f) {
            // Ray parallel to slab; check if origin is outside
            if (o < b_min || o > b_max) return false;
        } else {
            f32 inv_d = 1.0f / d;
            f32 t1 = (b_min - o) * inv_d;
            f32 t2 = (b_max - o) * inv_d;
            if (t1 > t2) std::swap(t1, t2);

            t_min = std::max(t_min, t1);
            t_max = std::min(t_max, t2);

            if (t_min > t_max) return false;
        }
    }

    out_t = (t_min >= 0.0f) ? t_min : t_max;
    return out_t >= 0.0f;
}

bool Ray::intersects(const BoundingSphere& sphere, f32& out_t) const {
    const Vec3 m = origin - sphere.center;
    const f32 b = m.dot(direction);
    const f32 c = m.dot(m) - sphere.radius * sphere.radius;

    // Ray origin outside sphere and pointing away
    if (c > 0.0f && b > 0.0f) return false;

    const f32 discr = b * b - c;
    if (discr < 0.0f) return false;

    f32 t = -b - std::sqrt(discr);
    if (t < 0.0f) t = -b + std::sqrt(discr);
    if (t < 0.0f) return false;

    out_t = t;
    return true;
}

bool Ray::intersects_triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
                             f32& out_t, Vec2& out_barycentric) const {
    const Vec3 edge1 = v1 - v0;
    const Vec3 edge2 = v2 - v0;
    const Vec3 pvec = direction.cross(edge2);
    const f32 det = edge1.dot(pvec);

    if (std::abs(det) < 1e-7f) return false;
    const f32 inv_det = 1.0f / det;

    const Vec3 tvec = origin - v0;
    const f32 u = tvec.dot(pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;

    const Vec3 qvec = tvec.cross(edge1);
    const f32 v = direction.dot(qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;

    const f32 t = edge2.dot(qvec) * inv_det;
    if (t < 0.0f) return false;

    out_t = t;
    out_barycentric = Vec2(u, v);
    return true;
}

// --- Frustum ---

Frustum Frustum::from_view_projection(const Mat4& vp) {
    Frustum f;

    // View-projection matrix in SANAD Engine row-major format m[row][col]:
    // Vectors multiply on left: v * M
    // Row 0 = col 0 of standard projection, etc.
    const auto& m = vp.m;

    // Left:   col3 + col0 (or row3 + row0 in row-vector form)
    f.planes[Left]   = Plane(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);
    // Right:  col3 - col0
    f.planes[Right]  = Plane(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);
    // Bottom: col3 + col1
    f.planes[Bottom] = Plane(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);
    // Top:    col3 - col1
    f.planes[Top]    = Plane(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);
    // Near:   col2 (Vulkan depth [0, 1])
    f.planes[Near]   = Plane(m[0][2], m[1][2], m[2][2], m[3][2]);
    // Far:    col3 - col2
    f.planes[Far]    = Plane(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);

    for (int i = 0; i < PlaneCount; ++i) {
        f.planes[i].normalize();
    }

    return f;
}

bool Frustum::contains_point(const Vec3& pt) const {
    for (int i = 0; i < PlaneCount; ++i) {
        if (planes[i].signed_distance(pt) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersects_sphere(const BoundingSphere& sphere) const {
    for (int i = 0; i < PlaneCount; ++i) {
        if (planes[i].signed_distance(sphere.center) < -sphere.radius) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersects_aabb(const AABB& box) const {
    if (!box.is_valid()) return false;

    for (int i = 0; i < PlaneCount; ++i) {
        const Plane& p = planes[i];
        // Positive vertex: the box vertex furthest along the plane normal
        const Vec3 pos_vertex(
            (p.normal.x >= 0.0f) ? box.max.x : box.min.x,
            (p.normal.y >= 0.0f) ? box.max.y : box.min.y,
            (p.normal.z >= 0.0f) ? box.max.z : box.min.z
        );

        if (p.signed_distance(pos_vertex) < 0.0f) {
            return false; // Entire box is on the negative (outside) side of this plane
        }
    }
    return true;
}

} // namespace nf
