// Tests/RuntimeTests/test_damage.cpp — Phase 50: a hit is layered.
//
// A weapon's damage number used to be the whole story. These cases pin the two
// layers added on top of it:
//
//   * hit zones are opt-in and geometric — a head costs more, a limb costs less,
//     and a scene that never asked for zones keeps the old flat hit;
//   * armor is a real pool — it absorbs a configured fraction until it is gone,
//     penetration bypasses part of it, and the raw `health_apply_damage()` path
//     still means "environmental damage", not "a bullet".
//
// The last case is the integration: a real camera, a real collider, a real
// weapon and a head hit that spends armor. It skips when the machine has no
// Vulkan device, like every other runtime integration in this suite.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Damage.hpp>
#include <NF/Runtime/Health.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Runtime/Weapon.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr f32 kEps = 1e-4f;

HealthComponent armoured(f32 armor, f32 absorb = 0.5f) {
    HealthComponent h;
    h.max_health = 100.0f;
    h.health = 100.0f;
    h.max_armor = armor;
    h.armor = armor;
    h.armor_absorb = absorb;
    return h;
}

} // namespace

// --- zones -------------------------------------------------------------------

NF_TEST(hit_zones_are_head_body_and_limbs) {
    // A two-metre collider from y=0 to y=2: the top 18% is the head, the bottom
    // 28% is limbs, and the middle is the body.
    NF_CHECK(hit_zone_from_height(1.95f, 0.0f, 2.0f) == HitZone::Head);
    NF_CHECK(hit_zone_from_height(1.64f, 0.0f, 2.0f) == HitZone::Head);
    NF_CHECK(hit_zone_from_height(1.00f, 0.0f, 2.0f) == HitZone::Body);
    NF_CHECK(hit_zone_from_height(0.56f, 0.0f, 2.0f) == HitZone::Limbs);
    NF_CHECK(hit_zone_from_height(0.0f, 0.0f, 2.0f) == HitZone::Limbs);

    // Degenerate and non-finite extents never become an accidental headshot.
    NF_CHECK(hit_zone_from_height(1.0f, 1.0f, 1.0f) == HitZone::Body);
    NF_CHECK(hit_zone_from_height(std::nanf(""), 0.0f, 2.0f) == HitZone::Body);
    NF_CHECK(hit_zone_from_height(1.0f, 0.0f, std::numeric_limits<f32>::infinity()) ==
             HitZone::Body);
}

NF_TEST(hit_zone_names_round_trip_and_refuse_unknown_values) {
    HitZone zone = HitZone::Body;
    NF_CHECK(hit_zone_from_name("head", zone));
    NF_CHECK(zone == HitZone::Head);
    NF_CHECK(hit_zone_from_name("limbs", zone));
    NF_CHECK(zone == HitZone::Limbs);
    NF_CHECK(hit_zone_from_name("body", zone));
    NF_CHECK(zone == HitZone::Body);

    NF_CHECK(!hit_zone_from_name("Head", zone)); // exact, like every scene key
    NF_CHECK(zone == HitZone::Body);             // refusal leaves it alone
    NF_CHECK(std::string(hit_zone_name(HitZone::Head)) == "head");
}

NF_TEST(damage_type_names_round_trip_and_refuse_unknown_values) {
    DamageType type = DamageType::Ballistic;
    NF_CHECK(damage_type_from_name("explosive", type));
    NF_CHECK(type == DamageType::Explosive);
    NF_CHECK(damage_type_from_name("fire", type));
    NF_CHECK(type == DamageType::Fire);
    NF_CHECK(damage_type_from_name("ballistic", type));
    NF_CHECK(type == DamageType::Ballistic);

    NF_CHECK(!damage_type_from_name("Fire", type));
    NF_CHECK(type == DamageType::Ballistic);
    NF_CHECK(std::string(damage_type_name(DamageType::Explosive)) == "explosive");
}

