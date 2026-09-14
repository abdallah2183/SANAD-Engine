#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

namespace nf::physics {

// ============================================================================
// Shapes
//
// Deliberately a small closed set: sphere, oriented box, infinite plane. That is
// enough to build a level out of and enough to test exhaustively. Capsules,
// convex hulls and meshes are not here on purpose — see Docs/Phase8_Plan.md §7.
//
// A Shape is a tagged struct-of-all rather than a std::variant: the shapes are a
// few floats each, and a plain switch in the narrowphase beats visitor machinery
// for both readability and the debugger.
// ============================================================================

enum class ShapeType : u8 {
    Sphere = 0,
    Box = 1,
    Plane = 2,
};

/// Axis-aligned bounding box. Overlap is inclusive of touching faces: two boxes
/// that exactly touch are candidates, because a resting contact is exactly that
/// case and excluding it would make every object fall through the floor.
struct Aabb {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    static Aabb from_centre_half_extents(const Vec3& centre, const Vec3& half_extents) {
        return Aabb{centre - half_extents, centre + half_extents};
    }

    Vec3 centre() const { return (min + max) * 0.5f; }
    Vec3 half_extents() const { return (max - min) * 0.5f; }

    bool overlaps(const Aabb& o) const {
        return min.x <= o.max.x && max.x >= o.min.x &&
               min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }

    Aabb merged(const Aabb& o) const {
        return Aabb{min.min(o.min), max.max(o.max)};
    }

    void expand(f32 margin) {
        const Vec3 m(margin);
        min -= m;
        max += m;
    }
};

/// Sphere centred on the body origin.
struct SphereShape {
    f32 radius = 0.5f;
};

/// Oriented box, half-extents along the body's local axes.
struct BoxShape {
    Vec3 half_extents{0.5f, 0.5f, 0.5f};
};

/// Infinite plane through the body origin. Static only: a moving plane is a wall,
/// which a box models better and collides correctly against.
struct PlaneShape {
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

struct Shape {
    ShapeType type = ShapeType::Sphere;
    SphereShape sphere{};
    BoxShape box{};
    PlaneShape plane{};

    static Shape make_sphere(f32 radius);
    static Shape make_box(const Vec3& half_extents);
    static Shape make_plane(const Vec3& normal);

    /// True for a shape that extends infinitely, which the broadphase must not
    /// insert into a spatial grid.
    bool is_unbounded() const { return type == ShapeType::Plane; }

    /// Volume. Zero for a plane (it has none).
    f32 volume() const;

    /// Diagonal of the inertia tensor about the centre of mass, for a body of
    /// the given mass. All three shapes are symmetric about their local axes, so
    /// the tensor is diagonal in body space and this is all the solver needs.
    /// Returns zero for a plane, whose mass is treated as infinite.
    Vec3 inertia_diagonal(f32 mass) const;

    /// World-space AABB. A plane returns an enormous box rather than infinities,
    /// because callers do arithmetic on the result; the broadphase is expected to
    /// keep planes out of the grid entirely (see `is_unbounded`).
    void compute_aabb(const Vec3& position, const Quat& orientation,
                      Vec3& out_min, Vec3& out_max) const;

    /// Furthest point of the shape in a world-space direction. The plane returns
    /// the body origin, which is the only sensible answer for an infinite shape
    /// and keeps the function total.
    Vec3 support(const Vec3& position, const Quat& orientation, const Vec3& direction) const;

    /// Half the AABB diagonal, i.e. a bounding radius. Used for broadphase
    /// margin and for cheap early rejection.
    f32 bounding_radius() const;
};

} // namespace nf::physics
