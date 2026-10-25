// Tests/RuntimeTests/test_ballistics.cpp — Phase 46: time of flight and sides.
//
// Two gaps were left open by Phase 45. A shot always arrived in the frame it
// was fired, and every box was a valid target regardless of who owned it.
// These cases pin the two replacements:
//
//   * a projectile integrates a real parabola, expires at the weapon's range,
//     and is deterministic across frame-rate splits;
//   * a faction is neutral when absent, blocks same-team damage by default,
//     and allows it only when the ATTACKER opts in.
//
// The last case goes through the runtime: a projectile is fired, does no
// damage while in flight, and lands on the target only after enough frames.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/Factions.hpp>
#include <NF/Runtime/Projectile.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Runtime/SceneValidator.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#ifndef NF_BALLISTICS_SCENE_PATH
    #define NF_BALLISTICS_SCENE_PATH ""
#endif

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr f32 kEps = 1e-4f;

WeaponProfile projectile_rifle() {
    WeaponProfile p;
    p.mode = FireMode::Semi;
    p.magazine = 4u;
    p.reserve = 8u;
    p.damage = 10.0f;
    p.range = 100.0f;
    p.spread_deg = 0.0f;
    p.spread_max_deg = 1.0f;
    p.spread_bloom_deg = 0.0f;
    p.recoil_pitch = 0.0f;
    p.recoil_yaw = 0.0f;
    p.shake = 0.0f;
    p.delivery = WeaponDelivery::Projectile;
    p.projectile_speed = 20.0f;
    p.projectile_gravity = 0.0f;
    return p;
}

} // namespace

// --- the projectile ----------------------------------------------------------

NF_TEST(projectile_defaults_are_hitscan_and_named) {
    const WeaponProfile p;
    NF_CHECK(p.delivery == WeaponDelivery::Hitscan);
    NF_CHECK(weapon_valid(p));
    NF_CHECK(std::string(weapon_delivery_name(WeaponDelivery::Hitscan)) == "hitscan");

    WeaponDelivery delivery = WeaponDelivery::Hitscan;
    NF_CHECK(weapon_delivery_from_name("projectile", delivery));
    NF_CHECK(delivery == WeaponDelivery::Projectile);
    NF_CHECK(!weapon_delivery_from_name("Projectile", delivery));
    NF_CHECK(delivery == WeaponDelivery::Projectile); // refusal leaves it alone
}

NF_TEST(a_projectile_profile_needs_a_speed_and_non_negative_gravity) {
    WeaponProfile p = projectile_rifle();
    NF_CHECK(weapon_valid(p));

    p.projectile_speed = 0.0f;
    NF_CHECK(!weapon_valid(p));

    p = projectile_rifle();
    p.projectile_speed = -1.0f;
    NF_CHECK(!weapon_valid(p));

    p = projectile_rifle();
    p.projectile_gravity = -0.1f;
    NF_CHECK(!weapon_valid(p));

    // A hitscan profile ignores both fields: an old scene with a stray
    // `gravity=` must not stop firing because the key is meaningless there.
    p = projectile_rifle();
    p.delivery = WeaponDelivery::Hitscan;
    p.projectile_speed = 0.0f;
    p.projectile_gravity = -5.0f;
    NF_CHECK(weapon_valid(p));
}

NF_TEST(the_trajectory_is_a_parabola_independent_of_frame_rate) {
    ProjectileBody slow;
    slow.position = Vec3{0.0f, 10.0f, 0.0f};
    slow.velocity = Vec3{30.0f, 0.0f, 0.0f};
    slow.lifetime_seconds = 10.0f;

    ProjectileBody fast = slow;
    const Vec3 gravity{0.0f, -9.81f, 0.0f};
    for (int i = 0; i < 60; ++i) {
        (void)projectile_advance(slow, gravity, 1.0f / 60.0f);
    }
    for (int i = 0; i < 30; ++i) {
        (void)projectile_advance(fast, gravity, 1.0f / 30.0f);
    }

    NF_CHECK_NEAR(slow.position.x, 30.0f, 0.01f);
    NF_CHECK_NEAR(fast.position.x, 30.0f, 0.01f);
    // 0.5*g*t^2 = 4.905 m after one second.
    NF_CHECK_NEAR(slow.position.y, 10.0f - 4.905f, 0.02f);
    NF_CHECK_NEAR(fast.position.y, slow.position.y, 0.02f);
    NF_CHECK_NEAR(slow.velocity.y, -9.81f, 0.02f);
    NF_CHECK_NEAR(fast.velocity.y, slow.velocity.y, 0.02f);
}

