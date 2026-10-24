#include <NF/Runtime/Weapon.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf::runtime {

namespace {

// xorshift32. Chosen over <random> on purpose: the generator is part of the
// determinism contract, and a standard library distribution is allowed to
// change between releases. Three shifts and a xor are also what a shot costs.
inline u32 next_random(u32& state) {
    u32 x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

// Uniform in [0, 1). 24 bits is every value an f32 can hold exactly, so the
// conversion is lossless and the same seed produces the same float on every
// machine.
inline f32 uniform01(u32& state) {
    return static_cast<f32>(next_random(state) >> 8) * (1.0f / 16777216.0f);
}

inline bool finite_positive(f32 v) { return std::isfinite(v) && v > 0.0f; }

inline bool finite_non_negative(f32 v) { return std::isfinite(v) && v >= 0.0f; }

// The spread seed must never be zero: xorshift32 is stuck at zero forever, and
// a weapon whose every shot lands dead centre would look like a bug rather
// than a pattern.
inline u32 sanitized_seed(u32 seed) { return seed == 0u ? 0x9E3779B9u : seed; }

// One volley: spend a round, place the pellets, and set the clocks. Shared by
// the trigger path and by weapon_fire_now so a script and a key press cannot
// disagree about what a shot costs.
void fire_one_volley(WeaponState& state, const WeaponProfile& profile, WeaponStep& out) {
    --state.in_magazine;
    ++state.shot_index;
    state.cooldown = weapon_interval(profile);
    state.bloom_deg =
        std::min(profile.spread_max_deg, state.bloom_deg + profile.spread_bloom_deg);
    state.trigger_queued = false;

    if (profile.mode == FireMode::Burst && state.burst_left > 0u) {
        --state.burst_left;
        state.burst_timer = profile.burst_gap_seconds;
    }

    const f32 spread = weapon_spread_deg(state, profile);
    for (u32 pellet = 0; pellet < profile.pellets; ++pellet) {
        // Uniform over the DISK, not over the square: sampling the radius
        // directly would cluster pellets at the centre, which reads as a
        // weapon that is more accurate than it says.
        const f32 radius = spread * std::sqrt(uniform01(state.rng));
        const f32 angle = 6.28318530718f * uniform01(state.rng);
        WeaponShot shot;
        shot.yaw_deg = radius * std::cos(angle);
        shot.pitch_deg = radius * std::sin(angle);
        out.shots.push_back(shot);
    }
    out.events.push_back(WeaponEvent{WeaponEventKind::Fired, state.shot_index});
}

} // namespace

const char* fire_mode_name(FireMode mode) noexcept {
    switch (mode) {
    case FireMode::Semi: return "semi";
    case FireMode::Auto: return "auto";
    case FireMode::Burst: return "burst";
    }
    return "semi";
}

bool fire_mode_from_name(std::string_view name, FireMode& out) noexcept {
    if (name == "semi") {
        out = FireMode::Semi;
        return true;
    }
    if (name == "auto") {
        out = FireMode::Auto;
        return true;
    }
    if (name == "burst") {
        out = FireMode::Burst;
        return true;
    }
    return false;
}

const char* weapon_delivery_name(WeaponDelivery delivery) noexcept {
    switch (delivery) {
    case WeaponDelivery::Hitscan: return "hitscan";
    case WeaponDelivery::Projectile: return "projectile";
    }
    return "hitscan";
}

bool weapon_delivery_from_name(std::string_view name, WeaponDelivery& out) noexcept {
    if (name == "hitscan") {
        out = WeaponDelivery::Hitscan;
        return true;
    }
    if (name == "projectile") {
        out = WeaponDelivery::Projectile;
        return true;
    }
    return false;
}

bool weapon_valid(const WeaponProfile& profile) noexcept {
    if (!profile.enabled) {
        return false;
    }
    if (profile.magazine == 0u || profile.pellets == 0u) {
        return false;
    }
    if (!finite_positive(profile.rounds_per_minute) || !finite_positive(profile.range)) {
        return false;
    }
    if (!finite_non_negative(profile.reload_seconds) ||
        !finite_non_negative(profile.damage) ||
        !finite_non_negative(profile.spread_deg) ||
        !finite_non_negative(profile.spread_bloom_deg) ||
        !finite_non_negative(profile.spread_recover_deg_per_s) ||
        !finite_non_negative(profile.recoil_pitch) ||
        !finite_non_negative(profile.recoil_yaw) ||
        !finite_non_negative(profile.recoil_growth) ||
        !finite_non_negative(profile.armor_penetration) ||
        profile.armor_penetration > 1.0f ||
        !finite_non_negative(profile.blast_radius) ||
        !finite_non_negative(profile.blast_damage) ||
        !finite_non_negative(profile.blast_min_scale) ||
        profile.blast_min_scale > 1.0f ||
        !finite_non_negative(profile.shake)) {
        return false;
    }
    // A cone narrower than its own bloom is a profile that lies about its
    // accuracy, so the ceiling must be at least the floor.
    if (!finite_positive(profile.spread_max_deg) || profile.spread_max_deg < profile.spread_deg) {
        return false;
    }
    if (profile.mode == FireMode::Burst) {
        if (profile.burst_count == 0u || !finite_non_negative(profile.burst_gap_seconds)) {
            return false;
        }
    }
    if (profile.status != StatusKind::None) {
        // A named status without a positive dose, duration or tick is a scene
        // typo, not an invisible effect. Refuse the whole profile.
        if (!finite_positive(profile.status_damage_per_second) ||
            !finite_positive(profile.status_seconds) ||
            !finite_positive(profile.status_tick_seconds)) {
            return false;
        }
    }
    if (profile.delivery == WeaponDelivery::Projectile) {
        // A projectile with no speed never leaves the barrel; a negative
        // gravity is a bullet that falls upward. Refuse both rather than
        // spawning a body that will misbehave for the rest of the session.
        if (!finite_positive(profile.projectile_speed) ||
            !finite_non_negative(profile.projectile_gravity)) {
            return false;
        }
    }
    return true;
}

f32 weapon_interval(const WeaponProfile& profile) noexcept {
    const f32 rpm = std::clamp(profile.rounds_per_minute, 1.0f, kMaxRoundsPerMinute);
    return 60.0f / rpm;
}

WeaponState weapon_state(const WeaponProfile& profile) {
    WeaponState state;
    state.in_magazine = profile.magazine;
    state.reserve = profile.reserve;
    state.rng = sanitized_seed(profile.seed);
    return state;
}

f32 weapon_spread_deg(const WeaponState& state, const WeaponProfile& profile) noexcept {
    const f32 spread = profile.spread_deg + state.bloom_deg;
    return std::clamp(spread, 0.0f, profile.spread_max_deg);
}

f32 weapon_damage_at(const WeaponProfile& profile, f32 distance) noexcept {
    if (!std::isfinite(distance) || distance <= profile.falloff_start) {
        return profile.damage;
    }
    // A scene that authored no falloff (end <= start) gets full damage at any
    // range: the alternative is a division by zero dressed as a curve.
    if (!(profile.falloff_end > profile.falloff_start)) {
        return profile.damage;
    }
    if (distance >= profile.falloff_end) {
        return profile.damage * profile.falloff_min_scale;
    }
    const f32 t = (distance - profile.falloff_start) /
                  (profile.falloff_end - profile.falloff_start);
    const f32 scale = 1.0f + (profile.falloff_min_scale - 1.0f) * t;
    return profile.damage * scale;
}

RecoilKick weapon_recoil_kick(const WeaponProfile& profile, u32 shot_index) noexcept {
    const u32 index = std::min(shot_index, kMaxRecoilGrowthShots);
    const f32 scale = 1.0f + profile.recoil_growth * static_cast<f32>(index);
    const f32 direction = (shot_index % 2u == 0u) ? 1.0f : -1.0f;
    RecoilKick kick;
    kick.pitch = profile.recoil_pitch * scale;
    kick.yaw = profile.recoil_yaw * scale * direction;
    return kick;
}

bool weapon_begin_reload(WeaponState& state, const WeaponProfile& profile, WeaponStep& out) {
    if (!weapon_valid(profile) || state.reloading) {
        return false;
    }
    if (state.in_magazine >= profile.magazine || state.reserve == 0u) {
        return false;
    }
    // A queued press must not survive into the reload: the player pressed
    // while the magazine was full enough to fire, and firing is what they get
    // when the reload ends.
    state.trigger_queued = false;
    state.burst_left = 0u;
    state.burst_timer = 0.0f;
    out.events.push_back(WeaponEvent{WeaponEventKind::ReloadStarted, 0u});

    if (!(profile.reload_seconds > 0.0f)) {
        // Instant reload: load now, in the same frame, so a scene that asked
        // for no reload time does not spend one frame unable to fire.
        const u32 need = profile.magazine - state.in_magazine;
        const u32 take = std::min(need, state.reserve);
        state.in_magazine += take;
        state.reserve -= take;
        out.events.push_back(WeaponEvent{WeaponEventKind::ReloadFinished, take});
        return true;
    }

    state.reloading = true;
    state.reload_timer = profile.reload_seconds;
    return true;
}

u32 weapon_refill(WeaponState& state, const WeaponProfile& profile) noexcept {
    if (state.reserve >= profile.reserve) {
        return 0u;
    }
    const u32 added = profile.reserve - state.reserve;
    state.reserve = profile.reserve;
    return added;
}

WeaponStep weapon_step(WeaponState& state, const WeaponProfile& profile,
                       const WeaponInput& input, f32 dt) {
    WeaponStep out;
    if (!weapon_valid(profile)) {
        return out;
    }
    // A paused frame (or a NaN one) advances nothing and fires nothing. The
    // trigger edge is deliberately NOT consumed either: the press will be seen
    // on the first frame that actually runs.
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return out;
    }

