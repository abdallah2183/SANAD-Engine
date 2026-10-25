// Tests/RuntimeTests/test_combat_feedback.cpp - Phase 56: the visible half of
// the fight.
//
// Phases 49-55 publish what happened: `ShotEvent` says a pellet left a weapon,
// `ImpactEvent` says what it hit. Nothing consumed them automatically, so a
// scene with a working gun still fired silently and left no mark. This file
// covers the three contracts that make the feedback layer real:
//
//   1. the director's arithmetic: one flash and one shot cue per volley, one
//      tracer per pellet, one scorch and one impact cue per surface hit (never
//      per status tick or splash victim), deterministic caps, and ageing
//      BEFORE ingest so an effect is guaranteed at least one rendered frame;
//   2. the scene line: `Feedback:` round-trips through the real writer/reader,
//      an absent line stays absent, and an impossible value is refused by name;
//   3. the runtime bridge: a real `fire_weapon()` becomes flashes, tracers and
//      a projected decal in `RenderWorld`, and the procedural one-shots reach
//      the mixer. A component the renderer never sees is the dead-subsystem
//      failure this project has paid for before.
//
// The pure half needs no device. The integration half skips without Vulkan,
// like every other Runtime extraction test.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Runtime/CombatFeedback.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
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
using namespace nf::assets;
using namespace nf::runtime;

namespace {

ShotEvent shot(u32 pellet = 1u, u32 pellets = 1u, f32 x = 0.0f,
               const Vec3& direction = Vec3{0.0f, 0.0f, -1.0f}) {
    ShotEvent event;
    event.shooter = ecs::Entity{1u, 0u};
    event.origin = Vec3{x, 1.0f, 2.0f};
    event.direction = direction;
    event.damage = 10.0f;
    event.pellet = pellet;
    event.pellets = pellets;
    event.seed = 1234u;
    return event;
}

ImpactEvent impact(f32 x = 0.0f, bool status = false, bool splash = false) {
    ImpactEvent event;
    event.shooter = ecs::Entity{1u, 0u};
    event.victim = ecs::Entity{2u, 0u};
    event.position = Vec3{x, 1.5f, -5.0f};
    event.normal = Vec3{0.0f, 0.0f, 1.0f};
    event.damage = 10.0f;
    event.applied = 10.0f;
    event.splash = splash;
    event.status = status;
    return event;
}

const std::vector<ShotEvent> no_shots;
const std::vector<ImpactEvent> no_impacts;

} // namespace

// --- the director ------------------------------------------------------------

NF_TEST(feedback_volley_makes_one_flash_and_one_shot_cue_not_one_per_pellet) {
    CombatFeedback feedback;
    feedback.configure(CombatFeedbackConfig{});

    // A three-pellet shotgun volley. The flash and the bang belong to the
    // trigger pull; the tracers belong to the pellets.
    std::vector<ShotEvent> shots{
        shot(1u, 3u, 0.0f), shot(2u, 3u, 0.0f), shot(3u, 3u, 0.0f)};
    // One real surface hit, one DoT tick, and one splash victim. Only the first
    // is a new round landing on a wall.
    std::vector<ImpactEvent> impacts{
        impact(0.0f, false, false), impact(1.0f, true, false),
        impact(2.0f, false, true)};

    feedback.update(1.0f / 60.0f, shots, impacts);

    NF_CHECK_EQ(feedback.flashes().size(), static_cast<size_t>(1));
    NF_CHECK_EQ(feedback.tracers().size(), static_cast<size_t>(3));
    NF_CHECK_EQ(feedback.impacts().size(), static_cast<size_t>(1));
    NF_CHECK_EQ(feedback.sounds().size(), static_cast<size_t>(2));

    if (feedback.flashes().size() == 1) {
        const FeedbackFlash& flash = feedback.flashes()[0];
        NF_CHECK_NEAR(flash.position.x, 0.0f, 1e-5f);
        NF_CHECK_NEAR(flash.age, 0.0f, 1e-6f);
        NF_CHECK_NEAR(flash.strength, 1.0f, 1e-6f);
    }
    if (feedback.tracers().size() == 3) {
        // The beam starts just past the muzzle along the pellet's own
        // direction, not at the camera's centre aim.
        for (const FeedbackTracer& tracer : feedback.tracers()) {
            NF_CHECK_NEAR(tracer.start.z, 2.0f - 0.35f, 1e-5f);
            NF_CHECK_NEAR(tracer.end.z, 2.0f - 0.35f - 9.0f, 1e-4f);
        }
    }
    if (!feedback.sounds().empty()) {
        NF_CHECK(feedback.sounds()[0].kind == FeedbackSound::Kind::Shot);
    }
}