NF_TEST(a_step_stops_exactly_at_the_lifetime) {
    ProjectileBody body;
    body.velocity = Vec3{10.0f, 0.0f, 0.0f};
    body.lifetime_seconds = 1.0f;

    const ProjectileAdvance first = projectile_advance(body, Vec3{}, 0.75f);
    NF_CHECK(first.moved);
    NF_CHECK(!first.expired);
    NF_CHECK_NEAR(body.position.x, 7.5f, 1e-4f);

    const ProjectileAdvance last = projectile_advance(body, Vec3{}, 10.0f);
    NF_CHECK(last.moved);
    NF_CHECK(last.expired);
    NF_CHECK_NEAR(last.step_seconds, 0.25f, 1e-4f);
    NF_CHECK_NEAR(body.position.x, 10.0f, 1e-4f);
    NF_CHECK_NEAR(body.age_seconds, 1.0f, 1e-4f);
    NF_CHECK_NEAR(body.distance_travelled, 10.0f, 1e-4f);

    const ProjectileAdvance after = projectile_advance(body, Vec3{}, 1.0f);
    NF_CHECK(after.expired);
    NF_CHECK(!after.moved);
    NF_CHECK_NEAR(body.position.x, 10.0f, 1e-4f);
}

NF_TEST(a_paused_or_broken_step_advances_nothing) {
    ProjectileBody body;
    body.velocity = Vec3{5.0f, 0.0f, 0.0f};
    body.lifetime_seconds = 2.0f;

    NF_CHECK(!projectile_advance(body, Vec3{}, 0.0f).moved);
    NF_CHECK(!projectile_advance(body, Vec3{}, -1.0f).moved);
    NF_CHECK(!projectile_advance(body, Vec3{}, std::nanf("")).moved);
    NF_CHECK_NEAR(body.position.x, 0.0f, kEps);

    body.lifetime_seconds = 0.0f;
    NF_CHECK(!projectile_body_valid(body));
    NF_CHECK(!projectile_advance(body, Vec3{}, 1.0f).moved);
}

NF_TEST(lifetime_is_derived_from_range_and_speed) {
    NF_CHECK_NEAR(projectile_lifetime_for_range(100.0f, 20.0f), 5.0f, kEps);
    NF_CHECK_NEAR(projectile_lifetime_for_range(0.0f, 20.0f), 0.0f, kEps);
    NF_CHECK_NEAR(projectile_lifetime_for_range(100.0f, 0.0f), 0.0f, kEps);
}

NF_TEST(drop_matches_the_closed_form) {
    // 100 m at 100 m/s is one second; real gravity drops 4.905 m.
    NF_CHECK_NEAR(projectile_drop_at(100.0f, 100.0f, 9.81f), 4.905f, 0.01f);
    NF_CHECK_NEAR(projectile_drop_at(100.0f, 100.0f, 0.0f), 0.0f, kEps);
    NF_CHECK_NEAR(projectile_drop_at(100.0f, 0.0f, 9.81f), 0.0f, kEps);
}

NF_TEST(intercept_leads_a_crossing_target) {
    const ProjectileIntercept still =
        projectile_intercept(Vec3{}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{}, 20.0f);
    NF_CHECK(still.valid);
    NF_CHECK_NEAR(still.time_seconds, 0.5f, 1e-4f);
    NF_CHECK_NEAR(still.point.x, 10.0f, 1e-4f);
    NF_CHECK_NEAR(still.point.y, 0.0f, 1e-4f);

    // A target crossing at 10 m/s, a 20 m/s round: the meeting point is
    // ahead of the target, not on top of it.
    const ProjectileIntercept crossing = projectile_intercept(
        Vec3{}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{0.0f, 10.0f, 0.0f}, 20.0f);
    NF_CHECK(crossing.valid);
    NF_CHECK_NEAR(crossing.time_seconds, 0.57735f, 1e-4f);
    NF_CHECK_NEAR(crossing.point.x, 10.0f, 1e-4f);
    NF_CHECK_NEAR(crossing.point.y, 5.7735f, 1e-3f);
    NF_CHECK_NEAR(crossing.point.length(), 20.0f * crossing.time_seconds, 1e-3f);
}

