// Tests/RuntimeTests/test_combat_ai.cpp — an NPC that can shoot back.
//
// Phase 44 gave the engine a gun. A gun only the player can pull is half a
// fight, so Phase 45 adds the four decisions every shooter makes: notice, turn,
// decide, and hold fire between bursts. They live in CombatAI.hpp as arithmetic
// because that is the only way to test them — an aim bug found by watching an
// NPC miss is an afternoon, and an aim bug found by reading a number is a
// minute.
//
// The last case is the integration: a real scene, a real update loop, and a
// player whose health actually drops.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/CombatAI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr f32 kEps = 1e-4f;

/// An agent that is already looking at its target: no turn, no reaction.
CombatAimConfig quick_config() {
    CombatAimConfig c;
    c.reaction_seconds = 0.0f;
    c.aim_speed_deg_per_s = 720.0f;
    c.fire_cone_deg = 3.0f;
    c.max_range = 100.0f;
    c.burst_on_seconds = 1.0f;
    c.burst_off_seconds = 0.5f;
    return c;
}

CombatAimInput target_at(f32 x, f32 y, f32 z) {
    CombatAimInput in;
    in.has_target = true;
    in.eye = Vec3{0.0f, 0.0f, 0.0f};
    in.target = Vec3{x, y, z};
    in.visible = true;
    in.dt = 1.0f / 60.0f;
    return in;
}

} // namespace

// --- the angles --------------------------------------------------------------

NF_TEST(aim_angles_and_direction_round_trip) {
    const f32 yaws[] = {0.0f, 45.0f, 90.0f, -90.0f, 179.0f, -179.0f};
    const f32 pitches[] = {0.0f, 15.0f, -30.0f, 45.0f};
    for (const f32 yaw : yaws) {
        for (const f32 pitch : pitches) {
            const Vec3 dir = aim_direction(yaw, pitch);
            NF_CHECK_NEAR(dir.length(), 1.0f, 1e-4f);
            f32 back_yaw = 0.0f;
            f32 back_pitch = 0.0f;
            aim_angles(dir, back_yaw, back_pitch);
            NF_CHECK_NEAR(angle_delta_deg(back_yaw, yaw), 0.0f, 1e-2f);
            NF_CHECK_NEAR(back_pitch, pitch, 1e-2f);
        }
    }
    // The convention, spelled out: yaw 0 / pitch 0 looks down -Z, positive yaw
    // turns left (-X), positive pitch looks up.
    const Vec3 forward = aim_direction(0.0f, 0.0f);
    NF_CHECK_NEAR(forward.z, -1.0f, kEps);
    const Vec3 left = aim_direction(90.0f, 0.0f);
    NF_CHECK_NEAR(left.x, -1.0f, kEps);
    const Vec3 up = aim_direction(0.0f, 90.0f);
    NF_CHECK_NEAR(up.y, 1.0f, kEps);
}

NF_TEST(a_zero_direction_has_no_angles) {
    f32 yaw = 123.0f;
    f32 pitch = 45.0f;
    aim_angles(Vec3{0.0f, 0.0f, 0.0f}, yaw, pitch);
    NF_CHECK_NEAR(yaw, 0.0f, kEps);
    NF_CHECK_NEAR(pitch, 0.0f, kEps);
}

NF_TEST(angle_delta_takes_the_short_way) {
    NF_CHECK_NEAR(angle_delta_deg(350.0f, 10.0f), 20.0f, kEps);
    NF_CHECK_NEAR(angle_delta_deg(10.0f, 350.0f), -20.0f, kEps);
    NF_CHECK_NEAR(angle_delta_deg(0.0f, 180.0f), 180.0f, kEps);
    NF_CHECK_NEAR(angle_delta_deg(0.0f, 190.0f), -170.0f, kEps);
    NF_CHECK_NEAR(angle_delta_deg(720.0f, 720.0f), 0.0f, kEps);
}

// --- the aim -----------------------------------------------------------------

