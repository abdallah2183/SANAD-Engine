// Tests/RuntimeTests/test_weapon.cpp — the gun, as arithmetic.
//
// Phase 30 taught the camera to aim and Phase 33 taught it to kick. Neither
// could shoot: every game wrote its own fire rate, magazine and reload clock in
// Lua, and every game got a different bug. Weapon.hpp is the part that is the
// same for every gun, and these cases are the contract it keeps:
//
//   1. a press is a press: semi fires once, auto fires at the profile's rate,
//      and a burst is exactly `burst_count` rounds;
//   2. the magazine is real: a reload moves rounds from the reserve, a partial
//      reload loads what is there, and an empty gun clicks once per pull;
//   3. the cone is deterministic: the same seed produces the same pattern, so a
//      recorded firefight reproduces;
//   4. a hit is geometry: the nearest box wins, a tie resolves the same way on
//      every run, and the shooter is never the target.
//
// The runtime integration (trigger -> camera -> hitscan -> hit points) is
// covered by the last case, which goes through a real scene and the real
// loader rather than a hand-built component.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/Health.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Runtime/Hitscan.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/Weapon.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr f32 kEps = 1e-4f;

/// A profile whose cadence is easy to reason about: one shot every 0.1 s.
WeaponProfile rifle() {
    WeaponProfile p;
    p.mode = FireMode::Auto;
    p.rounds_per_minute = 600.0f;
    p.magazine = 30u;
    p.reserve = 90u;
    p.reload_seconds = 1.0f;
    p.damage = 20.0f;
    p.range = 100.0f;
    p.spread_deg = 0.5f;
    p.spread_bloom_deg = 0.2f;
    p.spread_max_deg = 3.0f;
    p.spread_recover_deg_per_s = 5.0f;
    p.recoil_pitch = 1.0f;
    p.recoil_yaw = 0.4f;
    p.recoil_growth = 0.25f;
    p.falloff_start = 10.0f;
    p.falloff_end = 40.0f;
    p.falloff_min_scale = 0.5f;
    p.seed = 12345u;
    return p;
}

physics::Aabb box(f32 min_x, f32 min_y, f32 min_z, f32 max_x, f32 max_y, f32 max_z) {
    return physics::Aabb{Vec3{min_x, min_y, min_z}, Vec3{max_x, max_y, max_z}};
}

} // namespace

// --- the profile -------------------------------------------------------------

NF_TEST(weapon_defaults_describe_a_usable_rifle) {
    const WeaponProfile p;
    NF_CHECK(weapon_valid(p));
    NF_CHECK(p.magazine > 0u);
    NF_CHECK_NEAR(weapon_interval(p), 60.0f / p.rounds_per_minute, kEps);

    const WeaponState s = weapon_state(p);
    NF_CHECK_EQ(s.in_magazine, p.magazine);
    NF_CHECK_EQ(s.reserve, p.reserve);
    NF_CHECK(!s.reloading);
    NF_CHECK_NEAR(s.cooldown, 0.0f, kEps);
    // The generator is seeded from the profile, never left at zero (xorshift32
    // would be stuck there and every shot would land dead centre).
    NF_CHECK(s.rng != 0u);
}

NF_TEST(weapon_refuses_a_profile_that_cannot_fire) {
    WeaponProfile p = rifle();
    p.magazine = 0u;
    NF_CHECK(!weapon_valid(p));

    p = rifle();
    p.rounds_per_minute = 0.0f;
    NF_CHECK(!weapon_valid(p));

    p = rifle();
    p.range = 0.0f;
    NF_CHECK(!weapon_valid(p));

    p = rifle();
    p.pellets = 0u;
    NF_CHECK(!weapon_valid(p));

    p = rifle();
    p.spread_deg = 5.0f;
    p.spread_max_deg = 1.0f; // a cone narrower than its own floor
    NF_CHECK(!weapon_valid(p));

    p = rifle();
    p.enabled = false;
    NF_CHECK(!weapon_valid(p));
}

NF_TEST(the_fire_mode_names_round_trip) {
    FireMode mode = FireMode::Semi;
    NF_CHECK(fire_mode_from_name("auto", mode));
    NF_CHECK(mode == FireMode::Auto);
    NF_CHECK(fire_mode_from_name("burst", mode));
    NF_CHECK(mode == FireMode::Burst);
    NF_CHECK(!fire_mode_from_name("Auto", mode)); // case matters: the file is exact
    NF_CHECK(mode == FireMode::Burst);            // a refusal leaves the value alone
    NF_CHECK(std::string(fire_mode_name(FireMode::Semi)) == "semi");
}

