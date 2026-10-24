#pragma once

// NF/Runtime/Projectile.hpp — a bullet that takes time to arrive.
//
// Phase 44 fired a ray and applied the hit in the same frame. That is the right
// model for a rifle at close range and the wrong one for everything a player
// can see: a grenade, a rocket, a slow plasma bolt, or a long shot that should
// visibly drop. A weapon still produces an angle (Weapon.hpp); this turns that
// angle into a body with a velocity, and integrates the body one frame at a
// time. The runtime sweeps the segment it travelled against the scene's boxes,
// so a fast projectile cannot tunnel through a thin wall between frames.
//
// The split is deliberate:
//
//   * this file owns the ARITHMETIC — position, velocity, gravity, lifetime,
//     distance travelled;
//   * WeaponRuntime.cpp owns the WORLD — colliders, damage, factions;
//   * no ECS, no Vulkan, no clock, so the trajectory is testable on its own.
//
// Integration is exact for constant acceleration, not Euler:
//
//   p' = p + v*dt + 0.5*a*dt^2
//   v' = v + a*dt
//
// With a constant gravity this is the closed-form parabola, so the same shot
// lands in the same place at 30, 60 or 144 fps as long as the total time is the
// same. The caller still has to trace each segment: a frame that covers ten
// metres is a ten-metre segment, and checking only the end point would let a
// bullet pass through a wall.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

namespace nf::runtime {

/// One projectile in flight. Session data: never saved.
struct ProjectileBody {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};

    /// Seconds since spawn, capped at `lifetime_seconds`.
    f32 age_seconds = 0.0f;

    /// Total flight time allowed. A projectile that reaches it expires where it
    /// is, which is how `range` stays a real limit instead of a suggestion.
    f32 lifetime_seconds = 0.0f;

    /// Path length flown so far, in metres. Damage falloff reads THIS, not the
    /// straight-line distance to the shooter: a bullet that arcs upward and
    /// comes down has still travelled the longer path.
    f32 distance_travelled = 0.0f;
};

/// What one integration step covered.
struct ProjectileAdvance {
    /// Segment from the previous position to the new one. The runtime traces
    /// exactly this, so a fast round cannot skip a wall.
    Vec3 from{0.0f, 0.0f, 0.0f};
    Vec3 to{0.0f, 0.0f, 0.0f};

    /// The time actually integrated. Less than the requested `dt` when the
    /// lifetime ended mid-frame; zero when the step was refused.
    f32 step_seconds = 0.0f;

    /// True when the projectile reached its lifetime during this step. The
    /// caller resolves the segment first and then removes it.
    bool expired = false;

    /// False for a paused or invalid step; `from == to == position`.
    bool moved = false;
};

/// Whether a body has a usable lifetime and finite state.
///
/// A projectile with a non-positive or non-finite lifetime, or a non-finite
/// position/velocity, is refused rather than integrated into a NaN that poisons
/// every later frame.
[[nodiscard]] bool projectile_body_valid(const ProjectileBody& body) noexcept;

/// Advances one frame under constant `gravity` (metres per second squared).
///
/// Contract:
///   * `dt <= 0`, a non-finite `dt`, or an invalid body advances nothing;
///   * a non-finite gravity is treated as zero — the shot still flies straight,
///     which is more useful than dropping every projectile on the floor;
///   * the step is clipped to the remaining lifetime, so `age_seconds` never
///     exceeds `lifetime_seconds` and the segment ends exactly at the limit;
///   * `distance_travelled` grows by the length of the segment actually flown.
[[nodiscard]] ProjectileAdvance projectile_advance(ProjectileBody& body, const Vec3& gravity,
                                                   f32 dt) noexcept;

/// Speed a projectile needs to cover `range` metres before its lifetime ends.
///
/// A scene authors a muzzle speed; the lifetime is derived from the weapon's
/// range and that speed, so a slower projectile does not silently get a longer
/// reach. Returns 0 for an invalid pair.
[[nodiscard]] f32 projectile_lifetime_for_range(f32 range, f32 speed) noexcept;

/// The drop below the straight line after flying `distance` at `speed` under
/// `gravity`, for a UI reticle or a test. Zero for a non-positive speed.
[[nodiscard]] f32 projectile_drop_at(f32 distance, f32 speed, f32 gravity) noexcept;

/// Where to aim so a projectile of `speed` meets a target moving at
/// `target_velocity` (Phase 48).
struct ProjectileIntercept {
    bool valid = false;
    /// The world-space point to aim at. When no intercept exists this is the
    /// target's current position, so a caller can always use it.
    Vec3 point{0.0f, 0.0f, 0.0f};
    /// Time from launch to arrival. Zero when the solution is invalid.
    f32 time_seconds = 0.0f;
};

/// Solves the constant-velocity intercept:
///
///     |target + velocity*t - shooter| = speed*t
///
/// A projectile that aims at where the target IS hits behind a moving target.
/// This returns where it WILL be, assuming the target keeps its velocity.
/// The smallest positive root is chosen: the first meeting, not the one after
/// the target has passed and come back.
///
/// A non-finite input, a non-positive speed, or a target outrunning the round
/// (no positive root) returns an invalid solution with the current position,
/// so a caller can fall back to aiming at the target without a branch that
/// silently produces a NaN.
[[nodiscard]] ProjectileIntercept projectile_intercept(Vec3 shooter, Vec3 target,
                                                       Vec3 target_velocity,
                                                       f32 speed) noexcept;

} // namespace nf::runtime
