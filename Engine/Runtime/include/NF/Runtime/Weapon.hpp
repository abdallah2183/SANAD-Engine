#pragma once

// NF/Runtime/Weapon.hpp — a gun, as arithmetic.
//
// The engine could already aim a camera (Phase 30), aim down sights and take a
// recoil kick (Phase 33). What it could not do was SHOOT: a game had to write
// the fire rate, the magazine, the spread and the reload clock itself, in Lua,
// per weapon, and every game got a different set of bugs. This is the part that
// is the same for every gun, so it can be written once and tested.
//
// The split is the same one CameraFollow and PlayerLook made:
//
//   * WeaponProfile is what a LEVEL authors — rate of fire, magazine, damage,
//     spread, recoil. It is scene data and round-trips through the file.
//   * WeaponState is what a SESSION accumulates — rounds left, the cooldown
//     clock, the burst queue, the current spread bloom. It is never saved:
//     a loaded game starts with a full magazine because the scene says so.
//   * weapon_step() is the frame: it reads a profile, a state and two booleans
//     of input, and returns the shots fired THIS frame plus what happened.
//
// Nothing here knows about the ECS, the physics world, the renderer or the
// clock. A shot is an ANGLE OFFSET, not a ray: turning the offset into a hit is
// the runtime's job (Hitscan.hpp), and keeping them apart is what lets the
// whole fire-rate/reload/spread machine be tested without a device.
//
// Determinism is deliberate. The spread uses a 32-bit xorshift seeded from the
// profile, so the same trigger sequence produces the same pattern on every run
// and on every machine — the same contract the physics solver keeps, and the
// reason a recorded input log can reproduce a firefight.

#include <NF/Core/Types.hpp>
#include <NF/Runtime/Damage.hpp>
#include <NF/Runtime/StatusEffect.hpp>

#include <string_view>
#include <vector>

namespace nf::runtime {

/// How the trigger behaves.
///
/// The three modes are not three code paths; they are three answers to "does
/// the trigger ask for a shot this frame". Semi asks once per press, Auto asks
/// while held, Burst asks `burst_count` times and then stops until the trigger
/// is released and pressed again.
enum class FireMode : u8 {
    Semi = 0,
    Auto = 1,
    Burst = 2,
};

/// The scene-format name of a mode: "semi", "auto", "burst".
[[nodiscard]] const char* fire_mode_name(FireMode mode) noexcept;

/// Parses a scene-format name. Returns false (leaving `out` untouched) for an
/// unknown name, so the loader can warn about the key it did not understand
/// instead of quietly promoting a typo to Semi.
[[nodiscard]] bool fire_mode_from_name(std::string_view name, FireMode& out) noexcept;

/// How a shot reaches its target.
///
/// Hitscan is the Phase 44 default: a ray is traced and damage lands in the
/// frame the trigger fired. Projectile gives the shot a body with a muzzle
/// speed, a gravity drop and a real flight time (Projectile.hpp). The trigger,
/// magazine, spread, recoil and falloff are identical for both — this is a
/// property of the ammunition, not of the weapon's state machine.
enum class WeaponDelivery : u8 {
    Hitscan = 0,
    Projectile = 1,
};

/// The scene-format name of a delivery: "hitscan" or "projectile".
[[nodiscard]] const char* weapon_delivery_name(WeaponDelivery delivery) noexcept;

/// Parses a delivery name. A refusal leaves `out` untouched, so the loader can
/// name the bad key instead of silently firing a different kind of round.
[[nodiscard]] bool weapon_delivery_from_name(std::string_view name,
                                             WeaponDelivery& out) noexcept;

/// One weapon, as a level authors it.
///
/// Every field is a number a designer can defend. The defaults describe the
/// engine's idea of a service rifle — semi/auto, 420 rounds per minute, a
/// twelve-round magazine, a little spread and a little climb — because a
/// `Weapon:` line with no keys should still shoot something sensible.
struct WeaponProfile {
    FireMode mode = FireMode::Semi;

    /// Rounds per minute. The interval between shots is 60/rpm; a value that
    /// is not finite and positive makes the profile invalid (see
    /// weapon_valid) rather than firing every frame.
    f32 rounds_per_minute = 420.0f;

