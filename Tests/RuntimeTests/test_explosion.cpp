// Tests/RuntimeTests/test_explosion.cpp — Phase 51: a blast is a volume.
//
// The direct-hit path was complete after Phase 50: a round lands on one entity,
// reads its zone, spends its armor. A rocket is not a better bullet; it is a
// different shape. These cases pin the shape:
//
//   * damage falls from the centre to the edge and is zero outside;
//   * distance is measured to the nearest point of a box, not to its origin;
//   * the runtime applies the blast at the impact point, skips the direct
//     victim (which already took the direct hit), respects walls and factions,
//     and emits one impact event per victim.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Explosion.hpp>
#include <NF/Runtime/Impact.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Runtime/SceneValidator.hpp>
#include <NF/Runtime/Weapon.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>

#ifndef NF_COMBAT_DAMAGE_SCENE_PATH
    #define NF_COMBAT_DAMAGE_SCENE_PATH ""
#endif

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr f32 kEps = 1e-4f;

WeaponProfile rocket() {
    WeaponProfile p;
    p.mode = FireMode::Semi;
    p.magazine = 4u;
    p.reserve = 4u;
    p.damage = 10.0f;
    p.range = 100.0f;
    p.spread_deg = 0.0f;
    p.spread_max_deg = 1.0f;
    p.spread_bloom_deg = 0.0f;
    p.recoil_pitch = 0.0f;
    p.recoil_yaw = 0.0f;
    p.shake = 0.0f;
    p.blast_radius = 4.0f;
    p.blast_damage = 20.0f;
    p.blast_min_scale = 0.0f;
    return p;
}

} // namespace

// --- the falloff -------------------------------------------------------------

NF_TEST(a_blast_falls_from_the_centre_to_the_edge) {
    ExplosionFalloff blast{10.0f, 100.0f, 0.2f};
    NF_CHECK(explosion_valid(blast));
    NF_CHECK_NEAR(explosion_damage_at(blast, 0.0f), 100.0f, kEps);
    NF_CHECK_NEAR(explosion_damage_at(blast, 5.0f), 60.0f, kEps);
    NF_CHECK_NEAR(explosion_damage_at(blast, 10.0f), 0.0f, kEps);
    NF_CHECK_NEAR(explosion_damage_at(blast, 11.0f), 0.0f, kEps);

    // A negative or non-finite distance is the centre, not a NaN.
    NF_CHECK_NEAR(explosion_damage_at(blast, -3.0f), 100.0f, kEps);
    NF_CHECK_NEAR(explosion_damage_at(blast, std::nanf("")), 100.0f, kEps);

    // The edge scale is clamped: a scene typo cannot make a blast grow.
    ExplosionFalloff over{10.0f, 100.0f, 2.0f};
    NF_CHECK_NEAR(explosion_damage_at(over, 10.0f), 0.0f, kEps);
    ExplosionFalloff under{10.0f, 100.0f, -1.0f};
    NF_CHECK_NEAR(explosion_damage_at(under, 5.0f), 50.0f, kEps);
}

NF_TEST(an_invalid_blast_is_inert) {
    ExplosionFalloff zero_radius{0.0f, 100.0f, 0.0f};
    NF_CHECK(!explosion_valid(zero_radius));
    NF_CHECK_NEAR(explosion_damage_at(zero_radius, 0.0f), 0.0f, kEps);

    ExplosionFalloff zero_damage{10.0f, 0.0f, 0.0f};
    NF_CHECK(!explosion_valid(zero_damage));

    ExplosionFalloff broken{10.0f, std::numeric_limits<f32>::quiet_NaN(), 0.0f};
    NF_CHECK(!explosion_valid(broken));
}

NF_TEST(distance_to_a_box_is_zero_inside_and_measured_to_the_nearest_face) {
    const Vec3 min{-1.0f, -1.0f, -1.0f};
    const Vec3 max{1.0f, 1.0f, 1.0f};
    NF_CHECK_NEAR(distance_point_to_aabb(Vec3{0.0f, 0.0f, 0.0f}, min, max), 0.0f, kEps);
    NF_CHECK_NEAR(distance_point_to_aabb(Vec3{3.0f, 0.0f, 0.0f}, min, max), 2.0f, kEps);
    NF_CHECK_NEAR(distance_point_to_aabb(Vec3{3.0f, 2.0f, 0.0f}, min, max),
                  std::sqrt(2.0f * 2.0f + 1.0f * 1.0f), kEps);

    // A broken box is not a free hit at the origin.
    NF_CHECK(std::isinf(distance_point_to_aabb(Vec3{}, Vec3{1.0f, 0.0f, 0.0f},
                                               Vec3{-1.0f, 0.0f, 0.0f})));
    NF_CHECK(std::isinf(distance_point_to_aabb(Vec3{std::nanf(""), 0.0f, 0.0f}, min, max)));
}

