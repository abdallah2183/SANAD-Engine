// Tests/RuntimeTests/test_state_machine_runtime.cpp — Phase 62: the machine half
// of the AI trio.
//
// `ai::StateMachine` has been unit-tested since Phase 22 (16 cases in AITests)
// and reachable from nothing: no scene could declare a state or an edge. Utility
// scoring answers "which consideration pulls hardest right now"; a machine
// answers "what am I doing", where the SEQUENCE is the behaviour — patrol,
// alert, chase, search, return.
//
// The cases cover the bridge:
//
//   1. the scene lines: `States:`, `State:` and `Transition:` round-trip, and a
//      value that cannot be read is refused by name;
//   2. the validator, which is where the cross-references are checked: an edge to
//      a state nothing declares is an ERROR (the machine would drop it and the
//      NPC would never leave), an unreachable state and a bad `initial` are
//      reported;
//   3. the frame: the machine runs, follows its guards in authored order, holds a
//      transition until its authored rate, and vetoes the trigger while a
//      `hold_fire` state is current;
//   4. a script can read which state is running.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Runtime/SceneValidator.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;
using namespace nf::runtime;

namespace {

StateSpec state(std::string name, bool hold_fire = false) {
    StateSpec s;
    s.name = std::move(name);
    s.hold_fire = hold_fire;
    return s;
}

TransitionSpec edge(std::string from, std::string to, BrainInput when = BrainInput::Aware,
                    f32 at = 0.5f, bool below = false) {
    TransitionSpec t;
    t.from = std::move(from);
    t.to = std::move(to);
    t.when = when;
    t.at = at;
    t.below = below;
    return t;
}

WeaponProfile straight_gun() {
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
    return gun;
}

PerceptionComponent sharp_eyes() {
    PerceptionComponent pc;
    pc.sight_range = 40.0f;
    pc.sight_half_fov_deg = 90.0f;
    pc.sight_gain = 8.0f;
    pc.sight_linger = 1.0f;
    pc.hearing_range = 0.0f;
    pc.touch_range = 0.0f;
    pc.pain = false;
    pc.eye_height = 0.0f;
    return pc;
}

void add_follow_camera(scene::Scene& scene, const char* target, f32 distance) {
    const ecs::Entity camera = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
    scene.world().add<scene::Transform>(camera, scene::Transform{});
    CameraComponent cam;
    cam.is_active = true;
    cam.follow_target = target;
    cam.pivot_y = 0.0f;
    cam.distance = distance;
    cam.mouse_look = false;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    scene.world().add<CameraComponent>(camera, cam);
}

ecs::Entity add_entity(scene::Scene& scene, const char* name, f32 x, f32 y, f32 z,
                       bool collider = true) {
    const ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{name});
    auto& t = scene.world().add<scene::Transform>(e);
    t.local_x = x;
    t.local_y = y;
    t.local_z = z;
    if (collider) {
        scene.world().add<physics::ColliderComponent>(
            e, physics::ColliderComponent{physics::Shape::make_box(Vec3{0.4f, 0.4f, 0.4f})});
    }
    return e;
}

void add_health(scene::Scene& scene, ecs::Entity e, f32 hp) {
    HealthComponent health;
    health.max_health = hp;
    health.health = hp;
    scene.world().add<HealthComponent>(e, health);
}

void add_bare_camera(scene::Scene& scene) {
    const ecs::Entity camera = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
    scene.world().add<scene::Transform>(camera, scene::Transform{});
    CameraComponent cam;
    cam.is_active = true;
    scene.world().add<CameraComponent>(camera, cam);
}

struct NoInput final : gameplay::IInputSource {
    [[nodiscard]] bool action_pressed(std::string_view) const override { return false; }
    [[nodiscard]] f32 action_axis(std::string_view) const override { return 0.0f; }
};

} // namespace

// --- the scene lines ---------------------------------------------------------