NF_TEST(an_agent_with_no_target_holds_its_aim) {
    CombatAimState state;
    state.yaw_deg = 33.0f;
    state.pitch_deg = -7.0f;
    state.trigger = true; // it was firing

    CombatAimInput in;
    in.has_target = false;
    in.dt = 1.0f / 60.0f;

    const CombatAimOutput out = combat_aim_step(state, quick_config(), in);
    NF_CHECK_NEAR(out.yaw_deg, 33.0f, kEps);
    NF_CHECK_NEAR(out.pitch_deg, -7.0f, kEps);
    NF_CHECK(!out.trigger);
    NF_CHECK(!state.engaged);
    NF_CHECK_NEAR(state.reaction_timer, 0.0f, kEps);
}

NF_TEST(the_aim_turns_at_a_finite_rate) {
    CombatAimConfig config = quick_config();
    config.aim_speed_deg_per_s = 90.0f; // a quarter turn takes one second
    config.reaction_seconds = 10.0f;    // never allowed to fire, for this case

    CombatAimState state; // starts looking down -Z (yaw 0)
    CombatAimInput in = target_at(-10.0f, 0.0f, 0.0f); // straight left = yaw +90
    in.dt = 0.5f;

    const CombatAimOutput out = combat_aim_step(state, config, in);
    // Half a second at 90 deg/s is 45 degrees of the 90.
    NF_CHECK_NEAR(out.yaw_deg, 45.0f, 1e-2f);
    NF_CHECK(!out.aimed);
    NF_CHECK(out.error_deg > 40.0f);
}

NF_TEST(the_reaction_gates_the_trigger_not_the_aim) {
    CombatAimConfig config = quick_config();
    config.reaction_seconds = 0.5f;

    CombatAimState state;
    CombatAimInput in = target_at(0.0f, 0.0f, -10.0f); // already in front
    in.dt = 0.1f;

    // Aimed immediately: the agent is looking straight at it.
    CombatAimOutput out = combat_aim_step(state, config, in);
    NF_CHECK(out.aimed);
    NF_CHECK(!out.trigger); // but it has not finished noticing

    for (int i = 0; i < 5; ++i) {
        out = combat_aim_step(state, config, in); // 0.6 s total
    }
    NF_CHECK(out.aimed);
    NF_CHECK(out.trigger);
}

NF_TEST(burst_discipline_fires_then_rests) {
    CombatAimConfig config = quick_config();
    config.burst_on_seconds = 0.5f;
    config.burst_off_seconds = 0.5f;

    CombatAimState state;
    CombatAimInput in = target_at(0.0f, 0.0f, -10.0f);
    in.dt = 0.1f;

    u32 on_frames = 0;
    u32 off_frames = 0;
    for (int i = 0; i < 40; ++i) { // four seconds
        const CombatAimOutput out = combat_aim_step(state, config, in);
        if (out.trigger) {
            ++on_frames;
        } else {
            ++off_frames;
        }
    }
    // Roughly half the frames fire, and BOTH phases happen: a stream or a
    // silence would each fail one of these.
    NF_CHECK(on_frames > 10u);
    NF_CHECK(off_frames > 10u);
    NF_CHECK(on_frames + off_frames == 40u);
}

NF_TEST(a_target_that_cannot_be_seen_is_not_shot) {
    CombatAimConfig config = quick_config();
    CombatAimState state;
    CombatAimInput in = target_at(0.0f, 0.0f, -10.0f);
    in.visible = false;
    in.dt = 0.1f;

    for (int i = 0; i < 20; ++i) {
        const CombatAimOutput out = combat_aim_step(state, config, in);
        NF_CHECK(!out.trigger);
        NF_CHECK(out.aimed); // it still tracks; it just does not shoot
    }
}

NF_TEST(the_pitch_is_clamped_to_the_agents_range) {
    CombatAimConfig config = quick_config();
    config.pitch_min = -20.0f;
    config.pitch_max = 20.0f;
    config.reaction_seconds = 10.0f;

    CombatAimState state;
    CombatAimInput in = target_at(0.0f, 1000.0f, -1.0f); // straight up
    in.dt = 1.0f;

    const CombatAimOutput out = combat_aim_step(state, config, in);
    NF_CHECK(out.pitch_deg <= 20.0f + kEps);
    NF_CHECK_NEAR(out.pitch_deg, 20.0f, 1e-2f);
}