    // --- 1. Clocks ---------------------------------------------------------
    if (state.cooldown > 0.0f) {
        state.cooldown = std::max(0.0f, state.cooldown - dt);
    }
    if (state.burst_timer > 0.0f) {
        state.burst_timer = std::max(0.0f, state.burst_timer - dt);
    }
    if (state.bloom_deg > 0.0f) {
        state.bloom_deg =
            std::max(0.0f, state.bloom_deg - profile.spread_recover_deg_per_s * dt);
    }

    // --- 2. A reload that finishes this frame ------------------------------
    if (state.reloading) {
        state.reload_timer -= dt;
        if (state.reload_timer <= 0.0f) {
            const u32 need = profile.magazine - state.in_magazine;
            const u32 take = std::min(need, state.reserve);
            state.in_magazine += take;
            state.reserve -= take;
            state.reloading = false;
            state.reload_timer = 0.0f;
            out.events.push_back(WeaponEvent{WeaponEventKind::ReloadFinished, take});
        }
    }

    // --- 3. The trigger ----------------------------------------------------
    const bool pressed = input.trigger_held && !state.trigger_was_held;
    const bool released = !input.trigger_held && state.trigger_was_held;
    out.trigger_edge = pressed;

    if (pressed) {
        state.trigger_queued = true;
        if (profile.mode == FireMode::Burst && state.burst_left == 0u) {
            state.burst_left = profile.burst_count;
            state.burst_timer = 0.0f;
        }
    }
    if (input.reload_pressed) {
        weapon_begin_reload(state, profile, out);
    }