NF_TEST(intercept_handles_the_equal_speed_case) {
    // Target speed equals projectile speed and it is coming straight in: the
    // quadratic's leading coefficient is zero, so this is the linear branch.
    const ProjectileIntercept closing = projectile_intercept(
        Vec3{}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{-10.0f, 0.0f, 0.0f}, 10.0f);
    NF_CHECK(closing.valid);
    NF_CHECK_NEAR(closing.time_seconds, 0.5f, 1e-4f);
    NF_CHECK_NEAR(closing.point.x, 5.0f, 1e-4f);

    // Running away at the same speed can never be caught.
    const ProjectileIntercept fleeing = projectile_intercept(
        Vec3{}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, 10.0f);
    NF_CHECK(!fleeing.valid);
    NF_CHECK_NEAR(fleeing.point.x, 10.0f, kEps); // current position fallback
}

NF_TEST(intercept_refuses_an_unreachable_or_invalid_shot) {
    const ProjectileIntercept outrun = projectile_intercept(
        Vec3{}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{20.0f, 0.0f, 0.0f}, 10.0f);
    NF_CHECK(!outrun.valid);
    NF_CHECK_NEAR(outrun.point.x, 10.0f, kEps);

    NF_CHECK(!projectile_intercept(Vec3{}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{}, 0.0f).valid);
    NF_CHECK(!projectile_intercept(Vec3{}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{}, -5.0f).valid);
    NF_CHECK(!projectile_intercept(Vec3{}, Vec3{10.0f, 0.0f, 0.0f},
                                   Vec3{std::nanf(""), 0.0f, 0.0f}, 20.0f)
                  .valid);
}

// --- the sides ---------------------------------------------------------------

NF_TEST(teams_are_neutral_friendly_or_hostile) {
    NF_CHECK(team_relation("", "Blue") == TeamRelation::Neutral);
    NF_CHECK(team_relation("Blue", "") == TeamRelation::Neutral);
    NF_CHECK(team_relation("", "") == TeamRelation::Neutral);
    NF_CHECK(team_relation("Blue", "Blue") == TeamRelation::Friendly);
    NF_CHECK(team_relation("Blue", "Red") == TeamRelation::Hostile);
    NF_CHECK(std::string(team_relation_name(TeamRelation::Hostile)) == "hostile");
}

NF_TEST(friendly_fire_is_blocked_unless_the_attacker_opts_in) {
    const TeamIdentity blue{"Blue", false};
    const TeamIdentity blue_shooter{"Blue", true};
    const TeamIdentity red{"Red", false};
    const TeamIdentity none{};

    NF_CHECK(!team_damage_allowed(blue, blue));
    NF_CHECK(team_damage_allowed(blue_shooter, blue));
    NF_CHECK(team_damage_allowed(blue, red));
    NF_CHECK(team_damage_allowed(red, blue));
    NF_CHECK(team_damage_allowed(none, blue));
    NF_CHECK(team_damage_allowed(blue, none));

    // The victim's licence does not make it shootable: only the attacker's
    // flag is read.
    const TeamIdentity blue_victim_opt_in{"Blue", true};
    NF_CHECK(!team_damage_allowed(blue, blue_victim_opt_in));
}

// --- the file ----------------------------------------------------------------

NF_TEST(projectile_and_faction_lines_round_trip) {
    scene::Scene source("Sides");
    const ecs::Entity shooter = source.world().create_entity();
    source.world().add<scene::NameComponent>(shooter, scene::NameComponent{"Shooter"});
    source.world().add<scene::Transform>(shooter);

    WeaponProfile p = projectile_rifle();
    p.delivery = WeaponDelivery::Projectile;
    p.projectile_speed = 340.5f;
    p.projectile_gravity = 4.2f;
    source.world().add<WeaponComponent>(shooter, WeaponComponent{p});
    source.world().add<FactionComponent>(shooter, FactionComponent{"Blue", true, true});

    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("delivery=projectile") != std::string::npos);
    NF_CHECK(text.find("speed=340.5") != std::string::npos);
    NF_CHECK(text.find("gravity=") != std::string::npos);
    NF_CHECK(text.find("  Faction: team=Blue friendly_fire=true") != std::string::npos);

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
        NF_CHECK(w->profile.delivery == WeaponDelivery::Projectile);
        NF_CHECK_NEAR(w->profile.projectile_speed, 340.5f, 1e-4f);
        NF_CHECK_NEAR(w->profile.projectile_gravity, 4.2f, 1e-4f);
    }
    const FactionComponent* f = loaded.scene->world().get<FactionComponent>(found);
    NF_CHECK(f != nullptr);
    if (f != nullptr) {
        NF_CHECK(f->team == "Blue");
        NF_CHECK(f->friendly_fire);
        NF_CHECK(f->enabled);
    }
}