NF_TEST(a_paused_frame_does_not_aim) {
    CombatAimState state;
    state.yaw_deg = 12.0f;
    CombatAimInput in = target_at(0.0f, 0.0f, -10.0f);
    in.dt = 0.0f;

    const CombatAimOutput out = combat_aim_step(state, quick_config(), in);
    NF_CHECK_NEAR(out.yaw_deg, 12.0f, kEps);
    NF_CHECK(!out.trigger);
    NF_CHECK(!state.engaged);
}

NF_TEST(a_target_behind_turns_the_short_way_round) {
    CombatAimConfig config = quick_config();
    config.aim_speed_deg_per_s = 180.0f;
    config.reaction_seconds = 10.0f;

    CombatAimState state;
    state.yaw_deg = -170.0f;                    // nearly facing +Z
    CombatAimInput in = target_at(0.0f, 0.0f, -10.0f); // wants yaw 0
    in.dt = 0.1f;                               // 18 degrees

    const CombatAimOutput out = combat_aim_step(state, config, in);
    // -170 + 18 = -152, not -188: the short way is through the wrap.
    NF_CHECK_NEAR(out.yaw_deg, -152.0f, 1e-2f);
}

// --- the scene line ----------------------------------------------------------

NF_TEST(a_combat_agent_line_round_trips) {
    scene::Scene source("Fight");
    const ecs::Entity enemy = source.world().create_entity();
    source.world().add<scene::NameComponent>(enemy, scene::NameComponent{"Bandit"});
    source.world().add<scene::Transform>(enemy, scene::Transform{});

    CombatAgentComponent agent;
    agent.target = "Player";
    agent.eye_height = 1.7f;
    agent.target_height = 1.2f;
    agent.config.reaction_seconds = 0.6f;
    agent.config.aim_speed_deg_per_s = 90.0f;
    agent.config.fire_cone_deg = 4.5f;
    agent.config.max_range = 35.0f;
    agent.config.burst_on_seconds = 0.3f;
    agent.config.burst_off_seconds = 1.1f;
    agent.retaliate = true;
    agent.suppression.level = 0.5f; // session state, must not survive load
    source.world().add<CombatAgentComponent>(enemy, agent);

    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("  CombatAgent:") != std::string::npos);
    NF_CHECK(text.find("retaliate=true") != std::string::npos);
    NF_CHECK(text.find("target=Player") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.scene) {
        return;
    }
    ecs::Entity found = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        if (loaded.scene->world().get<CombatAgentComponent>(e) != nullptr) {
            found = e;
        }
    }
    NF_CHECK(found.valid());
    const auto* back = loaded.scene->world().get<CombatAgentComponent>(found);
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK(back->target == "Player");
        NF_CHECK_NEAR(back->eye_height, 1.7f, 1e-4f);
        NF_CHECK_NEAR(back->target_height, 1.2f, 1e-4f);
        NF_CHECK_NEAR(back->config.reaction_seconds, 0.6f, 1e-4f);
        NF_CHECK_NEAR(back->config.aim_speed_deg_per_s, 90.0f, 1e-4f);
        NF_CHECK_NEAR(back->config.fire_cone_deg, 4.5f, 1e-4f);
        NF_CHECK_NEAR(back->config.max_range, 35.0f, 1e-4f);
        NF_CHECK_NEAR(back->config.burst_on_seconds, 0.3f, 1e-4f);
        NF_CHECK_NEAR(back->config.burst_off_seconds, 1.1f, 1e-4f);
        NF_CHECK(back->retaliate);
        // The aim and suppression are session state: a loaded scene starts
        // with an agent that is not mid-turn, mid-burst or pinned.
        NF_CHECK(!back->state.engaged);
        NF_CHECK(!back->state.trigger);
        NF_CHECK_NEAR(back->suppression.level, 0.0f, 1e-5f);
    }
}

// --- the whole fight ---------------------------------------------------------