    // --- 4. Does the trigger ask for a shot? -------------------------------
    bool wants = false;
    if (!state.reloading) {
        switch (profile.mode) {
        case FireMode::Semi:
            wants = state.trigger_queued;
            break;
        case FireMode::Auto:
            wants = input.trigger_held;
            break;
        case FireMode::Burst:
            wants = state.burst_left > 0u;
            break;
        }
    }

    if (wants && state.cooldown <= 0.0f && state.burst_timer <= 0.0f) {
        if (state.in_magazine == 0u) {
            // The click is reported once per pull. A held trigger on an empty
            // gun must not write an event every frame: that is a log nobody
            // can read and a sound nobody wants to hear.
            if (!state.empty_clicked) {
                out.events.push_back(WeaponEvent{WeaponEventKind::Empty, 0u});
                state.empty_clicked = true;
            }
            state.trigger_queued = false;
            if (profile.auto_reload) {
                weapon_begin_reload(state, profile, out);
            }
        } else {
            fire_one_volley(state, profile, out);
        }
    }

    // --- 5. Release --------------------------------------------------------
    if (released) {
        state.trigger_queued = false;
        state.empty_clicked = false;
        // A burst in flight is NOT cancelled: three rounds is a promise the
        // weapon made when the trigger went down. `burst_left` therefore
        // survives the release, and the next press starts a new burst only
        // after the current one has finished. The shot index is kept with it,
        // so the ramp continues across the release instead of restarting in
        // the middle of a burst.
        if (state.burst_left == 0u) {
            state.shot_index = 0u;
        }
    }
    state.trigger_was_held = input.trigger_held;
    return out;
}

bool weapon_fire_now(WeaponState& state, const WeaponProfile& profile, WeaponStep& out) {
    if (!weapon_valid(profile) || state.reloading) {
        return false;
    }
    // The cadence still applies: a script that calls this every frame fires at
    // the weapon's rate, not the frame rate. That is what keeps a Lua-driven
    // turret honest next to a player's trigger.
    if (state.cooldown > 0.0f || state.burst_timer > 0.0f) {
        return false;
    }
    if (state.in_magazine == 0u) {
        if (!state.empty_clicked) {
            out.events.push_back(WeaponEvent{WeaponEventKind::Empty, 0u});
            state.empty_clicked = true;
        }
        if (profile.auto_reload) {
            weapon_begin_reload(state, profile, out);
        }
        return false;
    }
    fire_one_volley(state, profile, out);
    return true;
}

} // namespace nf::runtime