// --- the trigger -------------------------------------------------------------

NF_TEST(semi_auto_fires_once_per_press) {
    WeaponProfile p = rifle();
    p.mode = FireMode::Semi;
    WeaponState s = weapon_state(p);

    WeaponInput in;
    in.trigger_held = true;

    u32 shots = 0;
    // Hold the trigger for two seconds of frames: a semi-automatic weapon must
    // not care how long it is held.
    for (int frame = 0; frame < 120; ++frame) {
        const WeaponStep step = weapon_step(s, p, in, 1.0f / 60.0f);
        shots += static_cast<u32>(step.shots.size());
    }
    NF_CHECK_EQ(shots, 1u);

    // Release and press again: that is the second shot.
    in.trigger_held = false;
    (void)weapon_step(s, p, in, 1.0f / 60.0f);
    in.trigger_held = true;
    const WeaponStep second = weapon_step(s, p, in, 1.0f / 60.0f);
    NF_CHECK_EQ(second.shots.size(), 1u);
    NF_CHECK(second.trigger_edge);
}

NF_TEST(auto_fires_at_the_profile_cadence) {
    WeaponProfile p = rifle(); // 600 rpm = one shot every 100 ms
    WeaponState s = weapon_state(p);
    WeaponInput in;
    in.trigger_held = true;

    u32 shots = 0;
    for (int frame = 0; frame < 60; ++frame) { // exactly one second
        shots += static_cast<u32>(weapon_step(s, p, in, 1.0f / 60.0f).shots.size());
    }
    // Ten shots in a second, +/- the frame the first and last land on.
    NF_CHECK(shots >= 9u);
    NF_CHECK(shots <= 11u);
}

NF_TEST(a_paused_frame_fires_nothing_and_advances_nothing) {
    WeaponProfile p = rifle();
    WeaponState s = weapon_state(p);
    s.cooldown = 0.5f;
    s.bloom_deg = 1.0f;
    WeaponInput in;
    in.trigger_held = true;

    const WeaponStep step = weapon_step(s, p, in, 0.0f);
    NF_CHECK(step.shots.empty());
    NF_CHECK(!step.trigger_edge);
    NF_CHECK_NEAR(s.cooldown, 0.5f, kEps);
    NF_CHECK_NEAR(s.bloom_deg, 1.0f, kEps);
    NF_CHECK_EQ(s.in_magazine, p.magazine);
    NF_CHECK(!s.trigger_was_held); // the press is seen by the first live frame
}

NF_TEST(a_burst_is_exactly_burst_count_rounds) {
    WeaponProfile p = rifle();
    p.mode = FireMode::Burst;
    p.burst_count = 3u;
    p.burst_gap_seconds = 0.05f;
    WeaponState s = weapon_state(p);

    WeaponInput in;
    in.trigger_held = true;

    u32 shots = 0;
    for (int frame = 0; frame < 90; ++frame) { // 1.5 s, trigger held throughout
        shots += static_cast<u32>(weapon_step(s, p, in, 1.0f / 60.0f).shots.size());
    }
    NF_CHECK_EQ(shots, 3u);

    // A new press after a release is a new burst.
    in.trigger_held = false;
    (void)weapon_step(s, p, in, 1.0f / 60.0f);
    in.trigger_held = true;
    u32 second = 0;
    for (int frame = 0; frame < 30; ++frame) {
        second += static_cast<u32>(weapon_step(s, p, in, 1.0f / 60.0f).shots.size());
    }
    NF_CHECK_EQ(second, 3u);
}

NF_TEST(an_empty_magazine_clicks_once_per_pull) {
    WeaponProfile p = rifle();
    p.magazine = 1u;
    p.reserve = 3u;
    p.auto_reload = false;
    WeaponState s = weapon_state(p);

    WeaponStep fired;
    NF_CHECK(weapon_fire_now(s, p, fired));
    NF_CHECK_EQ(s.in_magazine, 0u);

    WeaponInput in;
    in.trigger_held = true;
    u32 clicks = 0;
    for (int frame = 0; frame < 30; ++frame) {
        const WeaponStep step = weapon_step(s, p, in, 1.0f / 60.0f);
        NF_CHECK(step.shots.empty());
        for (const WeaponEvent& ev : step.events) {
            if (ev.kind == WeaponEventKind::Empty) {
                ++clicks;
            }
        }
    }
    // One click for the whole pull. A held trigger on an empty gun is not a
    // machine gun of click sounds.
    NF_CHECK_EQ(clicks, 1u);

    // Release and press: a second click, because the player asked again.
    in.trigger_held = false;
    (void)weapon_step(s, p, in, 1.0f / 60.0f);
    in.trigger_held = true;
    u32 second = 0;
    for (int frame = 0; frame < 5; ++frame) {
        for (const WeaponEvent& ev : weapon_step(s, p, in, 1.0f / 60.0f).events) {
            if (ev.kind == WeaponEventKind::Empty) {
                ++second;
            }
        }
    }
    NF_CHECK_EQ(second, 1u);
}