NF_TEST(feedback_ages_before_ingesting_so_a_new_effect_draws_one_frame) {
    CombatFeedback feedback;
    feedback.configure(CombatFeedbackConfig{});

    // dt is longer than every lifetime. Ageing first means the effects spawned
    // by this call are at age 0, not instantly expired: an effect the player
    // never sees is the bug this ordering exists to prevent.
    std::vector<ShotEvent> shots{shot()};
    std::vector<ImpactEvent> impacts{impact()};
    feedback.update(10.0f, shots, impacts);
    NF_CHECK_EQ(feedback.flashes().size(), static_cast<size_t>(1));
    NF_CHECK_EQ(feedback.tracers().size(), static_cast<size_t>(1));
    NF_CHECK_EQ(feedback.impacts().size(), static_cast<size_t>(1));

    // The next frame ages them past their lives and retires all three.
    feedback.update(10.0f, no_shots, no_impacts);
    NF_CHECK(feedback.flashes().empty());
    NF_CHECK(feedback.tracers().empty());
    NF_CHECK(feedback.impacts().empty());
    NF_CHECK(feedback.sounds().empty());
}

NF_TEST(feedback_caps_drop_the_oldest_deterministically) {
    CombatFeedbackConfig config;
    config.max_flashes = 2u;
    config.max_tracers = 2u;
    config.max_impacts = 2u;
    config.max_sounds = 2u;
    CombatFeedback feedback;
    feedback.configure(config);

    // Three single-pellet volleys in one update, each with its own impact.
    std::vector<ShotEvent> shots{shot(1u, 1u, 0.0f), shot(1u, 1u, 1.0f),
                                 shot(1u, 1u, 2.0f)};
    std::vector<ImpactEvent> impacts{impact(0.0f), impact(1.0f), impact(2.0f)};
    feedback.update(1.0f / 60.0f, shots, impacts);

    NF_CHECK_EQ(feedback.flashes().size(), static_cast<size_t>(2));
    NF_CHECK_EQ(feedback.tracers().size(), static_cast<size_t>(2));
    NF_CHECK_EQ(feedback.impacts().size(), static_cast<size_t>(2));
    NF_CHECK_EQ(feedback.sounds().size(), static_cast<size_t>(2));

    if (feedback.flashes().size() == 2) {
        // The newest two survive: the origin x is the deterministic order.
        NF_CHECK_NEAR(feedback.flashes()[0].position.x, 1.0f, 1e-5f);
        NF_CHECK_NEAR(feedback.flashes()[1].position.x, 2.0f, 1e-5f);
    }
}

NF_TEST(feedback_configure_clamps_a_hostile_component) {
    CombatFeedback feedback;
    CombatFeedbackConfig config;
    config.flash_seconds = -1.0f;
    config.tracer_width = 0.0f;
    config.impact_size = 0.0f;
    config.shot_volume = -5.0f;
    config.impact_volume = 99.0f;
    config.max_flashes = 0u;
    config.max_tracers = 0u;
    config.max_sounds = 0u;
    config.tracer_length = std::nanf("");
    feedback.configure(config);

    const CombatFeedbackConfig& out = feedback.config();
    NF_CHECK(out.flash_seconds > 0.0f);
    NF_CHECK(out.tracer_width > 0.0f);
    NF_CHECK(out.impact_size > 0.0f);
    NF_CHECK_NEAR(out.shot_volume, 0.0f, 1e-6f);
    NF_CHECK(out.impact_volume <= 4.0f);
    NF_CHECK(out.max_flashes >= 1u);
    NF_CHECK(out.max_tracers >= 1u);
    NF_CHECK(out.max_sounds >= 1u);
    NF_CHECK(std::isfinite(out.tracer_length));
    NF_CHECK(out.tracer_length > 0.0f);
}

NF_TEST(feedback_audio_off_keeps_the_visuals_and_drops_every_cue) {
    CombatFeedbackConfig config;
    config.audio = false;
    CombatFeedback feedback;
    feedback.configure(config);

    std::vector<ShotEvent> shots{shot()};
    std::vector<ImpactEvent> impacts{impact()};
    feedback.update(1.0f / 60.0f, shots, impacts);

    NF_CHECK_EQ(feedback.flashes().size(), static_cast<size_t>(1));
    NF_CHECK_EQ(feedback.tracers().size(), static_cast<size_t>(1));
    NF_CHECK_EQ(feedback.impacts().size(), static_cast<size_t>(1));
    NF_CHECK(feedback.sounds().empty());
}

// --- the scene line ----------------------------------------------------------