// --- the profile and the file ------------------------------------------------

NF_TEST(a_blast_profile_validates_its_three_numbers) {
    WeaponProfile p = rocket();
    NF_CHECK(weapon_valid(p));
    p.blast_radius = -1.0f;
    NF_CHECK(!weapon_valid(p));
    p = rocket();
    p.blast_damage = -1.0f;
    NF_CHECK(!weapon_valid(p));
    p = rocket();
    p.blast_min_scale = 1.5f;
    NF_CHECK(!weapon_valid(p));

    // No blast is still a valid rifle: the feature is opt-in.
    p = rocket();
    p.blast_radius = 0.0f;
    p.blast_damage = 0.0f;
    NF_CHECK(weapon_valid(p));
}

NF_TEST(a_blast_round_trips_through_the_scene_format) {
    scene::Scene source("Blast");
    const ecs::Entity entity = source.world().create_entity();
    source.world().add<scene::NameComponent>(entity, scene::NameComponent{"Launcher"});
    source.world().add<scene::Transform>(entity);
    WeaponProfile p = rocket();
    source.world().add<WeaponComponent>(entity, WeaponComponent{p});

    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("blast_radius=") != std::string::npos);
    NF_CHECK(text.find("blast_damage=") != std::string::npos);
    NF_CHECK(text.find("blast_min=") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.success || !loaded.scene) {
        return;
    }
    ecs::Entity found = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        const scene::NameComponent* n = loaded.scene->world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Launcher") {
            found = e;
        }
    }
    const WeaponComponent* w = loaded.scene->world().get<WeaponComponent>(found);
    NF_CHECK(w != nullptr);
    if (w != nullptr) {
        NF_CHECK_NEAR(w->profile.blast_radius, 4.0f, kEps);
        NF_CHECK_NEAR(w->profile.blast_damage, 20.0f, kEps);
        NF_CHECK_NEAR(w->profile.blast_min_scale, 0.0f, kEps);
    }
}

NF_TEST(the_combat_damage_sample_scene_loads_and_validates) {
    const std::string path = NF_COMBAT_DAMAGE_SCENE_PATH;
    if (path.empty()) {
        NF_SKIP("no source path configured for the sample scene");
    }
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error) && !exists_error;
    NF_CHECK(exists); // a missing shipped sample is a real failure
    if (!exists) {
        return;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        // Some locked-down CI sandboxes deny read access to the source tree
        // even though the file is present. That is an environment limit, not a
        // broken scene; the parser/round-trip cases still cover the format.
        NF_SKIP("sample scene exists but is not readable in this environment");
    }
    const std::string text{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.success || !loaded.scene) {
        return;
    }

    const ValidationReport report =
        validate_scene(*loaded.scene, "content://Scenes/CombatDamage.nfscene");
    NF_CHECK_EQ(report.errors, 0u);

    ecs::Entity player = ecs::kInvalidEntity;
    ecs::Entity direct = ecs::kInvalidEntity;
    ecs::Entity splash = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        const scene::NameComponent* n = loaded.scene->world().get<scene::NameComponent>(e);
        if (n == nullptr) {
            continue;
        }
        if (n->name == "Player") {
            player = e;
        } else if (n->name == "DirectTarget") {
            direct = e;
        } else if (n->name == "SplashTarget") {
            splash = e;
        }
    }
    NF_CHECK(player.valid());
    NF_CHECK(direct.valid());
    NF_CHECK(splash.valid());

    const WeaponComponent* w = loaded.scene->world().get<WeaponComponent>(player);
    NF_CHECK(w != nullptr);
    if (w != nullptr) {
        NF_CHECK_NEAR(w->profile.blast_radius, 4.0f, kEps);
        NF_CHECK_NEAR(w->profile.blast_damage, 45.0f, kEps);
        NF_CHECK_NEAR(w->profile.armor_penetration, 0.25f, kEps);
        NF_CHECK(w->profile.damage_type == DamageType::Explosive);
    }
    const HealthComponent* dh = loaded.scene->world().get<HealthComponent>(direct);
    NF_CHECK(dh != nullptr);
    if (dh != nullptr) {
        NF_CHECK_NEAR(dh->max_armor, 20.0f, kEps);
        NF_CHECK(dh->hit_zones);
        NF_CHECK_NEAR(dh->explosive_resistance, 0.2f, kEps);
    }
    const StatusEffectComponent* sample_status =
        loaded.scene->world().get<StatusEffectComponent>(splash);
    NF_CHECK(sample_status != nullptr);
    if (sample_status != nullptr) {
        NF_CHECK(status_find(sample_status->effects, StatusKind::Burning) != nullptr);
    }
}