NF_TEST(the_shipped_ballistics_scene_loads_and_validates) {
    const std::string path = NF_BALLISTICS_SCENE_PATH;
    if (path.empty()) {
        NF_SKIP("no source path configured for the sample scene");
    }
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error) && !exists_error;
    NF_CHECK(exists); // a missing shipped sample is a real failure
    if (!exists) {
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        // Some locked-down CI sandboxes deny read access to the source tree
        // even though the file is present. That is an environment limit, not a
        // broken scene; the pure parser cases still cover the format.
        NF_SKIP("sample scene exists but is not readable in this environment");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    NF_CHECK(loaded.warnings.empty());
    if (!loaded.success || !loaded.scene) {
        return;
    }

    u32 projectiles = 0u;
    u32 factions = 0u;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        if (const auto* weapon = loaded.scene->world().get<WeaponComponent>(e);
            weapon != nullptr && weapon->profile.delivery == WeaponDelivery::Projectile) {
            ++projectiles;
        }
        if (loaded.scene->world().get<FactionComponent>(e) != nullptr) {
            ++factions;
        }
    }
    NF_CHECK_EQ(projectiles, 2u);
    // Player, Ally, Enemy, AllyCrate, EnemyCrate. The Player joined the Blue
    // team in Phase 46 so friendly fire has an owner to filter against.
    NF_CHECK_EQ(factions, 5u);

    const ValidationReport report = validate_scene(*loaded.scene, path);
    NF_CHECK_EQ(report.errors, 0u);
    for (const ValidationIssue& issue : report.issues) {
        // A shipped sample that warns about its own team setup would teach the
        // wrong lesson.
        NF_CHECK(issue.message.find("own team") == std::string::npos);
    }
}

NF_TEST(auto_target_round_trips_and_is_ignored_when_off) {
    scene::Scene source("AutoTarget");
    const ecs::Entity guard = source.world().create_entity();
    source.world().add<scene::NameComponent>(guard, scene::NameComponent{"Guard"});
    source.world().add<scene::Transform>(guard);
    source.world().add<FactionComponent>(guard, FactionComponent{"Blue"});

    CombatAgentComponent agent;
    agent.auto_target = true;
    agent.target = "OldName";
    source.world().add<CombatAgentComponent>(guard, agent);

    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("auto_target=true") != std::string::npos);
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.success || !loaded.scene) {
        return;
    }
    ecs::Entity found = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        const scene::NameComponent* n = loaded.scene->world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Guard") {
            found = e;
        }
    }
    NF_CHECK(found.valid());
    const CombatAgentComponent* stored =
        loaded.scene->world().get<CombatAgentComponent>(found);
    NF_CHECK(stored != nullptr);
    if (stored != nullptr) {
        NF_CHECK(stored->auto_target);
        NF_CHECK(stored->target == "OldName"); // kept, but ignored by the runtime
    }
}

NF_TEST(the_validator_names_auto_target_without_a_team) {
    scene::Scene scene("NoSide");
    const ecs::Entity guard = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(guard, scene::NameComponent{"Guard"});
    scene.world().add<scene::Transform>(guard);
    CombatAgentComponent agent;
    agent.auto_target = true;
    scene.world().add<CombatAgentComponent>(guard, agent);

    const ValidationReport report = validate_scene(scene, "content://Scenes/NoSide.nfscene");
    NF_CHECK_EQ(report.errors, 0u);
    bool warned = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("no team") != std::string::npos) {
            warned = true;
        }
    }
    NF_CHECK(warned);
}

NF_TEST(an_unknown_delivery_is_refused_whole) {
    const std::string text =
        "# SANAD Scene v1\n"
        "version: 1\n"
        "name: BadDelivery\n"
        "entity_count: 1\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: Shooter\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(-1:0)\n"
        "  Weapon: delivery=laser rpm=600\n";
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    NF_CHECK(!loaded.warnings.empty());
    if (loaded.scene) {
        for (ecs::Entity e : loaded.scene->world().all_entities()) {
            NF_CHECK(loaded.scene->world().get<WeaponComponent>(e) == nullptr);
        }
    }
}

// --- the validator -----------------------------------------------------------