NF_TEST(resistance_is_per_type_and_applied_before_armor) {
    HealthComponent h = armoured(20.0f, 0.5f);
    h.ballistic_resistance = 0.5f;
    h.explosive_resistance = 0.0f;

    const DamageResult ballistic = health_apply_hit(h, 40.0f, HitZone::Body, 0.0f,
                                                    DamageType::Ballistic);
    NF_CHECK_NEAR(ballistic.zoned, 40.0f, kEps);
    NF_CHECK_NEAR(ballistic.resisted, 20.0f, kEps);
    // 20 effective: armor takes half (10), health takes the other 10.
    NF_CHECK_NEAR(ballistic.armor_absorbed, 10.0f, kEps);
    NF_CHECK_NEAR(ballistic.health_damage, 10.0f, kEps);
    NF_CHECK_NEAR(h.armor, 10.0f, kEps);
    NF_CHECK_NEAR(h.health, 90.0f, kEps);

    // A different type does not read the ballistic resistance. The remaining
    // 10 armor absorbs half of 40 = 10, so health takes 30.
    const DamageResult explosive = health_apply_hit(h, 40.0f, HitZone::Body, 0.0f,
                                                    DamageType::Explosive);
    NF_CHECK_NEAR(explosive.resisted, 0.0f, kEps);
    NF_CHECK_NEAR(explosive.armor_absorbed, 10.0f, kEps);
    NF_CHECK_NEAR(explosive.health_damage, 30.0f, kEps);
    NF_CHECK_NEAR(h.armor, 0.0f, kEps);
    NF_CHECK_NEAR(h.health, 60.0f, kEps);

    NF_CHECK_NEAR(health_resistance(h, DamageType::Ballistic), 0.5f, kEps);
    NF_CHECK_NEAR(health_resistance(h, DamageType::Fire), 0.0f, kEps);
}

NF_TEST(an_out_of_range_resistance_refuses_the_whole_health_block) {
    HealthComponent h = armoured(20.0f);
    h.fire_resistance = 1.5f;
    NF_CHECK(!health_valid(h));
    const DamageResult refused = health_apply_hit(h, 10.0f, HitZone::Body, 0.0f,
                                                   DamageType::Fire);
    NF_CHECK(!refused.accepted);
    NF_CHECK_NEAR(refused.health_damage, 0.0f, kEps);
    NF_CHECK_NEAR(health_resistance(h, DamageType::Fire), 0.0f, kEps);
}

NF_TEST(zones_are_opt_in_and_multiply_the_hit) {
    HealthComponent flat;
    flat.max_health = 100.0f;
    flat.health = 100.0f;
    flat.hit_zones = false;
    const DamageResult body = health_apply_hit(flat, 20.0f, HitZone::Head);
    NF_CHECK(body.accepted);
    NF_CHECK_NEAR(body.health_damage, 20.0f, kEps);
    NF_CHECK_NEAR(flat.health, 80.0f, kEps);

    HealthComponent zoned;
    zoned.max_health = 100.0f;
    zoned.health = 100.0f;
    zoned.hit_zones = true;
    zoned.headshot_multiplier = 2.0f;
    zoned.limb_multiplier = 0.5f;

    const DamageResult head = health_apply_hit(zoned, 20.0f, HitZone::Head);
    NF_CHECK(head.accepted);
    NF_CHECK_NEAR(head.zoned, 40.0f, kEps);
    NF_CHECK_NEAR(head.health_damage, 40.0f, kEps);
    NF_CHECK_NEAR(zoned.health, 60.0f, kEps);

    const DamageResult limb = health_apply_hit(zoned, 20.0f, HitZone::Limbs);
    NF_CHECK_NEAR(limb.health_damage, 10.0f, kEps);
    NF_CHECK_NEAR(zoned.health, 50.0f, kEps);

    const DamageResult torso = health_apply_hit(zoned, 20.0f, HitZone::Body);
    NF_CHECK_NEAR(torso.health_damage, 20.0f, kEps);
    NF_CHECK_NEAR(zoned.health, 30.0f, kEps);
}

// --- armor -------------------------------------------------------------------

NF_TEST(armor_absorbs_until_it_is_gone) {
    HealthComponent h = armoured(20.0f, 0.5f);

    const DamageResult first = health_apply_hit(h, 30.0f, HitZone::Body);
    NF_CHECK(first.accepted);
    NF_CHECK_NEAR(first.zoned, 30.0f, kEps);
    NF_CHECK_NEAR(first.armor_absorbed, 15.0f, kEps);
    NF_CHECK_NEAR(first.health_damage, 15.0f, kEps);
    NF_CHECK_NEAR(h.armor, 5.0f, kEps);
    NF_CHECK_NEAR(h.health, 85.0f, kEps);

    // The armor cannot absorb more than it has: the rest flows through.
    const DamageResult second = health_apply_hit(h, 30.0f, HitZone::Body);
    NF_CHECK_NEAR(second.armor_absorbed, 5.0f, kEps);
    NF_CHECK_NEAR(second.health_damage, 25.0f, kEps);
    NF_CHECK_NEAR(h.armor, 0.0f, kEps);
    NF_CHECK_NEAR(h.health, 60.0f, kEps);

    // With no armor left, the full hit reaches health.
    const DamageResult third = health_apply_hit(h, 10.0f, HitZone::Body);
    NF_CHECK_NEAR(third.armor_absorbed, 0.0f, kEps);
    NF_CHECK_NEAR(third.health_damage, 10.0f, kEps);
    NF_CHECK_NEAR(h.health, 50.0f, kEps);
}

