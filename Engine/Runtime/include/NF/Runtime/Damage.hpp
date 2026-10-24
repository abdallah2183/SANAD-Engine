#pragma once

// NF/Runtime/Damage.hpp — where a shot landed, and what stopped it.
//
// Phase 44 gave every hit the same price: weapon damage after distance falloff,
// subtracted from hit points. That is the right default for a crate and the
// wrong one for a soldier: a shot in the helmet and a shot in the boot are not
// the same event, and a vest that absorbs nothing is decoration.
//
// This file owns the two rules that turn one number into a layered hit:
//
//   * HIT ZONE — the collider is divided into a head band, a body band and a
//     limb band. A scene opts in with `zones=true`; without it every hit is a
//     body hit and every existing scene keeps its old damage exactly.
//   * ARMOR — while armor points remain, a configured fraction of the
//     protectable damage is taken from armor first. A weapon's `pierce=` lets
//     part of the hit bypass that protection.
//
// The arithmetic is pure: no ECS, no physics world, no frame clock. The runtime
// supplies a world-space point and the victim's collider height; a test supplies
// two numbers. Keeping the rule here is what makes it possible to prove that a
// headshot costs twice as much without standing in front of a target.

#include <NF/Core/Types.hpp>
#include <NF/Runtime/Health.hpp>

#include <string_view>

namespace nf::runtime {

/// What kind of damage a hit carries. The names are scene-format names and are
/// deliberately a small closed set: a game that needs "acid" can map it to
/// fire or add a resistance, but the engine should not invent damage rules it
/// cannot test.
enum class DamageType : u8 {
    Ballistic = 0,
    Explosive = 1,
    Fire = 2,
};

/// The scene-format name: "ballistic", "explosive" or "fire".
[[nodiscard]] const char* damage_type_name(DamageType type) noexcept;

/// Parses a damage type. A refusal leaves `out` untouched, so a loader can
/// name the unknown value instead of silently treating it as ballistic.
[[nodiscard]] bool damage_type_from_name(std::string_view name, DamageType& out) noexcept;

/// Where on a target's collider the hit landed.
///
/// `Body` is the neutral answer and the one every old scene gets. The other two
/// only carry a multiplier when `HealthComponent::hit_zones` is true.
enum class HitZone : u8 {
    Body = 0,
    Head = 1,
    Limbs = 2,
};

/// The scene-format name of a zone: "body", "head" or "limbs".
[[nodiscard]] const char* hit_zone_name(HitZone zone) noexcept;

/// Parses a zone name. A refusal leaves `out` untouched, so a caller can report
/// the name it did not understand instead of silently treating it as a body hit.
[[nodiscard]] bool hit_zone_from_name(std::string_view name, HitZone& out) noexcept;

/// The top fraction of a collider that counts as the head, and the bottom
/// fraction that counts as limbs. Eighteen percent is roughly a human head on a
/// 1.8 m body; twenty-eight percent covers the legs without turning a crouched
/// target's whole torso into a limb hit.
inline constexpr f32 kHeadZoneFraction = 0.18f;
inline constexpr f32 kLimbZoneFraction = 0.28f;

/// Classifies a world-space Y against a collider's vertical extent.
///
/// A degenerate or non-finite extent returns `Body`: a target with no height
/// should not become an accidental headshot. The point is clamped to the
/// collider, so a ray that grazes the top face still lands in the head band.
[[nodiscard]] HitZone hit_zone_from_height(f32 point_y, f32 min_y, f32 max_y) noexcept;

/// What one accepted hit did.
struct DamageResult {
    /// The damage the caller offered, before zones, resistance and armor.
    f32 incoming = 0.0f;
    /// After the zone multiplier, before resistance and armor.
    f32 zoned = 0.0f;
    /// Damage removed by the target's resistance to this type.
    f32 resisted = 0.0f;
    /// Points removed from armor.
    f32 armor_absorbed = 0.0f;
    /// Points removed from health.
    f32 health_damage = 0.0f;
    /// False when the component refused the hit (disabled, invulnerable,
    /// invalid, or a non-positive amount). All damage fields are zero then.
    bool accepted = false;
    /// The victim had health before this hit and none after it.
    bool killed = false;
    /// The zone the caller passed, reported even when zones are disabled so an
    /// event can say where the geometry landed.
    HitZone zone = HitZone::Body;
    /// The damage type the caller passed.
    DamageType type = DamageType::Ballistic;
};

/// Applies one layered hit to a health component.
///
/// Contract:
///   * `health_apply_damage()` remains the raw path: it bypasses armor and
///     zones. This function is the weapon path.
///   * A zone multiplier applies only when `hit_zones` is true. Head and limb
///     multipliers are clamped by `health_valid()`; a bad value refuses the
///     whole component rather than producing a negative or NaN hit.
///   * While armor remains, `armor_absorb` of the protectable damage is taken
///     from armor first, limited by the armor left. Damage above that limit
///     flows through to health in the same hit.
///   * `armor_penetration` in [0, 1] bypasses that fraction of the protection.
///     A non-finite or out-of-range value is clamped, because a malformed
///     weapon must not turn a hit into NaN.
///   * The target's resistance to `type` removes its fraction after the zone
///     multiplier and before armor. A non-finite or out-of-range resistance is
///     refused by `health_valid()`.
///   * Any accepted hit resets `since_damage`, even when armor absorbed all of
///     it: regeneration is about being shot, not about losing health.
[[nodiscard]] DamageResult health_apply_hit(HealthComponent& health, f32 amount, HitZone zone,
                                            f32 armor_penetration = 0.0f,
                                            DamageType type = DamageType::Ballistic) noexcept;

/// The target's resistance to one damage type, clamped to [0, 1]. Zero for an
/// invalid component, so a malformed scene cannot make a hit stronger.
[[nodiscard]] f32 health_resistance(const HealthComponent& health, DamageType type) noexcept;

/// Armor as a fraction in [0, 1]. Zero when the component has no armor pool or
/// is invalid, so a HUD bar never divides by zero.
[[nodiscard]] f32 health_armor_fraction(const HealthComponent& health) noexcept;

} // namespace nf::runtime
