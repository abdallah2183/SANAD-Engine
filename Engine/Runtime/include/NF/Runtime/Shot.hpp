#pragma once

// NF/Runtime/Shot.hpp — a frame-local record of what left the barrel.
//
// `ImpactEvent` says what a shot HIT. A game also needs what was FIRED: a
// muzzle flash at the origin, a tracer along the direction, a casing sound per
// pellet, a recoil shake that is not tied to a hit. A weapon that misses still
// fired, and a game that only listens to impacts cannot show that.
//
// One event is produced per pellet, at the moment the pellet is created. The
// event is a snapshot, not a live object: the profile can change after the
// shot without rewriting history.
//
// The buffer keeps the same two slots as ImpactBuffer:
//
//   * `current` is the frame that is running;
//   * `previous` is the completed frame a Lua script reads.
//
// A script that fires a weapon itself therefore sees that shot on the next
// frame, exactly like a trigger-driven one, instead of having it cleared
// before it can react.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>

#include <vector>

namespace nf::runtime {

/// One pellet or projectile leaving a weapon.
struct ShotEvent {
    ecs::Entity shooter{};
    /// World-space muzzle origin. For a player shot this is the camera; for an
    /// agent it is the eye. The renderer can put a flash here.
    Vec3 origin{0.0f, 0.0f, 0.0f};
    /// Unit direction after spread. A tracer follows this, not the camera's
    /// unspread aim, because the pellet is what the game sees.
    Vec3 direction{0.0f, 0.0f, -1.0f};
    /// The weapon's damage at the muzzle, before falloff. A HUD or an audio
    /// layer can use it without knowing the profile layout.
    f32 damage = 0.0f;
    /// 1-based index of this pellet in the volley, and the volley's size.
    u32 pellet = 1u;
    u32 pellets = 1u;
    /// True for a projectile with flight time, false for a hitscan round.
    bool projectile = false;
    /// The profile's deterministic spread seed, for replay/debug overlays.
    u32 seed = 0u;
};

/// The current and previous frame's shots. Not thread-safe: the runtime is
/// single-threaded, like the rest of the frame.
class ShotBuffer {
public:
    /// The most shots kept per frame. A 3600 rpm weapon is 60 rounds per
    /// second, and a shotgun can add pellets; 256 is the same generous bound
    /// the impact buffer uses.
    static constexpr usize kCapacity = 256u;

    void push(ShotEvent event);

    /// Moves `current` into `previous` and clears `current`.
    void begin_frame();

    [[nodiscard]] const std::vector<ShotEvent>& current() const { return m_current; }
    [[nodiscard]] const std::vector<ShotEvent>& previous() const { return m_previous; }

    void clear();

private:
    std::vector<ShotEvent> m_current;
    std::vector<ShotEvent> m_previous;
};

} // namespace nf::runtime
