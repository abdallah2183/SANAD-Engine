#pragma once

// NF/Runtime/Hitscan.hpp — "what did the bullet hit", as geometry.
//
// A weapon produces an ANGLE, not a ray (Weapon.hpp). Turning that angle into a
// hit needs the world, and the world is the one thing a weapon must not know
// about. This is the seam: the runtime hands over the scene's colliders as
// world-space boxes, gets back the nearest one, and is free to apply damage,
// spawn a decal or play a sound.
//
// Why boxes and not the physics world's exact shapes: the engine's own solver
// (PhysicsWorld) has no ray query — Jolt does, but the scene's dynamic bodies
// live in PhysicsWorld, so a Jolt ray would see the wrong world. Boxes are the
// one description both worlds agree on, `Shape::compute_aabb` already produces
// them, and a bullet hitting the bounds of a crate is the shot the player drew.
// The limitation is stated rather than hidden: a sphere target is shot as the
// box around it, so a shot at its corner can hit air. A per-shape test is the
// obvious upgrade and does not change this contract.
//
// The test is pure: no ECS, no device, no clock. A hit is a number.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>
#include <NF/Physics/Shapes.hpp>

#include <vector>

namespace nf::runtime {

/// One thing a bullet can hit: an entity and the box it occupies this frame.
///
/// The box is in WORLD space, already including the entity's transform and its
/// collider's local extents. The caller builds these once per volley rather than
/// once per pellet, because a shotgun's eight pellets should not walk the whole
/// scene eight times.
struct HitscanTarget {
    ecs::Entity entity{};
    physics::Aabb bounds{};
};

/// What a ray found.
struct HitscanHit {
    bool hit = false;
    ecs::Entity entity = ecs::kInvalidEntity;
    /// World-space point on the box's surface. When the ray STARTS inside a
    /// box, this is the origin: a shot fired from inside something hits it at
    /// zero distance, which is the truthful answer.
    Vec3 position{0.0f, 0.0f, 0.0f};
    /// Outward normal of the face that was entered, or -direction when the
    /// origin was already inside.
    Vec3 normal{0.0f, 0.0f, 0.0f};
    /// Distance along the ray, in metres.
    f32 distance = 0.0f;
};

/// The nearest target along the ray, or a miss.
///
/// Contract, matching JoltWorld's queries so the two never disagree about what
/// a "miss" is:
///   * `direction` is normalized internally; a zero-length direction is a miss.
///   * A non-positive `max_distance` is a miss.
///   * `ignore` skips one entity (the shooter), and an invalid one is a no-op.
///   * Ties at exactly the same distance go to the LOWER entity id, so two
///     coplanar walls always resolve the same way on every run.
[[nodiscard]] HitscanHit hitscan_nearest(const std::vector<HitscanTarget>& targets,
                                         Vec3 origin, Vec3 direction, f32 max_distance,
                                         ecs::Entity ignore = ecs::kInvalidEntity);

/// Ray against one box. Exposed because the slab test is the part worth
/// testing on its own, and because a caller with a spatial index will want to
/// run it against candidates it already narrowed down.
[[nodiscard]] bool ray_box(const Vec3& origin, const Vec3& direction, const physics::Aabb& box,
                           f32& out_distance, Vec3& out_normal);

} // namespace nf::runtime
