#pragma once

#include <NF/Physics/Shapes.hpp>

#include <cstdint>

namespace nf::physics {

/// One contact point, with the solver's accumulated impulses attached so they
/// can be carried into the next step (warm starting). Without warm starting a
/// stack of boxes jitters and slowly sinks, because every step rediscovers the
/// contact and starts its impulse from zero.
struct ContactPoint {
    Vec3 position{0.0f, 0.0f, 0.0f}; // world space
    f32 penetration = 0.0f;          // positive means overlapping
    f32 normal_impulse = 0.0f;       // accumulated, for warm starting
    f32 tangent_impulse[2] = {0.0f, 0.0f};
    u32 feature_id = 0;              // matches points across frames for warm starting
};

/// A contact manifold between two bodies.
///
/// `normal` points from A towards B. To separate, A moves along -normal and B
/// along +normal. Getting this backwards is the classic sign error and produces
/// objects that accelerate into each other, so the tests assert the direction
/// rather than just the depth.
struct Manifold {
    static constexpr u32 kMaxPoints = 4;

    u32 body_a = 0;
    u32 body_b = 0;
    Vec3 normal{0.0f, 1.0f, 0.0f};
    ContactPoint points[kMaxPoints];
    u32 point_count = 0;

    void clear() { point_count = 0; }

    /// Add a point. When the manifold is full the shallowest existing point is
    /// replaced, because a box resting on a face needs its four deepest corners:
    /// keeping the first four found makes it tip over.
    void add_point(const Vec3& position, f32 penetration, u32 feature_id);

    /// Deepest penetration in the manifold, or 0 when empty.
    f32 max_penetration() const;
};

/// Fill `out` with the contact between two shapes. Returns false when they are
/// not touching, in which case `out` is left cleared.
///
/// The caller owns the world transforms; this function is pure geometry and
/// touches no body state, which is what makes it directly testable.
bool collide(const Shape& shape_a, const Vec3& pos_a, const Quat& rot_a,
             const Shape& shape_b, const Vec3& pos_b, const Quat& rot_b,
             Manifold& out);

// --- Individual pairs, exposed for direct testing --------------------------
//
// The dispatcher is a switch over nine type pairs; testing each pair through its
// own function is far more precise than guessing which pair a dispatcher bug
// landed on.

bool collide_sphere_sphere(f32 radius_a, const Vec3& pos_a,
                           f32 radius_b, const Vec3& pos_b,
                           Manifold& out);

bool collide_sphere_box(f32 radius_a, const Vec3& pos_a,
                        const Vec3& half_extents_b, const Vec3& pos_b, const Quat& rot_b,
                        Manifold& out);

bool collide_sphere_plane(f32 radius_a, const Vec3& pos_a,
                          const Vec3& plane_normal_b, const Vec3& pos_b, const Quat& rot_b,
                          Manifold& out);

bool collide_box_plane(const Vec3& half_extents_a, const Vec3& pos_a, const Quat& rot_a,
                       const Vec3& plane_normal_b, const Vec3& pos_b, const Quat& rot_b,
                       Manifold& out);

bool collide_box_box(const Vec3& half_extents_a, const Vec3& pos_a, const Quat& rot_a,
                     const Vec3& half_extents_b, const Vec3& pos_b, const Quat& rot_b,
                     Manifold& out);

} // namespace nf::physics