    /// Rounds in a full magazine, and the most a reload can put back.
    u32 magazine = 12u;

    /// Rounds carried outside the magazine. A reload draws from here.
    u32 reserve = 60u;

    /// Seconds a reload takes. Zero or less completes on the frame it starts,
    /// which is a legitimate "no reload animation" weapon.
    f32 reload_seconds = 1.6f;

    /// Shots in one burst, and the gap between them. Burst mode ignores
    /// `rounds_per_minute`: the gap IS the cadence, because a three-round burst
    /// is a rhythm, not a rate.
    u32 burst_count = 3u;
    f32 burst_gap_seconds = 0.07f;

    /// Damage at the muzzle, before distance falloff.
    f32 damage = 24.0f;

    /// What kind of damage the round carries (Phase 52). Ballistic is the
    /// default and the only value a pre-Phase-52 scene has. A target's matching
    /// resistance reduces the hit before armor; a blast uses the same type as
    /// the weapon that fired it, so `damage_type=fire` makes a fire grenade.
    DamageType damage_type = DamageType::Ballistic;

    /// Farthest a shot can reach, in metres.
    f32 range = 150.0f;

    /// Hitscan (the default) or a projectile with travel time. A scene that
    /// never authors this key keeps the exact Phase 44 behaviour.
    WeaponDelivery delivery = WeaponDelivery::Hitscan;

    /// Muzzle speed for a projectile, in metres per second. Ignored by
    /// hitscan. The flight lifetime is `range / projectile_speed`, so range
    /// remains the hard reach of the weapon rather than a second opinion.
    f32 projectile_speed = 420.0f;

    /// Downward acceleration for a projectile, in metres per second squared.
    /// 9.81 is real gravity; 0 is a straight-flying energy bolt. Ignored by
    /// hitscan. This is a scalar rather than a vector because a scene should
    /// not be able to author sideways bullet drift by accident.
    f32 projectile_gravity = 9.81f;

    /// Projectiles per trigger pull. Above one is a shotgun: each pellet gets
    /// its own spread offset and its own hit, so a blast can hit two targets.
    u32 pellets = 1u;

    /// Radial blast (Phase 51). Zero radius disables it, which is the default
    /// and the pre-Phase-51 behaviour. `blast_damage` is the centre damage and
    /// `blast_min_scale` the fraction left at the edge; the runtime applies the
    /// blast at the impact point to every other entity in range.
    f32 blast_radius = 0.0f;
    f32 blast_damage = 0.0f;
    f32 blast_min_scale = 0.0f;

    /// Status applied to a living victim on a hit (Phase 54). `None` is the
    /// default. A burning round authors `status=burn status_dps=4
    /// status_time=3 status_tick=0.5`; the same status is applied by a direct
    /// hit and by every splash victim, because both are hits from this weapon.
    StatusKind status = StatusKind::None;
    f32 status_damage_per_second = 0.0f;
    f32 status_seconds = 0.0f;
    f32 status_tick_seconds = 0.5f;

    /// The cone, in degrees: the radius of the disk a pellet can land in when
    /// the weapon is cold.
    f32 spread_deg = 0.4f;

    /// Spread added per shot, and the ceiling the bloom may reach. Bloom is
    /// what makes a held trigger less accurate than a tapped one; recovering
    /// at `spread_recover_deg_per_s` is what makes a player wait.
    f32 spread_bloom_deg = 0.25f;
    f32 spread_max_deg = 4.0f;
    f32 spread_recover_deg_per_s = 6.0f;

    /// Recoil per shot, in degrees. Positive pitch raises the view, positive
    /// yaw turns it left — the conventions PlayerLook already uses.
    f32 recoil_pitch = 0.85f;
    f32 recoil_yaw = 0.22f;

    /// How much each successive shot in a burst climbs, as a fraction of the
    /// first. 0.18 means the second shot kicks 18% harder than the first; the
    /// pattern is capped at kMaxRecoilGrowth shots so a long burst cannot
    /// invent an unbounded kick.
    f32 recoil_growth = 0.18f;