NF_TEST(the_validator_warns_when_an_agent_targets_its_own_team) {
    scene::Scene scene("Allies");
    const ecs::Entity guard = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(guard, scene::NameComponent{"Guard"});
    scene.world().add<scene::Transform>(guard);
    scene.world().add<FactionComponent>(guard, FactionComponent{"Blue"});
    CombatAgentComponent agent;
    agent.target = "Friend";
    scene.world().add<CombatAgentComponent>(guard, agent);

    const ecs::Entity friend_entity = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(friend_entity, scene::NameComponent{"Friend"});
    scene.world().add<scene::Transform>(friend_entity);
    scene.world().add<FactionComponent>(friend_entity, FactionComponent{"Blue"});
    scene.world().add<HealthComponent>(friend_entity, HealthComponent{});

    const ValidationReport report = validate_scene(scene, "content://Scenes/Allies.nfscene");
    NF_CHECK_EQ(report.errors, 0u);
    bool warned = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("own team") != std::string::npos) {
            warned = true;
        }
    }
    NF_CHECK(warned);

    // The attacker's licence clears the warning: the scene now means what it
    // says, even if what it says is unusual.
    FactionComponent* side = scene.world().get<FactionComponent>(guard);
    NF_CHECK(side != nullptr);
    if (side != nullptr) {
        side->friendly_fire = true;
    }
    const ValidationReport licensed = validate_scene(scene, "content://Scenes/Allies.nfscene");
    for (const ValidationIssue& issue : licensed.issues) {
        NF_CHECK(issue.message.find("own team") == std::string::npos);
    }
}

// --- the runtime -------------------------------------------------------------

