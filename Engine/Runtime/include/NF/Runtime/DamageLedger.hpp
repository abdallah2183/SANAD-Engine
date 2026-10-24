#pragma once

// NF/Runtime/DamageLedger.hpp — who hurt me, how much, and how recently.
//
// `ImpactEvent` is frame-local: a game reads it, draws a spark, and it is gone.
// An NPC that was shot from behind needs the memory to survive the frame, and a
// HUD needs a stable "last attacker" that does not vanish when the next bullet
// lands somewhere else. This ledger is that memory.
//
// It is session state, not scene data. A copied entity starts with an empty
// ledger; a saved scene does not serialize it.

#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>

namespace nf::runtime {

/// Persistent damage memory for one entity.
struct DamageLedger {
    /// The last entity that damaged this one. Invalid for environmental damage
    /// or before the first hit.
    ecs::Entity last_attacker{};

    /// The entity that delivered the killing blow, when `killed` is true.
    ecs::Entity killer{};

    /// Accumulated damage. Armor is kept separate because "health removed" and
    /// "protection spent" are different numbers to a HUD and a balance test.
    f32 health_damage = 0.0f;
    f32 armor_damage = 0.0f;

    /// Accepted hits, including a hit whose damage was fully absorbed.
    u32 hits = 0u;

    /// Seconds since the last accepted hit. Advanced by the runtime.
    f32 seconds_since_damage = 0.0f;

    /// False until the first accepted hit; `seconds_since_damage` is meaningless
    /// before that.
    bool has_damage = false;

    /// True once this entity has been killed by a recorded hit.
    bool killed = false;
};

/// The ECS component. Created lazily on the first hit, so a scene with no
/// combat carries no ledger storage.
struct DamageLedgerComponent {
    DamageLedger ledger{};
};

/// Records one accepted hit. The last attacker is replaced, totals accumulate,
/// the quiet clock resets, and a killing hit names the killer.
void damage_ledger_record(DamageLedger& ledger, ecs::Entity attacker, f32 health_damage,
                          f32 armor_damage, bool killed) noexcept;

/// Advances the quiet clock. `dt <= 0` changes nothing.
void damage_ledger_step(DamageLedger& ledger, f32 dt) noexcept;

} // namespace nf::runtime