    /// Distance where damage starts to fall off, the distance where it has
    /// fallen to `falloff_min_scale`, and that floor. `falloff_end <=
    /// falloff_start` means "no falloff", which is the honest reading of a
    /// scene that authored one number.
    f32 falloff_start = 35.0f;
    f32 falloff_end = 120.0f;
    f32 falloff_min_scale = 0.5f;

    /// Fraction of a hit that bypasses the target's armor, in [0, 1]. 0 is an
    /// ordinary round; 1 is a round that ignores armor entirely. The value is
    /// clamped at runtime and refused by the loader outside the range, so an
    /// old scene's absent key means no penetration.
    f32 armor_penetration = 0.0f;

    /// Camera trauma per shot, in [0, 1]. Small on purpose: a rifle is a
    /// 0.1, an explosion is a 0.6. The camera's own trim scales it.
    f32 shake = 0.12f;

    /// The spread's seed. Two weapons with the same seed and the same trigger
    /// sequence produce the same pattern, which is what makes a recorded
    /// firefight reproducible.
    u32 seed = 0x5EEDBEEFu;

    /// Start a reload automatically when the trigger is pulled on an empty
    /// magazine. The empty click still reports, so a script can play it.
    bool auto_reload = true;

    /// Off: the component survives, the entity keeps its placement, and the
    /// weapon never fires. The same "switch it off, keep it placed" contract
    /// every other optional component keeps.
    bool enabled = true;
};

/// The most shots a recoil pattern ramps over. Past this the kick is constant:
/// a hundred-round belt should not end pointing at the sky.
inline constexpr u32 kMaxRecoilGrowthShots = 8u;

/// The fastest cadence the clock can represent, in rounds per minute. A scene
/// asking for more is clamped rather than firing more than once per frame at
/// 60 fps, which would make the rate depend on the frame rate.
inline constexpr f32 kMaxRoundsPerMinute = 3600.0f;

/// What happened this frame. Events are reported in the order they happened,
/// because "reload finished and then the trigger fired" is a different frame
/// from "the trigger fired and then a reload started".
enum class WeaponEventKind : u8 {
    /// A volley left the barrel. `value` is the 1-based shot index within the
    /// current trigger pull (1 for the first shot after a release).
    Fired = 0,
    /// The trigger was pulled on an empty magazine and nothing was fired. Sent
    /// once per pull, not once per frame.
    Empty = 1,
    ReloadStarted = 2,
    /// `value` is the rounds actually loaded, which may be fewer than the
    /// magazine holds when the reserve runs out.
    ReloadFinished = 3,
};

struct WeaponEvent {
    WeaponEventKind kind = WeaponEventKind::Fired;
    u32 value = 0u;
};

/// One projectile's angular offset from the aim, in degrees. The runtime adds
/// these to the camera's yaw/pitch; the maths never touches a direction vector,
/// so a weapon's accuracy does not depend on where the player is standing.
struct WeaponShot {
    f32 yaw_deg = 0.0f;
    f32 pitch_deg = 0.0f;
};

/// A weapon's live state. Session data: never written to a scene file.
struct WeaponState {
    u32 in_magazine = 0u;
    u32 reserve = 0u;

    /// Seconds until the next shot is allowed. Counts down every frame, so the
    /// cadence is a function of time and not of the frame rate.
    f32 cooldown = 0.0f;

    /// Seconds left in the current reload. Only meaningful while `reloading`.
    f32 reload_timer = 0.0f;

    /// Shots left in the current burst, and the gap before the next one.
    u32 burst_left = 0u;
    f32 burst_timer = 0.0f;

    /// Shots fired since the trigger was last released. Drives the recoil ramp
    /// and is reset on release, so the second burst of a magazine starts from
    /// the same place as the first.
    u32 shot_index = 0u;

    /// Current bloom, in degrees, added to the profile's base spread.
    f32 bloom_deg = 0.0f;

    /// The spread generator's state. Advanced only when a pellet is placed, so
    /// a weapon that never fires consumes no randomness.
    u32 rng = 0u;

    bool reloading = false;

    /// Last frame's trigger, for edge detection. Semi and burst need the
    /// press, not the hold.
    bool trigger_was_held = false;

    /// A press that has not been served yet. Buffered so a click that lands
    /// while the weapon is still cooling down is fired as soon as it can be,
    /// instead of being swallowed — the usual reason a semi-auto feels like it
    /// dropped an input.
    bool trigger_queued = false;