NF_TEST(state_lines_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_sm_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    scene::Scene scene("PatrolScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    StateMachineComponent machine;
    machine.enabled = false;
    machine.rate = 0.5f;
    machine.initial = "alert";
    machine.states.push_back(state("patrol"));
    machine.states.push_back(state("alert", true));
    machine.transitions.push_back(edge("patrol", "alert", BrainInput::Aware, 0.5f, false));
    machine.transitions.push_back(edge("alert", "patrol", BrainInput::Aware, 0.5f, true));
    machine.transitions.push_back(edge("patrol", "alert", BrainInput::Health, 0.25f, true));
    world.add<StateMachineComponent>(e, machine);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/patrol.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/patrol.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const auto* loaded = result.scene->world().get<StateMachineComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(!loaded->enabled);
        NF_CHECK_NEAR(loaded->rate, 0.5f, 1e-5f);
        NF_CHECK(loaded->initial == "alert");
        NF_CHECK_EQ(loaded->states.size(), static_cast<size_t>(2));
        NF_CHECK_EQ(loaded->transitions.size(), static_cast<size_t>(3));
        if (loaded->states.size() == 2) {
            NF_CHECK(loaded->states[0].name == "patrol");
            NF_CHECK(!loaded->states[0].hold_fire);
            NF_CHECK(loaded->states[1].name == "alert");
            NF_CHECK(loaded->states[1].hold_fire);
        }
        if (loaded->transitions.size() == 3) {
            NF_CHECK(loaded->transitions[0].from == "patrol");
            NF_CHECK(loaded->transitions[0].to == "alert");
            NF_CHECK(loaded->transitions[0].when == BrainInput::Aware);
            NF_CHECK(!loaded->transitions[0].below);
            NF_CHECK(loaded->transitions[1].below);
            NF_CHECK(loaded->transitions[2].when == BrainInput::Health);
            NF_CHECK_NEAR(loaded->transitions[2].at, 0.25f, 1e-5f);
        }
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(a_machine_line_that_cannot_be_read_is_refused_by_name) {
    const auto load = [](const std::string& body) {
        const std::string text =
            "# SANAD Scene v1\nversion: 1\nname: BadMachine\nentity_count: 1\n"
            "---\nentity: 1:0\n  Name: Guard\n"
            "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
            "parent(4294967295:0)\n" + body;
        return load_scene_from_text(text);
    };

    auto no_name = load("  State: hold_fire=true\n");
    NF_CHECK(no_name.success);
    if (no_name.scene) {
        NF_CHECK(no_name.scene->world().get<StateMachineComponent>(
                      no_name.scene->world().all_entities()[0]) == nullptr);
    }
    bool named = false;
    for (const std::string& w : no_name.warnings) {
        if (w.find("State line") != std::string::npos && w.find("name") != std::string::npos) {
            named = true;
        }
    }
    NF_CHECK(named);

    auto no_from = load("  Transition: to=alert when=aware\n");
    bool named_from = false;
    for (const std::string& w : no_from.warnings) {
        if (w.find("Transition line") != std::string::npos &&
            w.find("from") != std::string::npos) {
            named_from = true;
        }
    }
    NF_CHECK(named_from);

    auto bad_when = load("  Transition: from=a to=b when=smell\n");
    bool named_when = false;
    for (const std::string& w : bad_when.warnings) {
        if (w.find("Transition line") != std::string::npos &&
            w.find("when") != std::string::npos) {
            named_when = true;
        }
    }
    NF_CHECK(named_when);

    // The good case: the lines alone build the machine, and `States:` after its
    // states updates the clock without dropping them.
    auto ok = load("  State: name=patrol\n  State: name=alert hold_fire=true\n"
                   "  Transition: from=patrol to=alert when=aware\n"
                   "  States: rate=0.5 initial=patrol\n");
    NF_CHECK(ok.success);
    NF_CHECK(ok.scene != nullptr);
    if (ok.scene) {
        const auto* machine = ok.scene->world().get<StateMachineComponent>(
            ok.scene->world().all_entities()[0]);
        NF_CHECK(machine != nullptr);
        if (machine != nullptr) {
            NF_CHECK_NEAR(machine->rate, 0.5f, 1e-5f);
            NF_CHECK(machine->initial == "patrol");
            NF_CHECK_EQ(machine->states.size(), static_cast<size_t>(2));
            NF_CHECK_EQ(machine->transitions.size(), static_cast<size_t>(1));
        }
    }
}

// --- the validator -----------------------------------------------------------

NF_TEST(an_edge_to_a_state_nothing_declares_is_an_error) {
    scene::Scene scene("Dangling");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_entity(scene, "Guard", 0.0f, 0.0f, 0.0f, false);
    StateMachineComponent machine;
    machine.initial = "patrol";
    machine.states.push_back(state("patrol"));
    machine.transitions.push_back(edge("patrol", "chase"));
    world.add<StateMachineComponent>(e, machine);

    const ValidationReport report = validate_scene(scene, "content://Scenes/d.nfscene");
    NF_CHECK(!report.ok()); // an error, not a warning: the machine would drop the edge
    bool named = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.severity == ValidationSeverity::Error &&
            issue.message.find("chase") != std::string::npos) {
            named = true;
        }
    }
    NF_CHECK(named);
}

