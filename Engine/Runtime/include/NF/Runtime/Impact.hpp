#pragma once

// NF/Runtime/Impact.hpp — what a shot hit, as a frame-local event.
//
// `last_hit` answers "where did the most recent round land"; it is one value
// and the next hit overwrites it. A game that wants a spark, a decal, a sound
// and a hit marker needs EVERY impact of the frame, in order, without polling
// the same slot and missing all but the last.
//
// This is a bounded, deterministic buffer with two slots:
//
//   * `current` is filled by the frame that is running (weapons fire after
//     scripts read their input);
//   * `previous` is the completed frame, which is what a script reads.
//
// begin_frame() moves current into previous and empties current. A script that
// fires a weapon itself therefore sees that impact on the NEXT frame, exactly
// like a trigger-driven shot, instead of having it cleared before anyone reads
// it. A host that draws a frame reads `current` after update(), when it is the
// frame just simulated.
//
// Capacity is a hard cap. A scene that fires thousands of pellets into a wall
// must not grow an unbounded log; the OLDEST events are dropped first, so the
// most recent fight is always the one a script sees.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>
#include <NF/Runtime/Damage.hpp>

#include <string>
#include <vector>

namespace nf::runtime {

/// One resolved hit. Both hitscan and projectile paths produce these.
struct ImpactEvent {
    ecs::Entity shooter{};
    ecs::Entity victim{};
    /// Empty when the victim has no name.
    std::string victim_name;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 0.0f};
    /// Path distance to the impact, in metres.
    f32 distance = 0.0f;
    /// Damage after falloff, before the victim's own rules.
    f32 damage = 0.0f;
    /// Damage actually removed from hit points.
    f32 applied = 0.0f;
    /// The shot struck a teammate and was stopped by faction rules.
    bool blocked_by_faction = false;
    /// The victim had hit points before the hit and none after it.
    bool killed = false;

    // --- Phase 50: where it landed and what armor stopped -------------------
    /// The geometric zone, even when the victim has zones disabled. A game can
    /// still play a different impact sound without changing the damage rules.
    HitZone hit_zone = HitZone::Body;
    /// Points taken from armor. `applied` remains the health damage, so an old
    /// consumer that only knows `applied` keeps meaning "what reached health".
    f32 armor_absorbed = 0.0f;
    /// True for a victim of a radial blast (Phase 51) rather than the entity
    /// the round struck. A game can play a different surface sound for it.
    bool splash = false;
    /// What kind of damage the hit carried (Phase 52).
    DamageType damage_type = DamageType::Ballistic;
    /// True for a damage-over-time tick rather than a direct or splash hit
    /// (Phase 54). `position` is the victim's transform origin.
    bool status = false;
};

/// The current and previous frame's impacts. Not thread-safe: the runtime is
/// single-threaded, like the rest of the frame.
class ImpactBuffer {
public:
    /// The most events kept per frame. At 3600 rpm a one-second burst is 60
    /// rounds, so 256 leaves room for a shotgun and several agents.
    static constexpr usize kCapacity = 256u;

    /// Adds one event, dropping the oldest when the cap is reached.
    void push(ImpactEvent event);

    /// Moves `current` into `previous` and clears `current`. Called once at
    /// the start of a runtime frame.
    void begin_frame();

    /// The frame that just ran. What a host draws.
    [[nodiscard]] const std::vector<ImpactEvent>& current() const { return m_current; }

    /// The completed frame. What a Lua script reads.
    [[nodiscard]] const std::vector<ImpactEvent>& previous() const { return m_previous; }

    void clear();

private:
    std::vector<ImpactEvent> m_current;
    std::vector<ImpactEvent> m_previous;
};

} // namespace nf::runtime