NF_TEST(an_empty_gun_reloads_itself_when_the_profile_says_so) {
    WeaponProfile p = rifle();
    p.magazine = 1u;
    p.reserve = 5u;
    p.auto_reload = true;
    WeaponState s = weapon_state(p);

    WeaponStep fired;
    NF_CHECK(weapon_fire_now(s, p, fired));

    WeaponInput in;
    in.trigger_held = true;
    bool reload_started = false;
    // The trigger is pulled while the weapon is still cooling down from the
    // shot above, so the click (and the reload it starts) lands a few frames
    // later — which is exactly the behaviour a player sees.
    for (int frame = 0; frame < 30; ++frame) {
        for (const WeaponEvent& ev : weapon_step(s, p, in, 1.0f / 60.0f).events) {
            reload_started = reload_started || ev.kind == WeaponEventKind::ReloadStarted;
        }
    }
    NF_CHECK(reload_started);
    NF_CHECK(s.reloading);
}

// --- the magazine ------------------------------------------------------------

NF_TEST(a_reload_moves_rounds_from_the_reserve) {
    WeaponProfile p = rifle();
    p.magazine = 5u;
    p.reserve = 12u;
    p.reload_seconds = 1.0f;
    WeaponState s = weapon_state(p);
    s.in_magazine = 2u; // three fired

    WeaponStep step;
    NF_CHECK(weapon_begin_reload(s, p, step));
    NF_CHECK(s.reloading);
    NF_CHECK_EQ(step.events.size(), 1u);
    NF_CHECK(step.events[0].kind == WeaponEventKind::ReloadStarted);

    // Half a second is not enough.
    (void)weapon_step(s, p, WeaponInput{}, 0.5f);
    NF_CHECK(s.reloading);
    NF_CHECK_EQ(s.in_magazine, 2u);

    // The second half finishes it: three rounds move, nine stay in reserve.
    const WeaponStep done = weapon_step(s, p, WeaponInput{}, 0.5f);
    NF_CHECK(!s.reloading);
    NF_CHECK_EQ(s.in_magazine, 5u);
    NF_CHECK_EQ(s.reserve, 9u);
    NF_CHECK(!done.events.empty());
    NF_CHECK(done.events.back().kind == WeaponEventKind::ReloadFinished);
    NF_CHECK_EQ(done.events.back().value, 3u);
}

NF_TEST(a_partial_reload_loads_what_is_left) {
    WeaponProfile p = rifle();
    p.magazine = 10u;
    p.reserve = 2u;
    p.reload_seconds = 0.1f;
    WeaponState s = weapon_state(p);
    s.in_magazine = 0u;

    WeaponStep step;
    NF_CHECK(weapon_begin_reload(s, p, step));
    (void)weapon_step(s, p, WeaponInput{}, 0.2f);
    NF_CHECK_EQ(s.in_magazine, 2u);
    NF_CHECK_EQ(s.reserve, 0u);
}

NF_TEST(a_full_magazine_refuses_a_reload) {
    WeaponProfile p = rifle();
    WeaponState s = weapon_state(p);
    WeaponStep step;
    NF_CHECK(!weapon_begin_reload(s, p, step));
    NF_CHECK(!s.reloading);
    NF_CHECK(step.events.empty());
}

NF_TEST(a_reload_blocks_firing_until_it_finishes) {
    WeaponProfile p = rifle();
    p.reload_seconds = 0.5f;
    WeaponState s = weapon_state(p);
    s.in_magazine = 1u;

    WeaponStep step;
    NF_CHECK(weapon_begin_reload(s, p, step));

    WeaponInput in;
    in.trigger_held = true;
    u32 shots = 0;
    for (int frame = 0; frame < 20; ++frame) { // 0.33 s: still reloading
        shots += static_cast<u32>(weapon_step(s, p, in, 1.0f / 60.0f).shots.size());
    }
    NF_CHECK_EQ(shots, 0u);

    for (int frame = 0; frame < 20; ++frame) { // past the reload
        shots += static_cast<u32>(weapon_step(s, p, in, 1.0f / 60.0f).shots.size());
    }
    NF_CHECK(shots > 0u);
}