NF_TEST(an_agent_shoots_the_player_it_can_see) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_combat_ai";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        assets::AssetRegistry registry;
        assets::AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto world_scene = std::make_unique<scene::Scene>("Arena");

        // The player stands at the origin, at chest height so the collider and
        // the aim point line up. The camera follows it, which is what makes it
        // "the player".
        const ecs::Entity player = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(player, scene::NameComponent{"Player"});
        auto& pt = world_scene->world().add<scene::Transform>(player);
        pt.local_x = 0.0f;
        pt.local_y = 1.5f;
        pt.local_z = 0.0f;
        world_scene->world().add<physics::ColliderComponent>(
            player, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.4f, 0.4f, 0.4f})});
        HealthComponent hp;
        hp.max_health = 100.0f;
        hp.health = 100.0f;
        world_scene->world().add<HealthComponent>(player, hp);

        // The enemy, ten metres down -Z, turned to face +Z (rot_y 180).
        const ecs::Entity enemy = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(enemy, scene::NameComponent{"Bandit"});
        auto& et = world_scene->world().add<scene::Transform>(enemy);
        et.local_x = 0.0f;
        et.local_y = 1.5f;
        et.local_z = -10.0f;
        et.rot_y = 180.0f;

        WeaponProfile gun;
        gun.mode = FireMode::Auto;
        gun.rounds_per_minute = 600.0f;
        gun.magazine = 30u;
        gun.reserve = 300u;
        gun.damage = 10.0f;
        gun.range = 60.0f;
        gun.spread_deg = 0.0f;
        gun.spread_max_deg = 1.0f;
        gun.spread_bloom_deg = 0.0f;
        gun.recoil_pitch = 0.0f;
        gun.recoil_yaw = 0.0f;
        gun.shake = 0.0f;
        world_scene->world().add<WeaponComponent>(enemy, WeaponComponent{gun});

        CombatAgentComponent agent;
        agent.target = "Player";
        agent.eye_height = 0.0f;
        agent.target_height = 0.0f;
        agent.config.reaction_seconds = 0.2f;
        agent.config.aim_speed_deg_per_s = 720.0f;
        agent.config.fire_cone_deg = 5.0f;
        agent.config.max_range = 60.0f;
        agent.config.burst_on_seconds = 1.0f;
        agent.config.burst_off_seconds = 0.2f;
        world_scene->world().add<CombatAgentComponent>(enemy, agent);

        // A camera that follows the player, so player_entity() resolves.
        const ecs::Entity camera = world_scene->world().create_entity();
        world_scene->world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
        world_scene->world().add<scene::Transform>(camera, scene::Transform{});
        CameraComponent cam;
        cam.is_active = true;
        cam.follow_target = "Player";
        cam.pivot_y = 0.0f;
        cam.distance = 4.0f;
        cam.mouse_look = false;
        world_scene->world().add<CameraComponent>(camera, cam);

        rt.adopt_scene(std::move(world_scene), "content://Scenes/Arena.nfscene");

        // Two seconds of frames: reaction (0.2 s) + one second of trigger at
        // 600 rpm is ten shots of ten damage. A hundred hit points is exactly
        // enough to survive one burst, so the assertion is "it took damage",
        // not "it died".
        for (int frame = 0; frame < 120; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        const HealthComponent* hurt = rt.health_of(player);
        NF_CHECK(hurt != nullptr);
        if (hurt != nullptr) {
            NF_CHECK(hurt->health < 100.0f);
            NF_CHECK(hurt->health >= 0.0f);
        }
        // The shot came from the agent, and the engine recorded the player as
        // the thing that was hit.
        NF_CHECK(rt.last_weapon_hit().hit);
        NF_CHECK(rt.last_weapon_hit().name == "Player");

        // Cut the target and the shooting stops: an agent with nothing to aim
        // at must not keep firing at where the player used to be.
        float before = hurt != nullptr ? hurt->health : 0.0f;
        CombatAgentComponent* live = rt.edit_scene()->world().get<CombatAgentComponent>(enemy);
        NF_CHECK(live != nullptr);
        if (live != nullptr) {
            live->target.clear();
        }
        for (int frame = 0; frame < 120; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        const HealthComponent* after = rt.health_of(player);
        NF_CHECK(after != nullptr);
        if (after != nullptr) {
            NF_CHECK_NEAR(after->health, before, 1e-3f);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