NF_TEST(an_unreachable_state_and_a_bad_initial_are_reported) {
    scene::Scene scene("Orphan");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_entity(scene, "Guard", 0.0f, 0.0f, 0.0f, false);
    StateMachineComponent machine;
    machine.initial = "nowhere"; // not a state
    machine.states.push_back(state("patrol"));
    machine.states.push_back(state("orphan"));
    machine.transitions.push_back(edge("patrol", "patrol"));
    world.add<StateMachineComponent>(e, machine);

    const ValidationReport report = validate_scene(scene, "content://Scenes/o.nfscene");
    bool bad_initial = false;
    bool unreachable = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("nowhere") != std::string::npos) bad_initial = true;
        if (issue.message.find("orphan") != std::string::npos &&
            issue.message.find("never run") != std::string::npos) {
            unreachable = true;
        }
    }
    NF_CHECK(bad_initial);
    NF_CHECK(unreachable);

    // And a machine that is wired end to end is silent.
    scene::Scene fine("Fine");
    add_bare_camera(fine);
    const ecs::Entity f = add_entity(fine, "Guard", 0.0f, 0.0f, 0.0f, false);
    StateMachineComponent good;
    good.initial = "patrol";
    good.states.push_back(state("patrol"));
    good.states.push_back(state("alert", true));
    good.transitions.push_back(edge("patrol", "alert"));
    good.transitions.push_back(edge("alert", "patrol", BrainInput::Aware, 0.5f, true));
    fine.world().add<StateMachineComponent>(f, good);
    NF_CHECK(validate_scene(fine, "content://Scenes/f.nfscene").ok());
}

// --- the frame ---------------------------------------------------------------