NF_TEST(an_ammo_pickup_tops_the_reserve_up) {
    WeaponProfile p = rifle();
    WeaponState s = weapon_state(p);
    s.reserve = 10u;
    const u32 added = weapon_refill(s, p);
    NF_CHECK_EQ(added, p.reserve - 10u);
    NF_CHECK_EQ(s.reserve, p.reserve);
    NF_CHECK_EQ(weapon_refill(s, p), 0u); // already full
}

// --- the cone ----------------------------------------------------------------

NF_TEST(spread_blooms_per_shot_and_recovers) {
    WeaponProfile p = rifle();
    WeaponState s = weapon_state(p);
    NF_CHECK_NEAR(weapon_spread_deg(s, p), p.spread_deg, kEps);

    WeaponStep step;
    weapon_fire_now(s, p, step);
    NF_CHECK_NEAR(s.bloom_deg, p.spread_bloom_deg, kEps);
    NF_CHECK_NEAR(weapon_spread_deg(s, p), p.spread_deg + p.spread_bloom_deg, kEps);

    // Bloom is capped, so a long burst cannot invent an infinite cone.
    for (int i = 0; i < 100; ++i) {
        s.cooldown = 0.0f;
        weapon_fire_now(s, p, step);
    }
    NF_CHECK_NEAR(weapon_spread_deg(s, p), p.spread_max_deg, 1e-3f);

    // A second of quiet closes the cone back to the profile's base.
    (void)weapon_step(s, p, WeaponInput{}, 1.0f);
    NF_CHECK_NEAR(s.bloom_deg, 0.0f, 1e-3f);
}

NF_TEST(the_spread_pattern_is_deterministic_for_a_seed) {
    WeaponProfile p = rifle();
    p.spread_deg = 2.0f;
    p.spread_bloom_deg = 0.0f; // keep the cone constant so only the RNG moves

    auto fire_five = [&p](u32 seed, std::vector<WeaponShot>& out) {
        WeaponProfile local = p;
        local.seed = seed;
        WeaponState s = weapon_state(local);
        for (int i = 0; i < 5; ++i) {
            s.cooldown = 0.0f;
            WeaponStep step;
            weapon_fire_now(s, local, step);
            for (const WeaponShot& shot : step.shots) {
                out.push_back(shot);
            }
        }
    };

    std::vector<WeaponShot> a;
    std::vector<WeaponShot> b;
    std::vector<WeaponShot> c;
    fire_five(777u, a);
    fire_five(777u, b);
    fire_five(778u, c);

    NF_CHECK_EQ(a.size(), 5u);
    NF_CHECK_EQ(b.size(), 5u);
    for (size_t i = 0; i < a.size(); ++i) {
        NF_CHECK_NEAR(a[i].yaw_deg, b[i].yaw_deg, 0.0f);
        NF_CHECK_NEAR(a[i].pitch_deg, b[i].pitch_deg, 0.0f);
    }
    // A different seed is a different pattern. (The odds of five identical
    // floats from two seeds are the odds of the generator being broken.)
    bool differs = false;
    for (size_t i = 0; i < a.size(); ++i) {
        differs = differs || a[i].yaw_deg != c[i].yaw_deg || a[i].pitch_deg != c[i].pitch_deg;
    }
    NF_CHECK(differs);

    // Every offset is inside the cone.
    for (const WeaponShot& shot : a) {
        const f32 radius = std::sqrt(shot.yaw_deg * shot.yaw_deg + shot.pitch_deg * shot.pitch_deg);
        NF_CHECK(radius <= p.spread_deg + 1e-3f);
    }
}

NF_TEST(a_shotgun_places_every_pellet_separately) {
    WeaponProfile p = rifle();
    p.pellets = 8u;
    p.spread_deg = 3.0f;
    p.spread_bloom_deg = 0.0f;
    WeaponState s = weapon_state(p);

    WeaponStep step;
    NF_CHECK(weapon_fire_now(s, p, step));
    NF_CHECK_EQ(step.shots.size(), 8u);

    // Eight pellets are eight offsets, not one offset repeated.
    bool any_different = false;
    for (size_t i = 1; i < step.shots.size(); ++i) {
        any_different = any_different || step.shots[i].yaw_deg != step.shots[0].yaw_deg;
    }
    NF_CHECK(any_different);
    NF_CHECK_EQ(s.in_magazine, p.magazine - 1u); // one shell, eight pellets
}

// --- damage and recoil -------------------------------------------------------

