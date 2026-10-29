#pragma once

// NF/Core/Geometry.hpp — 3D Computational Geometry Subsystem for SANAD Engine
// Provides Axis-Aligned Bounding Box (AABB), Bounding Sphere, Ray, Plane,
// and 6-Plane View Frustum with high-performance intersection and culling algorithms.

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace nf {

// Forward declarations
struct AABB;
struct BoundingSphere;
struct Ray;
struct Plane;
struct Frustum;

// =========================================================================
// Plane — Hessian normal form: dot(normal, X) + distance = 0
// =========================================================================
struct Plane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32  distance{0.0f};

    constexpr Plane() = default;
    constexpr Plane(const Vec3& n, f32 d) : normal(n), distance(d) {}
    constexpr Plane(f32 a, f32 b, f32 c, f32 d) : normal(a, b, c), distance(d) {}

    static Plane from_point_and_normal(const Vec3& pt, const Vec3& n);

    void normalize();
    Plane normalized() const;

    f32 signed_distance(const Vec3& pt) const {
        return normal.dot(pt) + distance;
    }
};

// =========================================================================
// AABB — 3D Axis-Aligned Bounding Box
// =========================================================================
struct AABB {
    Vec3 min{1e30f, 1e30f, 1e30f};
    Vec3 max{-1e30f, -1e30f, -1e30f};

    constexpr AABB() = default;
    constexpr AABB(const Vec3& min_pt, const Vec3& max_pt) : min(min_pt), max(max_pt) {}

    static AABB from_center_extents(const Vec3& center, const Vec3& extents);

    bool is_valid() const {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    void reset() {
        min = Vec3(1e30f, 1e30f, 1e30f);
        max = Vec3(-1e30f, -1e30f, -1e30f);
    }

    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extents() const { return (max - min) * 0.5f; }
    Vec3 size() const { return max - min; }
    f32  radius() const { return extents().length(); }

    void expand(const Vec3& point) {
        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    }

    void expand(const AABB& other) {
        if (!other.is_valid()) return;
        min.x = std::min(min.x, other.min.x);
        min.y = std::min(min.y, other.min.y);
        min.z = std::min(min.z, other.min.z);
        max.x = std::max(max.x, other.max.x);
        max.y = std::max(max.y, other.max.y);
        max.z = std::max(max.z, other.max.z);
    }

    bool contains(const Vec3& pt) const {
        return pt.x >= min.x && pt.x <= max.x &&
               pt.y >= min.y && pt.y <= max.y &&
               pt.z >= min.z && pt.z <= max.z;
    }

    bool contains(const AABB& o) const {
        return contains(o.min) && contains(o.max);
    }

    bool intersects(const AABB& o) const {
        return max.x >= o.min.x && min.x <= o.max.x &&
               max.y >= o.min.y && min.y <= o.max.y &&
               max.z >= o.min.z && min.z <= o.max.z;
    }

    /// Transforms all 8 corners by matrix M and computes a new tight AABB in world space.
    AABB transformed(const Mat4& m) const;
};

// =========================================================================
// BoundingSphere — 3D Bounding Sphere
// =========================================================================
struct BoundingSphere {
    Vec3 center{0.0f, 0.0f, 0.0f};
    f32  radius{0.0f};

    constexpr BoundingSphere() = default;
    constexpr BoundingSphere(const Vec3& c, f32 r) : center(c), radius(r) {}

    static BoundingSphere from_aabb(const AABB& box) {
        return {box.center(), box.radius()};
    }

    bool contains(const Vec3& pt) const {
        return (pt - center).length_sq() <= (radius * radius);
    }

    bool intersects(const BoundingSphere& o) const {
        const f32 r_sum = radius + o.radius;
        return (center - o.center).length_sq() <= (r_sum * r_sum);
    }

    bool intersects(const AABB& box) const;
};

// =========================================================================
// Ray — 3D Directed Ray
// =========================================================================
struct Ray {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f}; // normalized

    constexpr Ray() = default;
    Ray(const Vec3& orig, const Vec3& dir);

    Vec3 at(f32 t) const {
        return origin + direction * t;
    }

    /// Ray vs AABB intersection (Kay-Kajiya slab method).
    /// Returns true if hit; out_t contains distance to closest hit along ray.
    bool intersects(const AABB& box, f32& out_t) const;

    /// Ray vs BoundingSphere intersection.
    bool intersects(const BoundingSphere& sphere, f32& out_t) const;

    /// Ray vs Triangle intersection (Möller–Trumbore algorithm).
    /// out_barycentric holds (u, v) weights; w = 1 - u - v.
    bool intersects_triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
                             f32& out_t, Vec2& out_barycentric) const;
};

// =========================================================================
// Frustum — 6-Plane View Frustum with positive-vertex culling
// =========================================================================
struct Frustum {
    enum PlaneIndex {
        Left = 0,
        Right = 1,
        Bottom = 2,
        Top = 3,
        Near = 4,
        Far = 5,
        PlaneCount = 6
    };

    std::array<Plane, PlaneCount> planes;

    Frustum() = default;

    /// Extracts normalized frustum planes from a view-projection matrix (Vulkan clip space [0, 1]).
    static Frustum from_view_projection(const Mat4& vp);

    bool contains_point(const Vec3& pt) const;
    bool intersects_sphere(const BoundingSphere& sphere) const;

    /// High-performance Frustum vs AABB test using positive-vertex optimization.
    /// Returns true if box is inside or intersects frustum; false if fully outside.
    bool intersects_aabb(const AABB& box) const;
};

} // namespace nf
