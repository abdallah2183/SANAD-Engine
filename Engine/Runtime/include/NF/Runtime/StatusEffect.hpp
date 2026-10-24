#pragma once

// NF/Runtime/StatusEffect.hpp — damage that keeps arriving after the hit.
//
// A fire round that only removes hit points once is a different colour of
// bullet. Burning, bleeding and corrosion are the same hit over time, and they
// need three rules the direct path does not:
//
//   1. APPLY — a hit adds or refreshes an effect on the victim;
//   2. TICK — damage is delivered at a stable interval, not once per frame, so
//      the same effect costs the same at 30 or 144 fps;
//   3. EXPIRE — the effect removes itself, including a partial final tick, so
//      a 0.75 s burn with a 0.5 s tick costs exactly 0.75 s of damage.
//
// This file owns the arithmetic and the small fixed-capacity set. The runtime
// owns the world: which health component receives the tick, the faction check,
// armor/resistance, and the impact event a game draws.

#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>
#include <NF/Runtime/Damage.hpp>

#include <array>
#include <string_view>
#include <vector>

namespace nf::runtime {

/// The closed set of status effects the engine understands.
///
/// `None` exists only for authoring ("this weapon applies no status") and is
/// never stored in an active set.
enum class StatusKind : u8 {
    None = 0,
    Burning = 1,
    Bleeding = 2,
    Corroding = 3,
};

/// Scene-format name: "none", "burn", "bleed" or "corrode".
[[nodiscard]] const char* status_kind_name(StatusKind kind) noexcept;

/// Parses a status name. A refusal leaves `out` untouched, so a weapon line
/// with `status=poison` is rejected instead of silently becoming no status.
[[nodiscard]] bool status_kind_from_name(std::string_view name, StatusKind& out) noexcept;

/// The damage channel a status ticks with.
///
/// Burning is fire, bleeding is ballistic, corrosion is explosive. The mapping
/// is fixed because a status is a gameplay concept, not a second damage type;
/// a target's existing resistance to the channel applies to every tick.
[[nodiscard]] DamageType status_damage_type(StatusKind kind) noexcept;

/// The largest number of active effects one entity may carry. With three
/// kinds this is headroom for future kinds and for a game that adds one
/// without changing the component layout.
inline constexpr usize kMaxStatusEffects = 8u;

/// One active effect. Session state except for the authored fields (kind, dps,
/// duration, tick) which can also come from a scene line.
struct StatusEffect {
    StatusKind kind = StatusKind::Burning;
    f32 damage_per_second = 0.0f;
    f32 remaining_seconds = 0.0f;
    f32 tick_seconds = 0.5f;
    /// Time accumulated toward the next tick. Session state; never saved.
    f32 tick_accumulator = 0.0f;
    /// Who applied it, for faction rules and attribution. Invalid for a scene
    /// authored effect, which is environmental.
    ecs::Entity source{};
};

/// A fixed-capacity set. No allocation, so it can live inside an ECS component.
struct StatusEffectSet {
    std::array<StatusEffect, kMaxStatusEffects> effects{};
    u32 count = 0u;
};

/// The ECS component. `enabled=false` freezes the set: no tick and no new
/// effect, the same "switch it off, keep it placed" contract as every other
/// optional component.
struct StatusEffectComponent {
    StatusEffectSet effects{};
    bool enabled = true;
};

/// A fresh effect with the tick accumulator at zero.
[[nodiscard]] StatusEffect make_status(StatusKind kind, f32 damage_per_second,
                                       f32 duration_seconds, f32 tick_seconds = 0.5f,
                                       ecs::Entity source = {}) noexcept;

/// Whether an effect can tick. Refuses None, non-positive or non-finite
/// numbers, and a non-finite accumulator.
[[nodiscard]] bool status_valid(const StatusEffect& effect) noexcept;

/// Adds or refreshes an effect. Same kind refreshes instead of stacking:
/// damage per second and duration take the stronger value, the tick interval
/// comes from the newest application, and the source is kept when the new one
/// is invalid. Returns false for an invalid effect.
bool status_add(StatusEffectSet& set, const StatusEffect& effect) noexcept;

/// Removes one kind. Returns true when it was present.
bool status_remove(StatusEffectSet& set, StatusKind kind) noexcept;

/// The active effect of this kind, or null.
[[nodiscard]] const StatusEffect* status_find(const StatusEffectSet& set,
                                              StatusKind kind) noexcept;

/// Seconds left for one kind, or 0 when absent.
[[nodiscard]] f32 status_remaining(const StatusEffectSet& set, StatusKind kind) noexcept;

/// One damage tick produced by `status_step()`.
struct StatusTick {
    StatusKind kind = StatusKind::Burning;
    DamageType damage_type = DamageType::Fire;
    f32 damage = 0.0f;
    ecs::Entity source{};
};

/// Advances every effect by `dt` and appends its ticks to `out`.
///
/// The output is cleared first. A step of 0.75 s on a 0.5 s tick emits one full
/// tick and one partial tick; a step that crosses several ticks emits each of
/// them. Expired effects are removed. `dt <= 0` advances nothing.
///
/// Returns the number of ticks appended.
u32 status_step(StatusEffectSet& set, f32 dt, std::vector<StatusTick>& out) noexcept;

/// Drops every effect.
void status_clear(StatusEffectSet& set) noexcept;

} // namespace nf::runtime