NF_TEST(damage_falls_off_with_distance) {
    WeaponProfile p = rifle();
    p.damage = 100.0f;
    p.falloff_start = 10.0f;
    p.falloff_end = 20.0f;
    p.falloff_min_scale = 0.5f;

    NF_CHECK_NEAR(weapon_damage_at(p, 5.0f), 100.0f, kEps);
    NF_CHECK_NEAR(weapon_damage_at(p, 10.0f), 100.0f, kEps);
    NF_CHECK_NEAR(weapon_damage_at(p, 15.0f), 75.0f, 1e-3f);
    NF_CHECK_NEAR(weapon_damage_at(p, 20.0f), 50.0f, kEps);
    NF_CHECK_NEAR(weapon_damage_at(p, 500.0f), 50.0f, kEps);

    // No authored curve: full damage at any range rather than a division by
    // zero dressed as a falloff.
    WeaponProfile flat = p;
    flat.falloff_start = 0.0f;
    flat.falloff_end = 0.0f;
    NF_CHECK_NEAR(weapon_damage_at(flat, 1000.0f), 100.0f, kEps);
}

NF_TEST(recoil_climbs_and_alternates) {
    WeaponProfile p = rifle();
    p.recoil_pitch = 1.0f;
    p.recoil_yaw = 0.5f;
    p.recoil_growth = 0.5f;

    const RecoilKick first = weapon_recoil_kick(p, 0u);
    NF_CHECK_NEAR(first.pitch, 1.0f, kEps);
    NF_CHECK_NEAR(first.yaw, 0.5f, kEps);

    const RecoilKick second = weapon_recoil_kick(p, 1u);
    NF_CHECK_NEAR(second.pitch, 1.5f, kEps);
    NF_CHECK_NEAR(second.yaw, -0.75f, kEps); // the other way

    // The ramp stops at the cap: a long burst does not end pointing at the sky.
    const RecoilKick capped = weapon_recoil_kick(p, 100u);
    const RecoilKick at_cap = weapon_recoil_kick(p, kMaxRecoilGrowthShots);
    NF_CHECK_NEAR(capped.pitch, at_cap.pitch, kEps);
}

NF_TEST(fire_now_respects_the_cadence) {
    WeaponProfile p = rifle(); // 100 ms between shots
    WeaponState s = weapon_state(p);

    WeaponStep step;
    NF_CHECK(weapon_fire_now(s, p, step));   // the first is free
    NF_CHECK(!weapon_fire_now(s, p, step));  // the second is not
    NF_CHECK_EQ(s.in_magazine, p.magazine - 1u);

    // 100 ms later it is ready again.
    (void)weapon_step(s, p, WeaponInput{}, 0.1f);
    NF_CHECK(weapon_fire_now(s, p, step));
    NF_CHECK_EQ(s.in_magazine, p.magazine - 2u);
}

// --- hit points --------------------------------------------------------------

NF_TEST(health_damage_clamps_at_zero) {
    HealthComponent h;
    h.max_health = 100.0f;
    h.health = 100.0f;

    NF_CHECK_NEAR(health_apply_damage(h, 30.0f), 30.0f, kEps);
    NF_CHECK_NEAR(h.health, 70.0f, kEps);
    NF_CHECK_NEAR(health_apply_damage(h, 500.0f), 70.0f, kEps);
    NF_CHECK_NEAR(h.health, 0.0f, kEps);
    NF_CHECK(!health_alive(h));
    NF_CHECK_NEAR(health_fraction(h), 0.0f, kEps);

    // A negative or non-finite amount is not healing and not damage.
    NF_CHECK_NEAR(health_apply_damage(h, -5.0f), 0.0f, kEps);
    NF_CHECK_NEAR(health_apply_damage(h, std::nanf("")), 0.0f, kEps);
}

NF_TEST(health_regenerates_only_after_the_delay) {
    HealthComponent h;
    h.max_health = 100.0f;
    h.health = 50.0f;
    h.regen_per_second = 10.0f;
    h.regen_delay = 2.0f;

    NF_CHECK_NEAR(health_step(h, 1.0f), 0.0f, kEps); // 1 s: still waiting
    NF_CHECK_NEAR(h.health, 50.0f, kEps);
    // 2 s: the clock reaches the delay on THIS step, so this is the first one
    // that heals — one second's worth, not two.
    NF_CHECK_NEAR(health_step(h, 1.0f), 10.0f, 1e-3f);
    NF_CHECK_NEAR(h.health, 60.0f, 1e-3f);

    // A hit resets the clock.
    health_apply_damage(h, 5.0f);
    NF_CHECK_NEAR(h.since_damage, 0.0f, kEps);
    NF_CHECK_NEAR(health_step(h, 1.0f), 0.0f, kEps);

    // Regeneration never passes the maximum.
    h.health = 95.0f;
    h.since_damage = 10.0f;
    NF_CHECK_NEAR(health_step(h, 5.0f), 5.0f, 1e-3f);
    NF_CHECK_NEAR(h.health, 100.0f, kEps);
}

