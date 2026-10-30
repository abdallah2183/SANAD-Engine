#pragma once

// NF/Physics/PhysicsQuery.hpp — Spatial Raycasting, Sweeping & Overlap Queries

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Physics/Shapes.hpp>

#include <vector>
#include <optional>

namespace nf::physics {

struct Ray3D {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f}; // Should be normalized
    f32 max_distance = 1000.0f;

    Vec3 point_at(f32 t) const { return origin + direction * t; }
};

struct RaycastHit {
    BodyHandle body{};
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 distance = 0.0f;
    f32 fraction = 0.0f; // [0, 1] relative to ray.max_distance
    bool has_hit = false;
};

struct SweepHit {
    BodyHandle body{};
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 distance = 0.0f;
    f32 fraction = 0.0f; // [0, 1] along sweep travel path
    bool has_hit = false;
};

/// High-performance spatial query and collision test suite over PhysicsWorld.
class PhysicsQuery {
public:
    /// Casts a ray through the physics world and returns the closest body hit (if any).
    static std::optional<RaycastHit> raycast(
        const PhysicsWorld& world,
        const Ray3D& ray,
        u32 layer_mask = 0xFFFFFFFF
    );

    /// Casts a ray through the physics world and returns all hits sorted from closest to furthest.
    static std::vector<RaycastHit> raycast_all(
        const PhysicsWorld& world,
        const Ray3D& ray,
        u32 layer_mask = 0xFFFFFFFF
    );

    /// Sweeps a sphere from start to end, returning the first collision contact.
    static std::optional<SweepHit> sweep_sphere(
        const PhysicsWorld& world,
        const Vec3& start,
        const Vec3& end,
        f32 radius,
        u32 layer_mask = 0xFFFFFFFF
    );

    /// Queries all bodies whose shapes overlap a sphere in world space.
    static std::vector<BodyHandle> overlap_sphere(
        const PhysicsWorld& world,
        const Vec3& center,
        f32 radius,
        u32 layer_mask = 0xFFFFFFFF
    );

    /// Queries all bodies whose shapes overlap an oriented bounding box (OBB).
    static std::vector<BodyHandle> overlap_box(
        const PhysicsWorld& world,
        const Vec3& center,
        const Vec3& half_extents,
        const Quat& orientation,
        u32 layer_mask = 0xFFFFFFFF
    );

    // --- Analytic Intersection Primitives ---

    static bool intersect_ray_sphere(
        const Ray3D& ray,
        const Vec3& sphere_pos,
        f32 radius,
        f32& out_t,
        Vec3& out_normal
    );

    static bool intersect_ray_box(
        const Ray3D& ray,
        const Vec3& box_pos,
        const Quat& box_orient,
        const Vec3& half_extents,
        f32& out_t,
        Vec3& out_normal
    );

    static bool intersect_ray_plane(
        const Ray3D& ray,
        const Vec3& plane_pos,
        const Vec3& plane_normal,
        f32& out_t,
        Vec3& out_normal
    );

    static bool check_overlap_sphere_shape(
        const Vec3& sphere_pos,
        f32 sphere_radius,
        const Shape& shape,
        const Vec3& shape_pos,
        const Quat& shape_orient
    );

    static bool check_overlap_box_shape(
        const Vec3& box_pos,
        const Vec3& box_half_extents,
        const Quat& box_orient,
        const Shape& shape,
        const Vec3& shape_pos,
        const Quat& shape_orient
    );
};

} // namespace nf::physics