    /// Whether the empty click has been reported for this pull.
    bool empty_clicked = false;
};

/// The two booleans a frame of input reduces to.
struct WeaponInput {
    /// True while the fire button is held.
    bool trigger_held = false;
    /// True on the frame the reload button went down. A held reload does not
    /// restart the reload every frame.
    bool reload_pressed = false;
};

/// The result of one frame: the shots to trace and what to tell the player.
struct WeaponStep {
    std::vector<WeaponShot> shots;
    std::vector<WeaponEvent> events;

    /// True when this frame's input was the press that started the current
    /// pull. The runtime uses it for the empty click's sound, and a test uses
    /// it to prove edge detection works.
    bool trigger_edge = false;
};

/// A fresh state for a profile: full magazine, full reserve, no timers.
[[nodiscard]] WeaponState weapon_state(const WeaponProfile& profile);

/// Whether a profile can fire at all.
///
/// Refused rather than repaired: a magazine of zero, a cadence of zero or a
/// range of zero is a scene bug, and a weapon that silently fires at the
/// engine's default rate hides it. The loader refuses the line; this is the
/// second gate, for a profile a game built in C++.
[[nodiscard]] bool weapon_valid(const WeaponProfile& profile) noexcept;

/// Seconds between shots, clamped to a cadence the clock can represent.
[[nodiscard]] f32 weapon_interval(const WeaponProfile& profile) noexcept;

/// The current cone radius: base spread plus bloom, capped at `spread_max_deg`.
[[nodiscard]] f32 weapon_spread_deg(const WeaponState& state,
                                    const WeaponProfile& profile) noexcept;

/// Damage a hit at `distance` takes, after falloff.
[[nodiscard]] f32 weapon_damage_at(const WeaponProfile& profile, f32 distance) noexcept;

/// The kick one shot adds, in degrees.
struct RecoilKick {
    f32 pitch = 0.0f;
    f32 yaw = 0.0f;
};

/// The kick for the `shot_index`-th shot of a pull (0-based).
///
/// Deterministic and readable: the pitch ramps by `recoil_growth` per shot up to
/// kMaxRecoilGrowthShots, and the yaw alternates sign so a burst climbs in a
/// narrow zig-zag rather than drifting off to one side. A scene that wants a
/// real spray pattern drives `nf.recoil` itself; this is the shape that is
/// better than "no pattern" for the weapon nobody tuned.
[[nodiscard]] RecoilKick weapon_recoil_kick(const WeaponProfile& profile,
                                            u32 shot_index) noexcept;

/// Starts a reload if there is anything to load. Returns true when one began
/// (or completed instantly), and appends the events to `out`.
bool weapon_begin_reload(WeaponState& state, const WeaponProfile& profile, WeaponStep& out);

/// Tops the reserve up to the profile's capacity (an ammo pickup). Returns the
/// rounds actually added.
u32 weapon_refill(WeaponState& state, const WeaponProfile& profile) noexcept;

/// Advances one frame.
///
/// The order inside is the contract, because each step depends on the previous
/// one: clocks first (so a cooldown that expired this frame is usable), then a
/// reload that may complete (so its rounds can be fired this frame), then the
/// trigger (so a shot uses the state the frame actually has), then the release
/// bookkeeping (so a release after a shot does not clear the shot's own index).
///
/// `dt <= 0` advances nothing and fires nothing: a paused frame must not empty
/// a magazine. A non-finite `dt` is treated the same way.
[[nodiscard]] WeaponStep weapon_step(WeaponState& state, const WeaponProfile& profile,
                                     const WeaponInput& input, f32 dt);

/// Fires one volley NOW, without a trigger.
///
/// For a script that shoots on a timer, and for a test that cannot press a key.
/// The fire-rate clock and the magazine still apply — this bypasses the TRIGGER,
/// not the weapon — so calling it every frame produces the profile's cadence
/// rather than a shot per frame. Returns false when nothing was fired (cooling
/// down, reloading, empty, or an invalid profile); `out` still carries the
/// Empty/reload events that explain why.
bool weapon_fire_now(WeaponState& state, const WeaponProfile& profile, WeaponStep& out);

} // namespace nf::runtime