NF_TEST(invulnerable_and_disabled_entities_take_nothing) {
    HealthComponent h;
    h.health = 50.0f;
    h.invulnerable = true;
    NF_CHECK_NEAR(health_apply_damage(h, 10.0f), 0.0f, kEps);
    NF_CHECK_NEAR(h.health, 50.0f, kEps);

    h.invulnerable = false;
    h.enabled = false;
    NF_CHECK_NEAR(health_apply_damage(h, 10.0f), 0.0f, kEps);
    NF_CHECK_NEAR(h.health, 50.0f, kEps);
}

NF_TEST(a_broken_health_block_is_neither_alive_nor_usable) {
    HealthComponent h;
    h.max_health = 0.0f;
    NF_CHECK(!health_valid(h));
    NF_CHECK(!health_alive(h));
    NF_CHECK_NEAR(health_fraction(h), 0.0f, kEps);
    NF_CHECK_NEAR(health_apply_damage(h, 10.0f), 0.0f, kEps);
}

// --- the ray -----------------------------------------------------------------

NF_TEST(the_nearest_box_wins) {
    std::vector<HitscanTarget> targets;
    targets.push_back(HitscanTarget{ecs::Entity{1u, 0u}, box(2.0f, -1.0f, -1.0f, 3.0f, 1.0f, 1.0f)});
    targets.push_back(HitscanTarget{ecs::Entity{2u, 0u}, box(-1.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f)});

    const HitscanHit hit = hitscan_nearest(targets, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
                                           100.0f);
    NF_CHECK(hit.hit);
    NF_CHECK_EQ(hit.entity.id, 2u);
    NF_CHECK_NEAR(hit.distance, 4.0f, 1e-4f);
    NF_CHECK_NEAR(hit.position.x, -1.0f, 1e-4f);
    NF_CHECK_NEAR(hit.normal.x, -1.0f, kEps); // the face the ray entered
}

NF_TEST(a_ray_that_misses_reports_a_miss) {
    std::vector<HitscanTarget> targets;
    targets.push_back(HitscanTarget{ecs::Entity{1u, 0u}, box(-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f)});

    NF_CHECK(!hitscan_nearest(targets, Vec3{-5.0f, 5.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 100.0f).hit);
    NF_CHECK(!hitscan_nearest(targets, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 100.0f).hit);
    NF_CHECK(!hitscan_nearest(targets, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.0f).hit);
    // Behind the origin is a miss too.
    NF_CHECK(!hitscan_nearest(targets, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}, 100.0f).hit);
    // Out of range.
    NF_CHECK(!hitscan_nearest(targets, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 2.0f).hit);
}

NF_TEST(a_ray_from_inside_a_box_hits_at_zero) {
    std::vector<HitscanTarget> targets;
    targets.push_back(HitscanTarget{ecs::Entity{1u, 0u}, box(-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f)});
    const HitscanHit hit = hitscan_nearest(targets, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
                                           100.0f);
    NF_CHECK(hit.hit);
    NF_CHECK_NEAR(hit.distance, 0.0f, kEps);
}

NF_TEST(a_tie_goes_to_the_lower_entity_id) {
    // Two boxes that share a face: the same distance, twice. The answer must
    // not depend on which one the vector happened to hold first.
    std::vector<HitscanTarget> targets;
    targets.push_back(HitscanTarget{ecs::Entity{7u, 0u}, box(0.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f)});
    targets.push_back(HitscanTarget{ecs::Entity{3u, 0u}, box(0.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f)});

    const HitscanHit hit = hitscan_nearest(targets, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
                                           100.0f);
    NF_CHECK(hit.hit);
    NF_CHECK_EQ(hit.entity.id, 3u);
}

NF_TEST(the_shooter_is_never_the_target) {
    std::vector<HitscanTarget> targets;
    targets.push_back(HitscanTarget{ecs::Entity{1u, 0u}, box(-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f)});
    targets.push_back(HitscanTarget{ecs::Entity{2u, 0u}, box(5.0f, -1.0f, -1.0f, 6.0f, 1.0f, 1.0f)});

    const HitscanHit hit = hitscan_nearest(targets, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
                                           100.0f, ecs::Entity{1u, 0u});
    NF_CHECK(hit.hit);
    NF_CHECK_EQ(hit.entity.id, 2u);
}

// --- the file ----------------------------------------------------------------

