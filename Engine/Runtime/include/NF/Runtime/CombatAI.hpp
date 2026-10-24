#pragma once

// NF/Runtime/CombatAI.hpp — an NPC that can actually shoot back.
//
// Phase 44 gave the engine a gun and hit points. A gun that only the player can
// pull is half a fight: every enemy still had to be a script that knew where the
// player was, how to turn towards them, when to pull the trigger and when to
// stop. That is four decisions, and they are the same four for every shooter.
//
// This is those four decisions as arithmetic:
//
//   1. NOTICE  — a guard that has not seen you does not track you. The reaction
//                clock is a gate on the aim, not a delay on the trigger, so an
//                enemy that has just spotted you is still pointing where it was.
//   2. TURN    — the aim converges on the target at a finite rate. A turret that
//                snaps is a turret that cannot be flanked.
//   3. DECIDE  — the trigger is allowed only when the aim is inside the cone,
//                the target is visible, and the reaction clock has run out.
//   4. DISCIPLINE — fire in bursts, not forever: a steady stream is both
//                unrealistic and unreadable, and the pause is where the player
//                gets to move.
//
// Nothing here knows about the ECS, the physics world or the frame loop. It
// takes a target position and a "can I see it" boolean, and returns two angles
// and a trigger. The runtime owns the ray that answers "can I see it" and the
// weapon that answers "did it hit".

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

namespace nf::runtime {

/// The view direction for a yaw/pitch pair, in the same convention the follow
/// camera uses: yaw 0 with pitch 0 looks down -Z, positive yaw turns left, and
/// positive pitch looks UP.
///
/// Exposed because a combatant aims in angles and the ray needs a vector, and
/// because the inverse below must use exactly the same convention — a mismatch
/// is an enemy that aims a few degrees off and never hits.
[[nodiscard]] Vec3 aim_direction(f32 yaw_deg, f32 pitch_deg) noexcept;

/// The yaw/pitch of a direction. A zero-length direction returns (0, 0): an
/// answer that is stable and obviously "no aim", rather than a NaN that spreads
/// into the camera on the next frame.
void aim_angles(const Vec3& direction, f32& out_yaw_deg, f32& out_pitch_deg) noexcept;

/// How a combatant fights. Scene data: it belongs to the level's difficulty.
struct CombatAimConfig {
    /// Seconds between noticing a target and being allowed to shoot. 0 is a
    /// turret that was already aimed; 0.6 is a guard who has to raise a rifle.
    f32 reaction_seconds = 0.35f;

    /// How fast the aim turns, in degrees per second. 180 is a soldier, 720 is
    /// a turret, 30 is something slow and heavy.
    f32 aim_speed_deg_per_s = 220.0f;

    /// How close to the target the aim must be before the trigger is allowed,
    /// in degrees. Wider than the weapon's own spread on purpose: this is
    /// "pointing at", not "guaranteed to hit".
    f32 fire_cone_deg = 3.0f;

    /// Farthest the agent will engage, in metres. Beyond it the target is not
    /// worth shooting at — and a longer shot is a shot the weapon's falloff
    /// already makes pointless.
    f32 max_range = 60.0f;

    /// Burst discipline. `burst_on_seconds` of trigger, then
    /// `burst_off_seconds` of rest, repeated. A zero on-time means "hold the
    /// trigger while aimed", which is what a turret wants.
    f32 burst_on_seconds = 0.55f;
    f32 burst_off_seconds = 0.85f;

    /// Vertical clamp for the aim, in degrees. The same idea as the camera's
    /// pitch range: an agent should not be able to point at its own feet or
    /// straight up.
    f32 pitch_min = -70.0f;
    f32 pitch_max = 50.0f;
};

/// A combatant's aim between frames. Session state: never saved.
struct CombatAimState {
    /// Where the agent is currently pointing, in degrees. Initialised by the
    /// caller from the agent's own facing, so an NPC does not snap to yaw 0 on
    /// its first frame.
    f32 yaw_deg = 0.0f;
    f32 pitch_deg = 0.0f;

    /// Seconds the current target has been noticed. Reset when it is lost.
    f32 reaction_timer = 0.0f;

    /// Seconds left in the current burst phase, and which phase it is.
    f32 burst_timer = 0.0f;
    bool trigger = false;

    /// Whether the agent had a target last frame. Used to reset the reaction
    /// clock on a LOSS of target, not on a re-acquire of the same one.
    bool engaged = false;
};

/// What the frame sees.
struct CombatAimInput {
    /// False when there is no target, or it is dead, or it is out of range.
    /// The agent then holds its aim and its fire, and forgets the reaction.
    bool has_target = false;
    /// World-space eye and target positions. The eye is the muzzle's origin:
    /// the agent's transform plus its own eye height, not the target's.
    Vec3 eye{0.0f, 0.0f, 0.0f};
    Vec3 target{0.0f, 0.0f, 0.0f};
    /// Whether the target can actually be seen. A ray is cast by the runtime
    /// (Hitscan.hpp); this is the answer, not the question.
    bool visible = false;
    /// Frame time. `dt <= 0` advances nothing.
    f32 dt = 0.0f;
};

/// What the frame decided.
struct CombatAimOutput {
    /// Where the agent points after this frame, in degrees.
    f32 yaw_deg = 0.0f;
    f32 pitch_deg = 0.0f;
    /// True when the agent may pull the trigger this frame.
    bool trigger = false;
    /// True when the aim is inside `fire_cone_deg` of the target. Reported
    /// separately from `trigger` because "aimed but holding fire" (a burst
    /// pause) is a state a HUD and a test both want to tell from "not aimed".
    bool aimed = false;
    /// Degrees between the current aim and the target, for a test or a debug
    /// overlay.
    f32 error_deg = 0.0f;
};

/// Advances one frame of aiming.
///
/// The order matters and is the contract:
///   * no target  -> hold the aim, drop the trigger, reset the reaction clock;
///   * a new target (or one that was lost) starts the reaction clock at zero;
///   * the aim turns towards the target at `aim_speed_deg_per_s`, taking the
///     SHORT way round, so an agent does not spin 350 degrees to correct 10;
///   * the trigger is allowed only after the reaction clock, inside the cone,
///     with the target visible, and while the burst phase is on.
[[nodiscard]] CombatAimOutput combat_aim_step(CombatAimState& state,
                                              const CombatAimConfig& config,
                                              const CombatAimInput& input);

/// The signed shortest angular difference `to - from`, wrapped into
/// (-180, 180]. Exposed because it is the one line of trigonometry in here that
/// a reader will want to check, and because the camera's auto-align needs the
/// same rule.
[[nodiscard]] f32 angle_delta_deg(f32 from_deg, f32 to_deg) noexcept;

} // namespace nf::runtime