NF_TEST(feedback_line_round_trips_every_key) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_feedback_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    scene::Scene scene("FeedbackScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    FeedbackComponent fb;
    fb.enabled = false;
    fb.audio = false;
    fb.flash_seconds = 0.11f;
    fb.flash_size = 0.4f;
    fb.tracer_seconds = 0.2f;
    fb.tracer_length = 14.0f;
    fb.tracer_width = 0.05f;
    fb.impact_seconds = 9.0f;
    fb.impact_size = 0.3f;
    fb.shot_volume = 0.9f;
    fb.impact_volume = 0.25f;
    world.add<FeedbackComponent>(e, fb);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/feedback.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/feedback.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const auto* loaded = result.scene->world().get<FeedbackComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(!loaded->enabled);
        NF_CHECK(!loaded->audio);
        NF_CHECK_NEAR(loaded->flash_seconds, 0.11f, 1e-5f);
        NF_CHECK_NEAR(loaded->flash_size, 0.4f, 1e-5f);
        NF_CHECK_NEAR(loaded->tracer_seconds, 0.2f, 1e-5f);
        NF_CHECK_NEAR(loaded->tracer_length, 14.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->tracer_width, 0.05f, 1e-5f);
        NF_CHECK_NEAR(loaded->impact_seconds, 9.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->impact_size, 0.3f, 1e-5f);
        NF_CHECK_NEAR(loaded->shot_volume, 0.9f, 1e-5f);
        NF_CHECK_NEAR(loaded->impact_volume, 0.25f, 1e-5f);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(feedback_absent_from_a_scene_stays_absent_when_saved) {
    scene::Scene scene("PlainScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    const std::string text = serialize_scene_to_text(scene);
    NF_CHECK(text.find("Feedback:") == std::string::npos);

    auto parsed = load_scene_from_text(text);
    NF_CHECK(parsed.success);
    NF_CHECK(parsed.scene != nullptr);
    if (parsed.scene) {
        NF_CHECK(parsed.scene->world().get<FeedbackComponent>(
                      parsed.scene->world().all_entities()[0]) == nullptr);
    }
}

NF_TEST(feedback_line_with_an_impossible_duration_is_refused_by_name) {
    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: BadFeedback\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Player\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
        "parent(4294967295:0)\n"
        "  Feedback: flash_time=-1 tracer_time=0.2\n";
    auto result = load_scene_from_text(text);
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        return;
    }
    NF_CHECK(result.scene->world().get<FeedbackComponent>(
                  result.scene->world().all_entities()[0]) == nullptr);

    bool named = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("Feedback") != std::string::npos &&
            warning.find("flash_time") != std::string::npos) {
            named = true;
            break;
        }
    }
    NF_CHECK(named);
}

// --- the runtime bridge ------------------------------------------------------