NF_TEST(an_old_weapon_line_writes_no_blast_keys) {
    scene::Scene source("Plain");
    const ecs::Entity entity = source.world().create_entity();
    source.world().add<scene::NameComponent>(entity, scene::NameComponent{"Rifle"});
    source.world().add<scene::Transform>(entity);
    source.world().add<WeaponComponent>(entity, WeaponComponent{WeaponProfile{}});
    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("blast_") == std::string::npos);
    NF_CHECK(text.find("damage_type=") == std::string::npos);
}

// --- the whole chain ---------------------------------------------------------

NF_TEST(the_runtime_blasts_a_second_target_without_double_hitting_the_first) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_explosion_runtime";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        assets::AssetRegistry registry;
        assets::AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto world_scene = std::make_unique<scene::Scene>("BlastRange");
        const ecs::Entity player = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(player, scene::NameComponent{"Player"});
        world_scene->world().add<scene::Transform>(player);
        WeaponProfile p = rocket();
        world_scene->world().add<WeaponComponent>(player, WeaponComponent{p});
        world_scene->world().add<HealthComponent>(player, HealthComponent{});

        const ecs::Entity direct = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(direct, scene::NameComponent{"Direct"});
        auto& dt = world_scene->world().add<scene::Transform>(direct);
        dt.local_y = 1.5f;
        dt.local_z = -10.0f;
        world_scene->world().add<physics::ColliderComponent>(
            direct, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})});
        HealthComponent direct_health;
        direct_health.max_health = 100.0f;
        direct_health.health = 100.0f;
        world_scene->world().add<HealthComponent>(direct, direct_health);

        const ecs::Entity nearby = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(nearby, scene::NameComponent{"Nearby"});
        auto& nt = world_scene->world().add<scene::Transform>(nearby);
        nt.local_x = 1.5f;
        nt.local_y = 1.5f;
        nt.local_z = -10.0f;
        world_scene->world().add<physics::ColliderComponent>(
            nearby, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})});
        HealthComponent nearby_health;
        nearby_health.max_health = 100.0f;
        nearby_health.health = 100.0f;
        world_scene->world().add<HealthComponent>(nearby, nearby_health);

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

        rt.adopt_scene(std::move(world_scene), "content://Scenes/BlastRange.nfscene");
        NF_CHECK_EQ(rt.fire_weapon(), 1u);

        const Runtime::WeaponHitInfo& hit = rt.last_weapon_hit();
        NF_CHECK(hit.hit);
        NF_CHECK(hit.name == "Direct");
        NF_CHECK_NEAR(hit.damage, 10.0f, 0.01f);

        const HealthComponent* dh = rt.health_of(direct);
        NF_CHECK(dh != nullptr);
        if (dh != nullptr) {
            // The direct victim takes the direct hit only. The blast skips it.
            NF_CHECK_NEAR(dh->health, 90.0f, 0.01f);
        }
        const HealthComponent* nh = rt.health_of(nearby);
        NF_CHECK(nh != nullptr);
        if (nh != nullptr) {
            // The blast centre is 1 m from Nearby's nearest face; with a 4 m
            // radius and linear falloff that is 75% of 20 = 15.
            NF_CHECK_NEAR(nh->health, 85.0f, 0.05f);
        }

        const Runtime::WeaponSnapshot snap = rt.weapon_snapshot();
        NF_CHECK_EQ(snap.explosions, 1u);
        NF_CHECK_EQ(snap.splash_hits, 1u);
        NF_CHECK_NEAR(snap.damage_dealt, 25.0f, 0.05f);

        bool saw_direct = false;
        bool saw_splash = false;
        for (const ImpactEvent& event : rt.impact_events()) {
            if (event.victim == direct) {
                saw_direct = true;
                NF_CHECK(!event.splash);
            }
            if (event.victim == nearby) {
                saw_splash = true;
                NF_CHECK(event.splash);
            }
        }
        NF_CHECK(saw_direct);
        NF_CHECK(saw_splash);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