NF_TEST(a_machine_follows_its_guards_and_returns) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_sm_patrol";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Patrol");
        // The target starts behind the guard, so the guard has nothing to notice
        // and stays on patrol.
        const ecs::Entity target = add_entity(*game, "Target", 0.0f, 0.0f, 12.0f);
        add_health(*game, target, 100.0f);

        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        game->world().add<PerceptionComponent>(guard, sharp_eyes());

        StateMachineComponent machine;
        machine.initial = "patrol";
        machine.states.push_back(state("patrol", true)); // watching, not shooting
        machine.states.push_back(state("alert"));
        machine.transitions.push_back(edge("patrol", "alert"));
        machine.transitions.push_back(edge("alert", "patrol", BrainInput::Aware, 0.5f, true));
        game->world().add<StateMachineComponent>(guard, machine);

        rt.adopt_scene(std::move(game), "content://Scenes/Patrol.nfscene");
        NF_CHECK_EQ(rt.state_machine_count(), static_cast<size_t>(1));

        rt.update(1.0f / 60.0f);
        std::string current;
        NF_CHECK(rt.entity_state(guard, current));
        NF_CHECK(current == "patrol");
        NF_CHECK(rt.brain_holds_fire(guard)); // the state vetoes the trigger
        const u32 fired_before = rt.entity_transition_count(guard);

        // Turn it round and the guard notices: patrol -> alert.
        rt.edit_scene()->world().get<scene::Transform>(guard)->rot_y = 180.0f;
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.entity_aware_of(guard, target));
        NF_CHECK(rt.entity_state(guard, current));
        NF_CHECK(current == "alert");
        NF_CHECK(!rt.brain_holds_fire(guard));
        NF_CHECK(rt.entity_transition_count(guard) > fired_before);

        // Turn away again: awareness lingers, then the guard goes back to patrol.
        rt.edit_scene()->world().get<scene::Transform>(guard)->rot_y = 0.0f;
        for (int frame = 0; frame < 180; ++frame) { // 3 s, past the 1 s linger
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(!rt.entity_aware_of(guard, target));
        NF_CHECK(rt.entity_state(guard, current));
        NF_CHECK(current == "patrol");
        NF_CHECK(rt.brain_holds_fire(guard));
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(the_first_guard_that_holds_decides_the_edge) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_sm_order";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Order");
        const ecs::Entity target = add_entity(*game, "Target", 0.0f, 0.0f, -10.0f);
        add_health(*game, target, 100.0f);

        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        game->world().add<PerceptionComponent>(guard, sharp_eyes());

        StateMachineComponent machine;
        machine.initial = "patrol";
        machine.states.push_back(state("patrol"));
        machine.states.push_back(state("first"));  // what the earlier edge goes to
        machine.states.push_back(state("second")); // what the later edge goes to
        // Both guards hold at the same instant; the module documents authoring
        // order as the tie-break, so the machine must take the earlier one.
        machine.transitions.push_back(edge("patrol", "first"));
        machine.transitions.push_back(edge("patrol", "second"));
        machine.transitions.push_back(edge("first", "patrol", BrainInput::Aware, 0.5f, true));
        machine.transitions.push_back(edge("second", "patrol", BrainInput::Aware, 0.5f, true));
        game->world().add<StateMachineComponent>(guard, machine);

        rt.adopt_scene(std::move(game), "content://Scenes/Order.nfscene");
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        std::string current;
        NF_CHECK(rt.entity_state(guard, current));
        NF_CHECK(current == "first");
        NF_CHECK_EQ(rt.entity_transition_count(guard), 1u);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_machine_waits_for_its_authored_rate) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_sm_rate";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Slow");
        const ecs::Entity target = add_entity(*game, "Target", 0.0f, 0.0f, -10.0f);
        add_health(*game, target, 100.0f);
        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        game->world().add<PerceptionComponent>(guard, sharp_eyes());

        StateMachineComponent machine;
        machine.initial = "patrol";
        machine.rate = 1.0f; // one tick per second
        machine.states.push_back(state("patrol"));
        machine.states.push_back(state("alert"));
        machine.transitions.push_back(edge("patrol", "alert"));
        machine.transitions.push_back(edge("alert", "patrol", BrainInput::Aware, 0.5f, true));
        game->world().add<StateMachineComponent>(guard, machine);

        rt.adopt_scene(std::move(game), "content://Scenes/Slow.nfscene");
        rt.update(1.0f / 60.0f); // the first tick always runs

        // The guard notices the target within a fifth of a second, but the
        // machine does not tick again until its second is up.
        for (int frame = 0; frame < 50; ++frame) { // 0.83 s
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.entity_aware_of(guard, target));
        std::string current;
        NF_CHECK(rt.entity_state(guard, current));
        NF_CHECK(current == "patrol");

        for (int frame = 0; frame < 20; ++frame) { // past the second
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.entity_state(guard, current));
        NF_CHECK(current == "alert");
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_hold_fire_state_stops_the_trigger) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_sm_hold";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    const auto run = [&](bool hold_fire, f32* out_health) {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Hold");
        const ecs::Entity player = add_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
        add_health(*game, player, 100.0f);
        add_follow_camera(*game, "Player", 4.0f);

        const ecs::Entity bandit = add_entity(*game, "Bandit", 0.0f, 1.5f, -10.0f);
        game->world().get<scene::Transform>(bandit)->rot_y = 180.0f;
        add_health(*game, bandit, 100.0f);
        game->world().add<WeaponComponent>(bandit, WeaponComponent{straight_gun()});
        CombatAgentComponent agent;
        agent.target = "Player";
        agent.eye_height = 0.0f;
        agent.target_height = 0.0f;
        agent.config.reaction_seconds = 0.2f;
        agent.config.aim_speed_deg_per_s = 720.0f;
        agent.config.fire_cone_deg = 5.0f;
        agent.config.max_range = 60.0f;
        agent.config.burst_on_seconds = 5.0f;
        agent.config.burst_off_seconds = 0.0f;
        game->world().add<CombatAgentComponent>(bandit, agent);
        game->world().add<PerceptionComponent>(bandit, sharp_eyes());

        StateMachineComponent machine;
        machine.initial = "watch";
        machine.states.push_back(state("watch", hold_fire));
        machine.states.push_back(state("fire"));
        machine.transitions.push_back(edge("watch", "fire", BrainInput::Health, 0.0f, true));
        machine.transitions.push_back(edge("fire", "watch", BrainInput::Health, 0.0f, true));
        game->world().add<StateMachineComponent>(bandit, machine);

        rt.adopt_scene(std::move(game), "content://Scenes/Hold.nfscene");
        for (int frame = 0; frame < 180; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        if (out_health != nullptr) {
            const HealthComponent* h = rt.health_of(player);
            *out_health = h != nullptr ? h->health : -1.0f;
        }
    };

    f32 fired_health = 0.0f;
    run(false, &fired_health);
    NF_CHECK(fired_health < 100.0f); // the control: this scene shoots

    f32 held_health = 0.0f;
    run(true, &held_health);
    NF_CHECK_NEAR(held_health, 100.0f, 1e-3f);

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_script_reads_its_state) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_sm_script";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    // 3 when the alert state is running, 1 otherwise (or when there is none).
    const std::string script =
        "function update(dt)\n"
        "  local id, gen = nf.self()\n"
        "  local x, y, z = nf.entity_pos(id, gen)\n"
        "  local name = nf.state()\n"
        "  nf.set_entity_pos(id, gen, x, (name == 'alert') and 3.0 or 1.0, z)\n"
        "end\n";
    NF_CHECK(vfs.write_text("content://Scripts/watch.lua", script).ok);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);
        NoInput no_input;
        rt.set_input_source(&no_input);

        auto game = std::make_unique<scene::Scene>("Watch");
        const ecs::Entity target = add_entity(*game, "Target", 0.0f, 0.0f, -10.0f);
        add_health(*game, target, 100.0f);
        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        game->world().add<PerceptionComponent>(guard, sharp_eyes());

        StateMachineComponent machine;
        machine.initial = "patrol";
        machine.states.push_back(state("patrol"));
        machine.states.push_back(state("alert"));
        machine.transitions.push_back(edge("patrol", "alert"));
        machine.transitions.push_back(edge("alert", "patrol", BrainInput::Aware, 0.5f, true));
        game->world().add<StateMachineComponent>(guard, machine);
        scripting::ScriptComponent script_component;
        script_component.path = "content://Scripts/watch.lua";
        game->world().add<scripting::ScriptComponent>(guard, script_component);

        rt.adopt_scene(std::move(game), "content://Scenes/Watch.nfscene");
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        const scene::Scene* live = rt.scene();
        NF_CHECK(live != nullptr);
        std::string current;
        NF_CHECK(rt.entity_state(guard, current));
        NF_CHECK(current == "alert");
        if (live != nullptr) {
            const auto* t = live->world().get<scene::Transform>(guard);
            NF_CHECK(t != nullptr);
            if (t != nullptr) {
                NF_CHECK_NEAR(t->local_y, 3.0f, 1e-3f);
            }
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