NF_TEST(weapon_and_health_lines_round_trip_through_the_scene_format) {
    scene::Scene source("Combat");
    const ecs::Entity shooter = source.world().create_entity();
    source.world().add<scene::NameComponent>(shooter, scene::NameComponent{"Shooter"});
    source.world().add<scene::Transform>(shooter);

    WeaponProfile p;
    p.mode = FireMode::Burst;
    p.rounds_per_minute = 900.0f;
    p.magazine = 30u;
    p.reserve = 120u;
    p.reload_seconds = 2.25f;
    p.burst_count = 4u;
    p.burst_gap_seconds = 0.09f;
    p.damage = 17.5f;
    p.range = 220.0f;
    p.pellets = 2u;
    p.spread_deg = 1.25f;
    p.spread_max_deg = 6.0f;
    p.falloff_start = 12.0f;
    p.falloff_end = 60.0f;
    p.falloff_min_scale = 0.35f;
    p.shake = 0.2f;
    p.seed = 4242u;
    p.auto_reload = false;
    source.world().add<WeaponComponent>(shooter, WeaponComponent{p});

    HealthComponent h;
    h.max_health = 150.0f;
    h.health = 90.0f;
    h.regen_per_second = 3.0f;
    h.regen_delay = 4.5f;
    h.invulnerable = true;
    source.world().add<HealthComponent>(shooter, h);

    const std::string text = serialize_scene_to_text(source);
    // The lines are in the file, and they say what the components said.
    NF_CHECK(text.find("  Weapon: mode=burst") != std::string::npos);
    NF_CHECK(text.find("  Health: max=150") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.success || !loaded.scene) {
        return;
    }

    ecs::Entity found = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        const scene::NameComponent* n = loaded.scene->world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Shooter") {
            found = e;
        }
    }
    NF_CHECK(found.valid());

    const WeaponComponent* w = loaded.scene->world().get<WeaponComponent>(found);
    NF_CHECK(w != nullptr);
    if (w != nullptr) {
        NF_CHECK(w->profile.mode == FireMode::Burst);
        NF_CHECK_NEAR(w->profile.rounds_per_minute, 900.0f, 1e-4f);
        NF_CHECK_EQ(w->profile.magazine, 30u);
        NF_CHECK_EQ(w->profile.burst_count, 4u);
        NF_CHECK_NEAR(w->profile.burst_gap_seconds, 0.09f, 1e-5f);
        NF_CHECK_NEAR(w->profile.damage, 17.5f, 1e-4f);
        NF_CHECK_NEAR(w->profile.falloff_min_scale, 0.35f, 1e-5f);
        NF_CHECK_EQ(w->profile.seed, 4242u);
        NF_CHECK(!w->profile.auto_reload);
        // The live state is rebuilt from the profile, not read from the file:
        // a loaded scene starts with a full magazine.
        NF_CHECK_EQ(w->state.in_magazine, 30u);
        NF_CHECK_EQ(w->state.reserve, 120u);
    }

    const HealthComponent* hc = loaded.scene->world().get<HealthComponent>(found);
    NF_CHECK(hc != nullptr);
    if (hc != nullptr) {
        NF_CHECK_NEAR(hc->max_health, 150.0f, 1e-4f);
        NF_CHECK_NEAR(hc->health, 90.0f, 1e-4f);
        NF_CHECK_NEAR(hc->regen_per_second, 3.0f, 1e-5f);
        NF_CHECK_NEAR(hc->regen_delay, 4.5f, 1e-5f);
        NF_CHECK(hc->invulnerable);
    }
}

NF_TEST(a_weapon_line_with_an_unknown_mode_is_refused_whole) {
    const std::string text =
        "# SANAD Scene v1\n"
        "version: 1\n"
        "name: Bad\n"
        "entity_count: 1\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: Shooter\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(-1:0)\n"
        "  Weapon: mode=automatic rpm=600\n";

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.scene) {
        return;
    }
    // The line was refused and said so; no half-built weapon exists to fire at
    // the engine's default rate.
    NF_CHECK(!loaded.warnings.empty());
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        NF_CHECK(loaded.scene->world().get<WeaponComponent>(e) == nullptr);
    }
}

// --- the whole chain ---------------------------------------------------------