NF_TEST(armor_penetration_bypasses_a_fraction_of_the_protection) {
    HealthComponent h = armoured(50.0f, 0.8f);
    const DamageResult half = health_apply_hit(h, 20.0f, HitZone::Body, 0.5f);
    // Half of 20 is protectable, 80% of that is absorbed = 8.
    NF_CHECK_NEAR(half.armor_absorbed, 8.0f, kEps);
    NF_CHECK_NEAR(half.health_damage, 12.0f, kEps);
    NF_CHECK_NEAR(h.armor, 42.0f, kEps);

    // Out-of-range and non-finite penetration is clamped, not propagated.
    const DamageResult full = health_apply_hit(h, 20.0f, HitZone::Body, 2.0f);
    NF_CHECK_NEAR(full.armor_absorbed, 0.0f, kEps);
    NF_CHECK_NEAR(full.health_damage, 20.0f, kEps);

    // The armor left after the full-penetration hit is still 42, so a NaN
    // penetration (clamped to zero) takes the ordinary 80% share of 20 = 16.
    const DamageResult nan = health_apply_hit(h, 20.0f, HitZone::Body, std::nanf(""));
    NF_CHECK_NEAR(nan.armor_absorbed, 16.0f, kEps);
    NF_CHECK_NEAR(nan.health_damage, 4.0f, kEps);
    NF_CHECK_NEAR(h.armor, 26.0f, kEps);
}

NF_TEST(a_headshot_and_armor_compose_in_the_right_order) {
    HealthComponent h = armoured(20.0f, 0.5f);
    h.hit_zones = true;
    h.headshot_multiplier = 2.0f;

    // 20 damage becomes 40 at the head; armor takes half of that (20), and the
    // other 20 reaches health. The armor is exactly empty, not negative.
    const DamageResult hit = health_apply_hit(h, 20.0f, HitZone::Head);
    NF_CHECK_NEAR(hit.zoned, 40.0f, kEps);
    NF_CHECK_NEAR(hit.armor_absorbed, 20.0f, kEps);
    NF_CHECK_NEAR(hit.health_damage, 20.0f, kEps);
    NF_CHECK_NEAR(h.armor, 0.0f, kEps);
    NF_CHECK_NEAR(h.health, 80.0f, kEps);
}

NF_TEST(raw_damage_still_bypasses_armor) {
    HealthComponent h = armoured(50.0f, 1.0f);
    NF_CHECK_NEAR(health_apply_damage(h, 10.0f), 10.0f, kEps);
    NF_CHECK_NEAR(h.health, 90.0f, kEps);
    NF_CHECK_NEAR(h.armor, 50.0f, kEps);
    NF_CHECK_NEAR(h.since_damage, 0.0f, kEps);
}

NF_TEST(an_accepted_hit_resets_the_regen_clock_even_when_armor_takes_it_all) {
    HealthComponent h = armoured(100.0f, 1.0f);
    h.health = 40.0f;
    h.since_damage = 9.0f;
    h.regen_per_second = 5.0f;
    h.regen_delay = 1.0f;

    const DamageResult hit = health_apply_hit(h, 10.0f, HitZone::Body);
    NF_CHECK_NEAR(hit.health_damage, 0.0f, kEps);
    NF_CHECK_NEAR(hit.armor_absorbed, 10.0f, kEps);
    NF_CHECK_NEAR(h.since_damage, 0.0f, kEps);
    // Half a second into a one-second delay: still no regeneration. (At exactly
    // the delay the clock has elapsed, which the existing health tests pin.)
    NF_CHECK_NEAR(health_step(h, 0.5f), 0.0f, kEps);
}

NF_TEST(invulnerable_and_broken_components_refuse_a_layered_hit) {
    HealthComponent invulnerable = armoured(50.0f);
    invulnerable.invulnerable = true;
    const DamageResult refused = health_apply_hit(invulnerable, 20.0f, HitZone::Head);
    NF_CHECK(!refused.accepted);
    NF_CHECK_NEAR(refused.health_damage, 0.0f, kEps);
    NF_CHECK_NEAR(invulnerable.armor, 50.0f, kEps);

    HealthComponent broken = armoured(50.0f);
    broken.armor_absorb = 1.5f;
    NF_CHECK(!health_valid(broken));
    const DamageResult invalid = health_apply_hit(broken, 20.0f, HitZone::Body);
    NF_CHECK(!invalid.accepted);
    NF_CHECK_NEAR(invalid.health_damage, 0.0f, kEps);
}