NF_TEST(runtime_turns_a_real_shot_into_flashes_tracers_scorch_and_sound) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_feedback_runtime";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("FeedbackRange");

        const ecs::Entity player = game->world().create_entity();
        game->world().add<scene::NameComponent>(player, scene::NameComponent{"Player"});
        game->world().add<scene::Transform>(player, scene::Transform{});

        WeaponProfile weapon;
        weapon.mode = FireMode::Semi;
        weapon.magazine = 3u;
        weapon.reserve = 3u;
        weapon.reload_seconds = 0.5f;
        weapon.damage = 10.0f;
        weapon.range = 100.0f;
        weapon.spread_deg = 0.0f;
        weapon.spread_max_deg = 1.0f;
        weapon.spread_bloom_deg = 0.0f;
        weapon.recoil_pitch = 0.0f;
        weapon.recoil_yaw = 0.0f;
        weapon.shake = 0.0f;
        game->world().add<WeaponComponent>(player, WeaponComponent{weapon});
        // The opt-in. Without this component the same shot produces no object,
        // no decal and no cue - the absent-means-unchanged contract.
        game->world().add<FeedbackComponent>(player, FeedbackComponent{});

        HealthComponent player_health;
        player_health.max_health = 100.0f;
        player_health.health = 100.0f;
        game->world().add<HealthComponent>(player, player_health);

        const ecs::Entity target = game->world().create_entity();
        game->world().add<scene::NameComponent>(target, scene::NameComponent{"Target"});
        auto& tt = game->world().add<scene::Transform>(target);
        tt.local_y = 1.5f;
        tt.local_z = -10.0f;
        game->world().add<physics::ColliderComponent>(
            target,
            physics::ColliderComponent{physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})});
        HealthComponent target_health;
        target_health.max_health = 30.0f;
        target_health.health = 30.0f;
        game->world().add<HealthComponent>(target, target_health);

        const ecs::Entity camera = game->world().create_entity();
        game->world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
        game->world().add<scene::Transform>(camera, scene::Transform{});
        CameraComponent cam;
        cam.is_active = true;
        cam.follow_target = "Player";
        cam.pivot_y = 1.5f;
        cam.distance = 4.0f;
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
        cam.mouse_look = false;
        game->world().add<CameraComponent>(camera, cam);

        rt.adopt_scene(std::move(game), "content://Scenes/FeedbackRange.nfscene");
        NF_CHECK(rt.combat_feedback_bound());

        NF_CHECK_EQ(rt.fire_weapon(), 1u);

        // One update publishes the shot/impact to the director. The frame is
        // short enough to mix no whole audio block, which is exactly the case
        // that used to lose a cue.
        rt.update(1.0f / 60.0f);
        const CombatFeedback& feedback = rt.combat_feedback();
        NF_CHECK_EQ(feedback.flashes().size(), static_cast<size_t>(1));
        NF_CHECK_EQ(feedback.tracers().size(), static_cast<size_t>(1));
        NF_CHECK_EQ(feedback.impacts().size(), static_cast<size_t>(1));

        // The renderer receives a flash cube, a tracer cube and one projected
        // scorch decal. Render-only ids stay 0 so picking never reports them.
        rendering::RenderWorld world = rt.render_world_snapshot();
        size_t feedback_objects = 0;
        for (const rendering::RenderObject& object : world.objects) {
            if (object.id == 0u && object.mesh_handle.valid()) {
                ++feedback_objects;
            }
        }
        NF_CHECK(feedback_objects >= 2u);
        NF_CHECK_EQ(world.decals.size(), static_cast<size_t>(1));
        if (world.decals.size() == 1) {
            NF_CHECK(world.decals[0].enabled);
            NF_CHECK_NEAR(world.decals[0].opacity, 1.0f, 1e-5f);
            // The decal's local +Z faces the hit surface normal. Size is baked
            // into the matrix, so the row is the size-scaled axis: normalize
            // before asserting the direction.
            const Vec3 facing{world.decals[0].world.m[2][0],
                              world.decals[0].world.m[2][1],
                              world.decals[0].world.m[2][2]};
            const Vec3 unit_facing = facing.normalized();
            NF_CHECK_NEAR(unit_facing.x, 0.0f, 1e-3f);
            NF_CHECK_NEAR(unit_facing.y, 0.0f, 1e-3f);
            NF_CHECK_NEAR(unit_facing.z, 1.0f, 1e-3f);
            // Its translation is the impact, not the shooter.
            NF_CHECK_NEAR(world.decals[0].world.m[3][2], -9.5f, 0.05f);
        }

        // The next frame reaches a whole 1024-frame block: the procedural
        // gunshot started by the pending cue must actually reach the mix.
        rt.update(1.0f / 60.0f);
        NF_CHECK(rt.audio_output_frames() > 0u);
        NF_CHECK(rt.audio_output_peak() > 0.001f);

        // A few frames past the flash and the tracer lives: both are retired,
        // the six-second scorch is still drawn.
        for (int frame = 0; frame < 12; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.combat_feedback().flashes().empty());
        NF_CHECK(rt.combat_feedback().tracers().empty());
        NF_CHECK_EQ(rt.combat_feedback().impacts().size(), static_cast<size_t>(1));

        rendering::RenderWorld later = rt.render_world_snapshot();
        NF_CHECK_EQ(later.decals.size(), static_cast<size_t>(1));

        // The compatibility half: the same runtime and the same fired shot in a
        // scene that never asked for feedback must draw and mix nothing new.
        auto plain = std::make_unique<scene::Scene>("NoFeedback");
        const ecs::Entity plain_player = plain->world().create_entity();
        plain->world().add<scene::NameComponent>(
            plain_player, scene::NameComponent{"Player"});
        plain->world().add<scene::Transform>(plain_player, scene::Transform{});
        plain->world().add<WeaponComponent>(plain_player, WeaponComponent{weapon});
        const ecs::Entity plain_camera = plain->world().create_entity();
        plain->world().add<scene::NameComponent>(
            plain_camera, scene::NameComponent{"Camera"});
        plain->world().add<scene::Transform>(plain_camera, scene::Transform{});
        CameraComponent plain_cam;
        plain_cam.is_active = true;
        plain_cam.follow_target = "Player";
        plain_cam.pivot_y = 1.5f;
        plain_cam.distance = 4.0f;
        plain_cam.yaw = 0.0f;
        plain_cam.pitch = 0.0f;
        plain_cam.mouse_look = false;
        plain->world().add<CameraComponent>(plain_camera, plain_cam);

        rt.adopt_scene(std::move(plain), "content://Scenes/NoFeedback.nfscene");
        NF_CHECK(!rt.combat_feedback_bound());
        NF_CHECK_EQ(rt.fire_weapon(), 1u);
        rt.update(1.0f / 60.0f);
        NF_CHECK(rt.combat_feedback().empty());
        rendering::RenderWorld without = rt.render_world_snapshot();
        NF_CHECK(without.objects.empty());
        NF_CHECK(without.decals.empty());
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
