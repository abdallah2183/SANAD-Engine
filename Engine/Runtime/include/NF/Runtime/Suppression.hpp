#pragma once

// NF/Runtime/Suppression.hpp — a near miss is information.
//
// An NPC that only reacts to hits cannot be pinned down: every bullet that
// misses is free. Suppression is the opposite rule: shots that pass close make
// the agent slower to aim and less willing to fire, and the effect decays when
// the shooting stops.
//
// The arithmetic is deliberately small and deterministic:
//
//   * a shot within a radius adds a fixed amount, capped at 1;
//   * the level decays at a fixed rate per second;
//   * aim speed and fire cone are scaled down by the level;
//   * above a threshold the trigger is held entirely.
//
// The runtime owns the question "which shots passed near which agent"; this
// file owns what the answer does to the aim.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

namespace nf::runtime {

/// A combatant's suppression level, between 0 (calm) and 1 (pinned).
struct SuppressionState {
    f32 level = 0.0f;
};

/// How much one near miss adds. Five close shots pin a target.
inline constexpr f32 kSuppressionPerShot = 0.22f;

/// How quickly the level falls when no new shot arrives.
inline constexpr f32 kSuppressionDecayPerSecond = 0.35f;

/// How far from the shot's path a shot still counts as a near miss, in metres.
inline constexpr f32 kSuppressionRadius = 3.0f;

/// Aim speed is multiplied by this at full suppression. A pinned agent turns at
/// half speed, so flanking works.
inline constexpr f32 kSuppressionAimSpeedScale = 0.5f;

/// The fire cone is multiplied by this at full suppression. A pinned agent
/// needs to be more precisely on target before it may shoot.
inline constexpr f32 kSuppressionConeScale = 0.5f;

/// At or above this level the trigger is held, regardless of aim.
inline constexpr f32 kSuppressionFireBlock = 0.75f;

/// Adds suppression, clamping the result to [0, 1]. Non-finite input is ignored.
void suppression_add(SuppressionState& state, f32 amount) noexcept;

/// Decays the level. `dt <= 0` changes nothing.
void suppression_step(SuppressionState& state, f32 dt) noexcept;

/// Aim-speed multiplier for the current level.
[[nodiscard]] f32 suppression_aim_scale(f32 level) noexcept;

/// Fire-cone multiplier for the current level.
[[nodiscard]] f32 suppression_cone_scale(f32 level) noexcept;

/// True when suppression is high enough to hold the trigger.
[[nodiscard]] bool suppression_blocks_fire(f32 level) noexcept;

/// Shortest distance from a point to a ray segment, in world units.
///
/// `direction` need not be normalised; a zero direction returns the distance to
/// the origin. `max_distance` clips the ray, so a shot that ends before the
/// point does not count as passing near it.
[[nodiscard]] f32 distance_point_to_ray(const Vec3& point, const Vec3& origin,
                                        const Vec3& direction, f32 max_distance) noexcept;

} // namespace nf::runtime