NF_TEST(armor_fraction_is_a_clamped_hud_value) {
    HealthComponent h = armoured(40.0f);
    h.max_armor = 80.0f;
    NF_CHECK_NEAR(health_armor_fraction(h), 0.5f, kEps);
    h.armor = 100.0f;
    NF_CHECK_NEAR(health_armor_fraction(h), 1.0f, kEps);
    h.max_armor = 0.0f;
    NF_CHECK_NEAR(health_armor_fraction(h), 0.0f, kEps);
}

// --- the scene format --------------------------------------------------------

NF_TEST(armor_zones_and_penetration_round_trip_through_the_scene_format) {
    scene::Scene source("Layered");
    const ecs::Entity entity = source.world().create_entity();
    source.world().add<scene::NameComponent>(entity, scene::NameComponent{"Soldier"});
    source.world().add<scene::Transform>(entity);

    WeaponProfile p;
    p.mode = FireMode::Semi;
    p.magazine = 8u;
    p.damage = 12.0f;
    p.armor_penetration = 0.35f;
    source.world().add<WeaponComponent>(entity, WeaponComponent{p});

    HealthComponent h;
    h.max_health = 120.0f;
    h.health = 80.0f;
    h.max_armor = 60.0f;
    h.armor = 40.0f;
    h.armor_absorb = 0.4f;
    h.hit_zones = true;
    h.headshot_multiplier = 2.5f;
    h.limb_multiplier = 0.5f;
    source.world().add<HealthComponent>(entity, h);

    const std::string text = serialize_scene_to_text(source);
    // The stream uses nine significant digits so every f32 round-trips; 0.35f
    // is therefore written as 0.349999994, not as the decimal the author typed.
    // The key is what this test can honestly require; the value is checked on
    // the loaded component below.
    NF_CHECK(text.find("pierce=") != std::string::npos);
    NF_CHECK(text.find("armor=40") != std::string::npos);
    NF_CHECK(text.find("armor_max=60") != std::string::npos);
    NF_CHECK(text.find("absorb=") != std::string::npos);
    NF_CHECK(text.find("zones=true") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.success || !loaded.scene) {
        return;
    }

    ecs::Entity found = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        const scene::NameComponent* n = loaded.scene->world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Soldier") {
            found = e;
        }
    }
    NF_CHECK(found.valid());
    const WeaponComponent* w = loaded.scene->world().get<WeaponComponent>(found);
    NF_CHECK(w != nullptr);
    if (w != nullptr) {
        NF_CHECK_NEAR(w->profile.armor_penetration, 0.35f, kEps);
    }
    const HealthComponent* hc = loaded.scene->world().get<HealthComponent>(found);
    NF_CHECK(hc != nullptr);
    if (hc != nullptr) {
        NF_CHECK_NEAR(hc->max_armor, 60.0f, kEps);
        NF_CHECK_NEAR(hc->armor, 40.0f, kEps);
        NF_CHECK_NEAR(hc->armor_absorb, 0.4f, kEps);
        NF_CHECK(hc->hit_zones);
        NF_CHECK_NEAR(hc->headshot_multiplier, 2.5f, kEps);
        NF_CHECK_NEAR(hc->limb_multiplier, 0.5f, kEps);
    }
}

NF_TEST(damage_types_and_resistances_round_trip_through_the_scene_format) {
    scene::Scene source("Types");
    const ecs::Entity entity = source.world().create_entity();
    source.world().add<scene::NameComponent>(entity, scene::NameComponent{"Elemental"});
    source.world().add<scene::Transform>(entity);

    WeaponProfile p;
    p.mode = FireMode::Semi;
    p.magazine = 6u;
    p.damage = 15.0f;
    p.damage_type = DamageType::Fire;
    source.world().add<WeaponComponent>(entity, WeaponComponent{p});

    HealthComponent h;
    h.max_health = 100.0f;
    h.health = 100.0f;
    h.ballistic_resistance = 0.2f;
    h.explosive_resistance = 0.4f;
    h.fire_resistance = 0.6f;
    source.world().add<HealthComponent>(entity, h);

    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("damage_type=fire") != std::string::npos);
    NF_CHECK(text.find("resist_ballistic=") != std::string::npos);
    NF_CHECK(text.find("resist_explosive=") != std::string::npos);
    NF_CHECK(text.find("resist_fire=") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.success || !loaded.scene) {
        return;
    }
    ecs::Entity found = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        const scene::NameComponent* n = loaded.scene->world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Elemental") {
            found = e;
        }
    }
    const WeaponComponent* w = loaded.scene->world().get<WeaponComponent>(found);
    NF_CHECK(w != nullptr);
    if (w != nullptr) {
        NF_CHECK(w->profile.damage_type == DamageType::Fire);
    }
    const HealthComponent* hc = loaded.scene->world().get<HealthComponent>(found);
    NF_CHECK(hc != nullptr);
    if (hc != nullptr) {
        NF_CHECK_NEAR(hc->ballistic_resistance, 0.2f, kEps);
        NF_CHECK_NEAR(hc->explosive_resistance, 0.4f, kEps);
        NF_CHECK_NEAR(hc->fire_resistance, 0.6f, kEps);
    }
}