// The one case that proves the wiring: a scene, a camera that follows the
// shooter, a gun, a target with hit points, and a shot. Every piece above is
// unit-tested on its own; this is the only test that says the runtime actually
// calls them — the difference between a working library and a working game.
NF_TEST(the_runtime_traces_a_shot_from_the_camera_into_hit_points) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_weapon_runtime";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        assets::AssetRegistry registry;
        assets::AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto world_scene = std::make_unique<scene::Scene>("Range");

        // The player: where the camera orbits, and what the weapon belongs to.
        const ecs::Entity player = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(player,
                                                      scene::NameComponent{"Player"});
        auto& pt = world_scene->world().add<scene::Transform>(player);
        pt.local_x = 0.0f;
        pt.local_y = 0.0f;
        pt.local_z = 0.0f;

        WeaponProfile p;
        p.mode = FireMode::Semi;
        p.magazine = 3u;
        p.reserve = 3u;
        p.reload_seconds = 0.5f;
        p.damage = 10.0f;
        p.range = 100.0f;
        p.spread_deg = 0.0f;
        p.spread_max_deg = 1.0f;
        p.spread_bloom_deg = 0.0f;
        p.recoil_pitch = 0.0f;
        p.recoil_yaw = 0.0f;
        p.shake = 0.0f;
        world_scene->world().add<WeaponComponent>(player, WeaponComponent{p});

        HealthComponent player_health;
        player_health.max_health = 100.0f;
        player_health.health = 100.0f;
        world_scene->world().add<HealthComponent>(player, player_health);

        // The target: ten metres down -Z, at the camera's eye height, with a
        // box collider and 30 hit points.
        const ecs::Entity target = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(target, scene::NameComponent{"Target"});
        auto& tt = world_scene->world().add<scene::Transform>(target);
        tt.local_x = 0.0f;
        tt.local_y = 1.5f;
        tt.local_z = -10.0f;
        world_scene->world().add<physics::ColliderComponent>(
            target, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})});
        HealthComponent target_health;
        target_health.max_health = 30.0f;
        target_health.health = 30.0f;
        world_scene->world().add<HealthComponent>(target, target_health);

        // The camera: behind the player, looking down -Z at the target.
        const ecs::Entity camera = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
        world_scene->world().add<scene::Transform>(camera);
        CameraComponent cam;
        cam.is_active = true;
        cam.follow_target = "Player";
        cam.pivot_y = 1.5f;
        cam.distance = 4.0f;
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
        cam.mouse_look = false;
        world_scene->world().add<CameraComponent>(camera, cam);

        rt.adopt_scene(std::move(world_scene), "content://Scenes/Range.nfscene");

        // The camera sits at (0, 1.5, 4) and looks down -Z; the target's near
        // face is at z = -9.5, so the shot travels 13.5 m.
        const u32 traced = rt.fire_weapon();
        NF_CHECK_EQ(traced, 1u);

        const Runtime::WeaponHitInfo& hit = rt.last_weapon_hit();
        NF_CHECK(hit.hit);
        NF_CHECK(hit.name == "Target");
        NF_CHECK_NEAR(hit.distance, 13.5f, 0.05f);
        NF_CHECK_NEAR(hit.damage, 10.0f, 0.01f);
        NF_CHECK_NEAR(hit.applied, 10.0f, 0.01f);

        const HealthComponent* th = rt.health_of(target);
        NF_CHECK(th != nullptr);
        if (th != nullptr) {
            NF_CHECK_NEAR(th->health, 20.0f, 0.01f);
        }

        // The counters are the observables that separate "a shot was fired"
        // from "a target lost hit points".
        const Runtime::WeaponSnapshot snap = rt.weapon_snapshot();
        NF_CHECK(snap.has_weapon);
        NF_CHECK_EQ(snap.ammo, 2u);
        NF_CHECK_EQ(snap.shots_fired, 1u);
        NF_CHECK_EQ(snap.hits_landed, 1u);
        NF_CHECK_NEAR(snap.damage_dealt, 10.0f, 0.01f);

        // The cadence holds even for a scripted shot: the same frame cannot
        // fire twice.
        NF_CHECK_EQ(rt.fire_weapon(), 0u);

        // Reload, then let the clock run past it: the one round the magazine
        // was missing comes back from the reserve (3 -> 2), and the magazine
        // is full again.
        NF_CHECK(rt.weapon_reload());
        NF_CHECK(rt.weapon_snapshot().reloading);
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        const Runtime::WeaponSnapshot after = rt.weapon_snapshot();
        NF_CHECK(!after.reloading);
        NF_CHECK_EQ(after.ammo, 3u);
        NF_CHECK_EQ(after.reserve, 2u);

        // A shot at a dead target still traces and still reports a hit; it
        // simply applies nothing. (Damage is clamped, never negative.)
        rt.fire_weapon();
        const HealthComponent* dead = rt.health_of(target);
        NF_CHECK(dead != nullptr);
        if (dead != nullptr) {
            NF_CHECK(dead->health <= 20.0f);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
