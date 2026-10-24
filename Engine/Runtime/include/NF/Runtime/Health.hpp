#pragma once

// NF/Runtime/Health.hpp — hit points, as arithmetic.
//
// A weapon that fires is only half a fight: something has to be able to take
// the hit. The engine already had damage for DESTRUCTIBLES (Phase 19) — a
// crate that shatters into shards — but nothing for a thing that is supposed
// to survive being shot and eventually not.
//
// This is deliberately the smallest model that is still honest:
//
//   * hit points with a maximum,
//   * optional regeneration after a quiet period,
//   * an invulnerable switch for cutscenes and spawn protection,
//   * and a `health` that reaches exactly zero rather than going negative.
//
// It does NOT own death. A dead entity is a gameplay decision — ragdoll it,
// hide it, play a sound, respawn it — and that decision belongs to the game,
// which can read `health_alive` from Lua. An engine that unilaterally deleted
// the entity would be making that decision for every game at once.

#include <NF/Core/Types.hpp>

namespace nf::runtime {

/// An entity's hit points, as the scene carries them.
///
/// `health` is authored as the STARTING value and is overwritten by play: a
/// scene that wants a wounded soldier writes `health=35 max=100`. Saving a
/// scene mid-fight therefore saves the wound, which is the honest reading of
/// "the scene says where this entity is".
struct HealthComponent {
    /// The ceiling. Must be finite and positive; a zero ceiling is refused by
    /// the loader rather than turning every hit into an instant death.
    f32 max_health = 100.0f;

    /// Current hit points, clamped to [0, max_health] on load and on every
    /// change.
    f32 health = 100.0f;

    /// Armor ceiling and current points (Phase 50). Zero disables armor and is
    /// the default, so every scene written before this field keeps exactly the
    /// damage it had.
    ///
    /// Armor is a second pool, not a second health bar: `health_apply_hit()`
    /// spends it before health, while the raw `health_apply_damage()` path
    /// still bypasses it. A scene authors `armor=50` for a full vest and
    /// `armor_max=50` for the bar's denominator; an omitted maximum means the
    /// authored current value is also the maximum.
    f32 max_armor = 0.0f;
    f32 armor = 0.0f;

    /// Fraction of a protectable hit that armor takes while points remain, in
    /// [0, 1]. The rest reaches health in the same hit. 0.65 is a vest that
    /// helps but does not make a target invincible.
    f32 armor_absorb = 0.65f;

    /// Opt-in hit zones (Phase 50). False means every hit is a body hit and the
    /// two multipliers below are unused, preserving the pre-Phase-50 contract.
    bool hit_zones = false;

    /// Multipliers for a head and a limb hit. Head above one rewards aim;
    /// limbs below one makes a leg shot a real choice rather than a free body
    /// shot. The defaults are a common shooter convention.
    f32 headshot_multiplier = 2.0f;
    f32 limb_multiplier = 0.75f;

    /// Fraction of each damage type this target ignores, in [0, 1]. These are
    /// opt-in per type: a fire-resistant enemy can still be shot normally.
    /// Applied after the hit zone and before armor, so a resistant target takes
    /// less from the same hit but still spends armor on what gets through.
    f32 ballistic_resistance = 0.0f;
    f32 explosive_resistance = 0.0f;
    f32 fire_resistance = 0.0f;

    /// Hit points regained per second once `regen_delay` has passed without
    /// damage. 0 (the default) means no regeneration at all.
    f32 regen_per_second = 0.0f;

    /// Seconds of quiet required before regeneration starts. A hit resets it,
    /// which is what makes regeneration a reward for disengaging rather than
    /// a reason to stand in the open.
    f32 regen_delay = 0.0f;

    /// Runtime clock: seconds since the last hit. Session state, never
    /// authored — a scene that names it is ignored rather than obeyed, because
    /// "I was shot 3 seconds ago" is not a property of a level.
    f32 since_damage = 0.0f;

    /// Damage is ignored entirely. For a cutscene, a spawn shield, or a boss
    /// that has not been made vulnerable yet.
    bool invulnerable = false;

    /// Off: the entity keeps its hit points and takes no damage. The same
    /// "switch it off, keep it placed" contract the other components keep.
    bool enabled = true;
};

/// Whether the component describes a usable pool of hit points.
///
/// A non-finite or non-positive maximum is refused: every later division by it
/// (the HUD's bar, the fraction) would be a NaN, and a NaN health compares
/// false against everything, so the entity would be neither alive nor dead.
[[nodiscard]] bool health_valid(const HealthComponent& health) noexcept;

/// Hit points as a fraction in [0, 1]. 0 for an invalid component, so a HUD
/// bar drawn from this never sees a NaN.
[[nodiscard]] f32 health_fraction(const HealthComponent& health) noexcept;

/// True while there are hit points left. An invalid component is NOT alive:
/// an entity with a broken health block should not be shootable forever.
[[nodiscard]] bool health_alive(const HealthComponent& health) noexcept;

/// Applies RAW damage and returns how much health was actually taken.
///
/// Refused (returns 0) when the component is disabled, invulnerable, invalid,
/// or the amount is not finite and positive. The clock resets on any accepted
/// hit — including one that only removes 1 point — because regeneration is
/// about being SHOT, not about being hurt badly.
///
/// This is the pre-Phase-50 path and deliberately bypasses armor and hit zones:
/// a script's `nf.damage()` and an environmental hazard are not a bullet. Use
/// `health_apply_hit()` (Damage.hpp) for a weapon hit that should read a zone
/// and spend armor.
f32 health_apply_damage(HealthComponent& health, f32 amount) noexcept;

/// Heals and returns how much was actually restored (never past the maximum).
f32 health_heal(HealthComponent& health, f32 amount) noexcept;

/// Advances the quiet clock and regenerates. Returns hit points restored.
///
/// `dt <= 0` advances nothing, so a paused frame cannot regenerate a player.
f32 health_step(HealthComponent& health, f32 dt) noexcept;

} // namespace nf::runtime