NF_TEST(an_old_health_line_writes_no_armor_or_zone_keys) {
    scene::Scene source("Old");
    const ecs::Entity entity = source.world().create_entity();
    source.world().add<scene::NameComponent>(entity, scene::NameComponent{"Old"});
    source.world().add<scene::Transform>(entity);
    source.world().add<HealthComponent>(entity, HealthComponent{});
    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("armor=") == std::string::npos);
    NF_CHECK(text.find("zones=") == std::string::npos);
    NF_CHECK(text.find("resist_") == std::string::npos);
}

// --- the whole chain ---------------------------------------------------------

NF_TEST(the_runtime_turns_a_head_hit_into_armor_and_health_damage) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_damage_runtime";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        assets::AssetRegistry registry;
        assets::AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto world_scene = std::make_unique<scene::Scene>("LayeredRange");
        const ecs::Entity player = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(player, scene::NameComponent{"Player"});
        world_scene->world().add<scene::Transform>(player);

        WeaponProfile p;
        p.mode = FireMode::Semi;
        p.magazine = 3u;
        p.damage = 10.0f;
        p.range = 100.0f;
        p.spread_deg = 0.0f;
        p.spread_max_deg = 1.0f;
        p.spread_bloom_deg = 0.0f;
        p.recoil_pitch = 0.0f;
        p.recoil_yaw = 0.0f;
        p.shake = 0.0f;
        p.armor_penetration = 0.0f;
        world_scene->world().add<WeaponComponent>(player, WeaponComponent{p});
        world_scene->world().add<HealthComponent>(player, HealthComponent{});

        const ecs::Entity target = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(target, scene::NameComponent{"Target"});
        auto& tt = world_scene->world().add<scene::Transform>(target);
        tt.local_y = 1.5f;
        tt.local_z = -10.0f;
        world_scene->world().add<physics::ColliderComponent>(
            target, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})});
        HealthComponent target_health;
        target_health.max_health = 100.0f;
        target_health.health = 100.0f;
        target_health.max_armor = 50.0f;
        target_health.armor = 50.0f;
        target_health.armor_absorb = 0.5f;
        target_health.hit_zones = true;
        target_health.headshot_multiplier = 2.0f;
        world_scene->world().add<HealthComponent>(target, target_health);

        const ecs::Entity camera = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
        world_scene->world().add<scene::Transform>(camera);
        CameraComponent cam;
        cam.is_active = true;
        cam.follow_target = "Player";
        // The camera and pivot sit at y=1.9, inside the target's head band
        // (the box spans y=1.0..2.0 and the top 18% starts at y=1.82).
        cam.pivot_y = 1.9f;
        cam.distance = 4.0f;
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
        cam.mouse_look = false;
        world_scene->world().add<CameraComponent>(camera, cam);

        rt.adopt_scene(std::move(world_scene), "content://Scenes/LayeredRange.nfscene");
        NF_CHECK_EQ(rt.fire_weapon(), 1u);

        const Runtime::WeaponHitInfo& hit = rt.last_weapon_hit();
        NF_CHECK(hit.hit);
        NF_CHECK(hit.name == "Target");
        NF_CHECK(hit.hit_zone == HitZone::Head);
        NF_CHECK_NEAR(hit.damage, 10.0f, 0.01f);
        NF_CHECK_NEAR(hit.armor_absorbed, 10.0f, 0.01f);
        NF_CHECK_NEAR(hit.applied, 10.0f, 0.01f);

        const HealthComponent* th = rt.health_of(target);
        NF_CHECK(th != nullptr);
        if (th != nullptr) {
            NF_CHECK_NEAR(th->armor, 40.0f, 0.01f);
            NF_CHECK_NEAR(th->health, 90.0f, 0.01f);
        }
        NF_CHECK_NEAR(rt.weapon_snapshot().armor_damage_dealt, 10.0f, 0.01f);
        NF_CHECK_NEAR(rt.weapon_snapshot().damage_dealt, 10.0f, 0.01f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