NF_TEST(a_projectile_does_no_damage_until_it_arrives) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_projectile_runtime";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        assets::AssetRegistry registry;
        assets::AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto world_scene = std::make_unique<scene::Scene>("Range");
        const ecs::Entity player = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(player, scene::NameComponent{"Player"});
        world_scene->world().add<scene::Transform>(player);

        WeaponProfile p = projectile_rifle();
        world_scene->world().add<WeaponComponent>(player, WeaponComponent{p});
        world_scene->world().add<FactionComponent>(player, FactionComponent{"Blue"});

        HealthComponent player_health;
        world_scene->world().add<HealthComponent>(player, player_health);

        const ecs::Entity target = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(target, scene::NameComponent{"Target"});
        auto& tt = world_scene->world().add<scene::Transform>(target);
        tt.local_y = 1.5f;
        tt.local_z = -10.0f;
        world_scene->world().add<physics::ColliderComponent>(
            target, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})});
        HealthComponent target_health;
        target_health.max_health = 30.0f;
        target_health.health = 30.0f;
        world_scene->world().add<HealthComponent>(target, target_health);
        world_scene->world().add<FactionComponent>(target, FactionComponent{"Red"});

        const ecs::Entity camera = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
        world_scene->world().add<scene::Transform>(camera);
        CameraComponent cam;
        cam.is_active = true;
        cam.follow_target = "Player";
        cam.pivot_y = 1.5f;
        cam.distance = 4.0f;
        // Aim flat at the target. The camera's default pitch looks 15 degrees
        // down (right for a third-person orbit, wrong for a geometry test
        // that expects the shot to cross the whole range at eye height).
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
        cam.mouse_look = false;
        world_scene->world().add<CameraComponent>(camera, cam);

        rt.adopt_scene(std::move(world_scene), "content://Scenes/Range.nfscene");

        NF_CHECK_EQ(rt.fire_weapon(), 1u);
        const Runtime::WeaponSnapshot spawned = rt.weapon_snapshot();
        NF_CHECK_EQ(spawned.shots_fired, 1u);
        NF_CHECK_EQ(spawned.active_projectiles, 1u);
        NF_CHECK_EQ(spawned.hits_landed, 0u);

        const HealthComponent* th = rt.health_of(target);
        NF_CHECK(th != nullptr);
        if (th != nullptr) {
            NF_CHECK_NEAR(th->health, 30.0f, kEps);
        }

        // 0.5 s at 20 m/s covers 10 m; the near face is 13.5 m from the
        // camera, so the round is still in the air.
        for (int i = 0; i < 30; ++i) {
            rt.update(1.0f / 60.0f);
        }
        const Runtime::WeaponSnapshot flying = rt.weapon_snapshot();
        NF_CHECK_EQ(flying.active_projectiles, 1u);
        NF_CHECK_EQ(flying.hits_landed, 0u);
        NF_CHECK(th != nullptr);
        if (th != nullptr) {
            NF_CHECK_NEAR(th->health, 30.0f, 1e-3f);
        }

        // Another 0.25 s reaches the target. Damage lands, the round is gone,
        // and the last-hit record names it.
        for (int i = 0; i < 15; ++i) {
            rt.update(1.0f / 60.0f);
        }
        const Runtime::WeaponSnapshot landed = rt.weapon_snapshot();
        NF_CHECK_EQ(landed.active_projectiles, 0u);
        NF_CHECK_EQ(landed.hits_landed, 1u);
        NF_CHECK_NEAR(landed.damage_dealt, 10.0f, 0.01f);
        const Runtime::WeaponHitInfo& hit = rt.last_weapon_hit();
        NF_CHECK(hit.hit);
        NF_CHECK(hit.name == "Target");
        NF_CHECK(!hit.blocked_by_faction);
        NF_CHECK(th != nullptr);
        if (th != nullptr) {
            NF_CHECK_NEAR(th->health, 20.0f, 0.05f);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(the_runtime_blocks_a_shot_into_a_teammate) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_faction_runtime";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        assets::AssetRegistry registry;
        assets::AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto world_scene = std::make_unique<scene::Scene>("Sides");
        const ecs::Entity player = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(player, scene::NameComponent{"Player"});
        world_scene->world().add<scene::Transform>(player);
        WeaponProfile p;
        p.mode = FireMode::Semi;
        p.magazine = 4u;
        p.damage = 10.0f;
        p.range = 100.0f;
        p.spread_deg = 0.0f;
        p.spread_max_deg = 1.0f;
        p.spread_bloom_deg = 0.0f;
        p.recoil_pitch = 0.0f;
        p.recoil_yaw = 0.0f;
        p.shake = 0.0f;
        world_scene->world().add<WeaponComponent>(player, WeaponComponent{p});
        world_scene->world().add<FactionComponent>(player, FactionComponent{"Blue"});
        world_scene->world().add<HealthComponent>(player, HealthComponent{});

        const ecs::Entity ally = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(ally, scene::NameComponent{"Ally"});
        auto& at = world_scene->world().add<scene::Transform>(ally);
        at.local_y = 1.5f;
        at.local_z = -10.0f;
        world_scene->world().add<physics::ColliderComponent>(
            ally, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})});
        HealthComponent ally_health;
        ally_health.max_health = 30.0f;
        ally_health.health = 30.0f;
        world_scene->world().add<HealthComponent>(ally, ally_health);
        world_scene->world().add<FactionComponent>(ally, FactionComponent{"Blue"});

        const ecs::Entity camera = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
        world_scene->world().add<scene::Transform>(camera);
        CameraComponent cam;
        cam.is_active = true;
        cam.follow_target = "Player";
        cam.pivot_y = 1.5f;
        cam.distance = 4.0f;
        // Aim flat at the target. The camera's default pitch looks 15 degrees
        // down (right for a third-person orbit, wrong for a geometry test
        // that expects the shot to cross the whole range at eye height).
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
        cam.mouse_look = false;
        world_scene->world().add<CameraComponent>(camera, cam);

        rt.adopt_scene(std::move(world_scene), "content://Scenes/Sides.nfscene");
        NF_CHECK_EQ(rt.fire_weapon(), 1u);

        const Runtime::WeaponHitInfo& blocked = rt.last_weapon_hit();
        NF_CHECK(blocked.hit);
        NF_CHECK(blocked.name == "Ally");
        NF_CHECK(blocked.blocked_by_faction);
        NF_CHECK_NEAR(blocked.applied, 0.0f, kEps);
        const HealthComponent* ah = rt.health_of(ally);
        NF_CHECK(ah != nullptr);
        if (ah != nullptr) {
            NF_CHECK_NEAR(ah->health, 30.0f, kEps);
        }

        // The attacker can opt in. The next round is fired by a weapon whose
        // owner now licenses friendly fire, and the same geometry hurts.
        WeaponComponent* weapon = rt.edit_scene()->world().get<WeaponComponent>(player);
        NF_CHECK(weapon != nullptr);
        FactionComponent* faction = rt.edit_scene()->world().get<FactionComponent>(player);
        NF_CHECK(faction != nullptr);
        if (weapon != nullptr && faction != nullptr) {
            faction->friendly_fire = true;
            weapon->state.cooldown = 0.0f;
            NF_CHECK_EQ(rt.fire_weapon(), 1u);
            const Runtime::WeaponHitInfo& allowed = rt.last_weapon_hit();
            NF_CHECK(!allowed.blocked_by_faction);
            NF_CHECK_NEAR(allowed.applied, 10.0f, 0.01f);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
