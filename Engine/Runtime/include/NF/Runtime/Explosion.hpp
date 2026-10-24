#pragma once

// NF/Runtime/Explosion.hpp — a blast is a radius, not a ray.
//
// Phase 46 gave a projectile a body. A body that lands on one target still only
// hurts that target; a rocket, a grenade or a barrel is defined by the space it
// denies. This file owns the arithmetic of that space:
//
//   * damage falls linearly from the centre to the edge;
//   * a point inside a box is at distance zero;
//   * a point outside is measured to the NEAREST point on the box, not to its
//     origin — a crate two metres wide should not shield itself from a blast
//     that touches its side.
//
// The runtime owns the world-facing half: which colliders are in range, what
// blocks the line to them, and who is allowed to be hurt. Keeping the falloff
// here means a designer can tune a grenade without a device, and a test can
// prove that the edge of the blast is not accidentally the centre.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

namespace nf::runtime {

/// The three numbers that define a radial blast.
struct ExplosionFalloff {
    /// Farthest a blast reaches, in metres. Zero or negative disables it.
    f32 radius = 0.0f;
    /// Damage at the centre, before the target's armor and zones.
    f32 damage = 0.0f;
    /// Damage at the edge as a fraction of the centre, in [0, 1]. Zero means
    /// the edge is harmless; one means the blast does not fall off at all.
    f32 min_scale = 0.0f;
};

/// Whether a blast can hurt anything.
///
/// A non-finite or non-positive radius/damage is refused. A negative or
/// non-finite minimum scale is clamped by `explosion_damage_at()`, not treated
/// as a reason to drop the whole blast: a scene typo should not make a grenade
/// silently inert.
[[nodiscard]] bool explosion_valid(const ExplosionFalloff& blast) noexcept;

/// Damage at `distance` from the centre.
///
/// The curve is linear in distance: centre = `damage`, edge = `damage *
/// min_scale`, beyond the edge = 0. A non-finite or negative distance is
/// treated as zero (the centre) rather than producing a NaN.
[[nodiscard]] f32 explosion_damage_at(const ExplosionFalloff& blast, f32 distance) noexcept;

/// Shortest distance from a point to an axis-aligned box. Zero inside it.
///
/// A box with a reversed or non-finite extent returns infinity: a broken
/// collider must not become a free hit at the centre of the world.
[[nodiscard]] f32 distance_point_to_aabb(const Vec3& point, const Vec3& min,
                                         const Vec3& max) noexcept;

} // namespace nf::runtime
