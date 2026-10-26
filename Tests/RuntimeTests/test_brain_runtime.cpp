// Tests/RuntimeTests/test_brain_runtime.cpp — Phase 61: the decision layer.
//
// Perception answers "what do I know"; the brain answers "what do I want to do
// about it". `ai::UtilitySystem` and its response curves have been unit-tested
// since Phase 22 (21 cases in AITests) and were reachable from nothing: no scene
// could name a consideration, and no frame turned the world into the numbers a
// consideration is a function of.
//
// These cases are about the BRIDGE, like the perception ones:
//
//   1. the scene lines: `Brain:` and `BrainAction:` round-trip, an impossible
//      value is refused by name, and an action line creates the brain it needs;
//   2. the validator: a brain with nothing to consider, duplicate names and an
//      all-zero weight list are the mistakes only the whole list can show;
//   3. the frame: the inputs are the world's numbers, a hurt NPC decides to
//      flee, an NPC that notices its target decides to engage, and the decision
//      holds until the authored period elapses;
//   4. the effect: a winning action with `hold_fire` stops the trigger while the
//      aim keeps tracking, and a script can read the decision by name.
//
// The pure half needs no device. The integration half skips without Vulkan.

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

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;
using namespace nf::runtime;

namespace {

BrainAction action(std::string name, BrainInput input, f32 weight = 1.0f, f32 slope = 1.0f,
                   f32 output_offset = 0.0f) {
    BrainAction a;
    a.name = std::move(name);
    a.input = input;
    a.weight = weight;
    a.slope = slope;
    a.output_offset = output_offset;
    return a;
}

/// "I want to <name> more the worse my hit points are" — the shape a flee
/// consideration has, and the one the tests lean on.
BrainAction flee_when_hurt(std::string name) {
    return action(std::move(name), BrainInput::Health, 1.0f, -1.0f, 1.0f);
}

/// "I want to <name> when I have noticed someone".
BrainAction engage_when_aware(std::string name) {
    return action(std::move(name), BrainInput::Aware);
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

/// Senses that see straight ahead, far, and fast — the brain cases are about
/// deciding, not about noticing.
PerceptionComponent sharp_eyes() {
    PerceptionComponent pc;
    pc.sight_range = 40.0f;
    pc.sight_half_fov_deg = 90.0f;
    pc.sight_gain = 8.0f;
    pc.sight_linger = 5.0f;
    pc.hearing_range = 0.0f;
    pc.touch_range = 0.0f;
    pc.pain = false;
    pc.eye_height = 0.0f;
    return pc;
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

NF_TEST(brain_lines_round_trip_every_consideration) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_brain_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    scene::Scene scene("BrainScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    BrainComponent brain;
    brain.enabled = false;
    brain.rate = 1.25f;
    brain.actions.push_back(action("engage", BrainInput::Aware, 2.5f, 1.0f, 0.0f));
    BrainAction hold = action("hold", BrainInput::Threat, 0.5f, -2.0f, 1.0f);
    hold.exponent = 2.0f;
    hold.input_offset = 0.3f;
    hold.hold_fire = true;
    brain.actions.push_back(hold);
    world.add<BrainComponent>(e, brain);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/brain.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/brain.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const auto* loaded = result.scene->world().get<BrainComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(!loaded->enabled);
        NF_CHECK_NEAR(loaded->rate, 1.25f, 1e-5f);
        NF_CHECK_EQ(loaded->actions.size(), static_cast<size_t>(2));
        if (loaded->actions.size() == 2) {
            NF_CHECK(loaded->actions[0].name == "engage");
            NF_CHECK(loaded->actions[0].input == BrainInput::Aware);
            NF_CHECK_NEAR(loaded->actions[0].weight, 2.5f, 1e-5f);
            NF_CHECK(loaded->actions[1].name == "hold");
            NF_CHECK(loaded->actions[1].input == BrainInput::Threat);
            NF_CHECK_NEAR(loaded->actions[1].slope, -2.0f, 1e-5f);
            NF_CHECK_NEAR(loaded->actions[1].exponent, 2.0f, 1e-5f);
            NF_CHECK_NEAR(loaded->actions[1].input_offset, 0.3f, 1e-5f);
            NF_CHECK(loaded->actions[1].hold_fire);
        }
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(brain_absent_from_a_scene_stays_absent_when_saved) {
    scene::Scene scene("PlainScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    const std::string text = serialize_scene_to_text(scene);
    NF_CHECK(text.find("Brain:") == std::string::npos);
    NF_CHECK(text.find("BrainAction:") == std::string::npos);
}

NF_TEST(a_consideration_that_cannot_be_read_is_refused_by_name) {
    const auto load = [](const std::string& body) {
        const std::string text =
            "# SANAD Scene v1\nversion: 1\nname: BadBrain\nentity_count: 1\n"
            "---\nentity: 1:0\n  Name: Guard\n"
            "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
            "parent(4294967295:0)\n" + body;
        return load_scene_from_text(text);
    };

    // No name: the decision could never be reported, so the line is refused.
    auto nameless = load("  BrainAction: input=aware\n");
    NF_CHECK(nameless.success);
    if (nameless.scene) {
        NF_CHECK(nameless.scene->world().get<BrainComponent>(
                      nameless.scene->world().all_entities()[0]) == nullptr);
    }
    bool named_name = false;
    for (const std::string& w : nameless.warnings) {
        if (w.find("BrainAction") != std::string::npos && w.find("name") != std::string::npos) {
            named_name = true;
        }
    }
    NF_CHECK(named_name);

    // An input the frame has no number for is a typo, not a strategy.
    auto bad_input = load("  BrainAction: name=flank input=smell\n");
    NF_CHECK(bad_input.success);
    bool named_input = false;
    for (const std::string& w : bad_input.warnings) {
        if (w.find("BrainAction") != std::string::npos && w.find("input") != std::string::npos) {
            named_input = true;
        }
    }
    NF_CHECK(named_input);
    if (bad_input.scene) {
        NF_CHECK(bad_input.scene->world().get<BrainComponent>(
                      bad_input.scene->world().all_entities()[0]) == nullptr);
    }

    // An unknown curve preset names itself too.
    auto bad_curve = load("  BrainAction: name=x input=aware curve=wobbly\n");
    bool named_curve = false;
    for (const std::string& w : bad_curve.warnings) {
        if (w.find("BrainAction") != std::string::npos && w.find("curve") != std::string::npos) {
            named_curve = true;
        }
    }
    NF_CHECK(named_curve);

    // The good case: an action line alone creates the brain it needs, with the
    // component's own default rate, and the curve preset is the shorthand the
    // explicit fields then override.
    auto ok = load("  BrainAction: name=flee input=health curve=inverse slope=-3\n");
    NF_CHECK(ok.success);
    NF_CHECK(ok.scene != nullptr);
    if (ok.scene) {
        const auto* brain = ok.scene->world().get<BrainComponent>(
            ok.scene->world().all_entities()[0]);
        NF_CHECK(brain != nullptr);
        if (brain != nullptr) {
            NF_CHECK_NEAR(brain->rate, 0.25f, 1e-6f);
            NF_CHECK_EQ(brain->actions.size(), static_cast<size_t>(1));
            if (!brain->actions.empty()) {
                NF_CHECK(brain->actions[0].name == "flee");
                NF_CHECK(brain->actions[0].input == BrainInput::Health);
                // The preset set output_offset to 1, then `slope=-3` overrode the
                // slope the preset wrote.
                NF_CHECK_NEAR(brain->actions[0].slope, -3.0f, 1e-5f);
                NF_CHECK_NEAR(brain->actions[0].output_offset, 1.0f, 1e-5f);
            }
        }
    }

    // A `Brain:` after its actions updates the clock without dropping them.
    auto order = load("  BrainAction: name=engage input=aware\n  Brain: rate=2 enabled=false\n");
    NF_CHECK(order.scene != nullptr);
    if (order.scene) {
        const auto* brain = order.scene->world().get<BrainComponent>(
            order.scene->world().all_entities()[0]);
        NF_CHECK(brain != nullptr);
        if (brain != nullptr) {
            NF_CHECK_NEAR(brain->rate, 2.0f, 1e-5f);
            NF_CHECK(!brain->enabled);
            NF_CHECK_EQ(brain->actions.size(), static_cast<size_t>(1));
        }
    }
}

// --- the validator -----------------------------------------------------------

NF_TEST(a_brain_with_nothing_to_consider_is_warned_about) {
    scene::Scene scene("Empty");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_entity(scene, "Guard", 0.0f, 0.0f, 0.0f, false);
    world.add<BrainComponent>(e, BrainComponent{}); // no actions at all

    const ValidationReport report = validate_scene(scene, "content://Scenes/empty.nfscene");
    bool named = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("never decides") != std::string::npos) {
            named = true;
        }
    }
    NF_CHECK(named);
}

NF_TEST(a_brain_with_duplicate_names_or_dead_weights_is_warned_about) {
    scene::Scene scene("Duplicated");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_entity(scene, "Guard", 0.0f, 0.0f, 0.0f, false);
    BrainComponent brain;
    brain.actions.push_back(action("engage", BrainInput::Aware, 0.0f));
    brain.actions.push_back(action("engage", BrainInput::Health, 0.0f));
    world.add<BrainComponent>(e, brain);

    const ValidationReport report = validate_scene(scene, "http://scenes/d.nfscene");
    bool duplicate = false;
    bool dead = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("both named") != std::string::npos) duplicate = true;
        if (issue.message.find("weight 0") != std::string::npos) dead = true;
    }
    NF_CHECK(duplicate);
    NF_CHECK(dead);

    // And a sane list is silent.
    scene::Scene fine("Fine");
    add_bare_camera(fine);
    const ecs::Entity f = add_entity(fine, "Guard", 0.0f, 0.0f, 0.0f, false);
    BrainComponent good;
    good.actions.push_back(action("engage", BrainInput::Aware));
    good.actions.push_back(flee_when_hurt("flee"));
    fine.world().add<BrainComponent>(f, good);
    NF_CHECK(validate_scene(fine, "http://scenes/f.nfscene").ok());
}

// --- the frame ---------------------------------------------------------------

NF_TEST(the_inputs_are_the_worlds_numbers) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_brain_inputs";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Inputs");
        const ecs::Entity target = add_entity(*game, "Target", 0.0f, 0.0f, -12.0f);
        add_health(*game, target, 100.0f);

        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 200.0f);
        game->world().add<PerceptionComponent>(guard, sharp_eyes());
        game->world().add<WeaponComponent>(guard, WeaponComponent{straight_gun()});
        BrainComponent brain;
        brain.rate = 0.0f; // every frame, so the test does not have to wait
        brain.actions.push_back(engage_when_aware("engage"));
        game->world().add<BrainComponent>(guard, brain);

        rt.adopt_scene(std::move(game), "content://Scenes/Inputs.nfscene");
        // Awareness needs time to fill, and until the guard has noticed the
        // target there IS no target: `distance` is 0 by definition, which is the
        // contract the input is documented with.
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.entity_aware_of(guard, target));

        // The distance is the frame's own arithmetic, not a number anybody typed.
        NF_CHECK_NEAR(rt.brain_input_value(guard, BrainInput::Distance), 12.0f, 0.05f);
        NF_CHECK_NEAR(rt.brain_input_value(guard, BrainInput::Health), 1.0f, 1e-4f);
        NF_CHECK_NEAR(rt.brain_input_value(guard, BrainInput::Ammo), 1.0f, 1e-4f);
        NF_CHECK_NEAR(rt.brain_input_value(guard, BrainInput::Threat), 0.0f, 1e-4f);

        // Hurt it and the health fraction follows; give the magazine a dent and
        // the ammo fraction follows too.
        rt.edit_scene()->world().get<HealthComponent>(guard)->health = 50.0f;
        rt.edit_scene()->world().get<WeaponComponent>(guard)->state.in_magazine = 15u;
        rt.update(1.0f / 60.0f);
        NF_CHECK_NEAR(rt.brain_input_value(guard, BrainInput::Health), 0.25f, 1e-4f);
        NF_CHECK_NEAR(rt.brain_input_value(guard, BrainInput::Ammo), 0.5f, 1e-4f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_hurt_npc_decides_to_flee_and_a_healthy_one_does_not) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_brain_flee";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Morale");
        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        BrainComponent brain;
        brain.rate = 0.0f;
        // A flat zero first: at full health nothing wants to run, and the tie
        // goes to the earlier action.
        brain.actions.push_back(action("hold", BrainInput::Distance, 1.0f, 0.0f, 0.0f));
        brain.actions.push_back(flee_when_hurt("flee"));
        game->world().add<BrainComponent>(guard, brain);

        rt.adopt_scene(std::move(game), "content://Scenes/Morale.nfscene");
        rt.update(1.0f / 60.0f);
        std::string decision;
        f32 score = 0.0f;
        NF_CHECK(rt.brain_decision(guard, decision, score));
        NF_CHECK(decision == "hold");
        NF_CHECK_NEAR(score, 0.0f, 1e-5f);

        // Wounded to 20%: 1 - 0.2 = 0.8 beats a flat zero.
        rt.edit_scene()->world().get<HealthComponent>(guard)->health = 20.0f;
        rt.update(1.0f / 60.0f);
        NF_CHECK(rt.brain_decision(guard, decision, score));
        NF_CHECK(decision == "flee");
        NF_CHECK_NEAR(score, 0.8f, 1e-4f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(an_npc_that_notices_someone_decides_to_engage) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_brain_engage";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Watch");
        // The target starts behind the guard: nothing to see, so the guard has
        // nothing to decide about.
        const ecs::Entity target = add_entity(*game, "Target", 0.0f, 0.0f, 12.0f);
        add_health(*game, target, 100.0f);

        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        game->world().add<PerceptionComponent>(guard, sharp_eyes());
        BrainComponent brain;
        brain.rate = 0.0f;
        // `wait` scores 1 while unaware and 0 once aware; `engage` the opposite.
        brain.actions.push_back(action("wait", BrainInput::Aware, 1.0f, -1.0f, 1.0f));
        brain.actions.push_back(engage_when_aware("engage"));
        game->world().add<BrainComponent>(guard, brain);

        rt.adopt_scene(std::move(game), "content://Scenes/Watch.nfscene");
        rt.update(1.0f / 60.0f);
        std::string decision;
        f32 score = 0.0f;
        NF_CHECK(rt.brain_decision(guard, decision, score));
        NF_CHECK(decision == "wait");
        NF_CHECK(!rt.entity_aware_of(guard, target));

        // Turn the guard round (its Transform's yaw is its facing with no agent).
        rt.edit_scene()->world().get<scene::Transform>(guard)->rot_y = 180.0f;
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.entity_aware_of(guard, target));
        NF_CHECK(rt.brain_decision(guard, decision, score));
        NF_CHECK(decision == "engage");
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(the_decision_holds_until_the_authored_period_elapses) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_brain_rate";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Clock");
        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        BrainComponent brain;
        brain.rate = 1.0f; // one decision per second
        brain.actions.push_back(action("hold", BrainInput::Distance, 1.0f, 0.0f, 0.0f));
        brain.actions.push_back(flee_when_hurt("flee"));
        game->world().add<BrainComponent>(guard, brain);

        rt.adopt_scene(std::move(game), "content://Scenes/Clock.nfscene");
        rt.update(1.0f / 60.0f);
        std::string decision;
        f32 score = 0.0f;
        NF_CHECK(rt.brain_decision(guard, decision, score));
        NF_CHECK(decision == "hold");

        // The world changes, the decision does not — for a second. A brain that
        // re-decided every frame would flicker between two close utilities, and
        // the rate is the cure.
        rt.edit_scene()->world().get<HealthComponent>(guard)->health = 10.0f;
        for (int frame = 0; frame < 50; ++frame) { // 0.83 s
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.brain_decision(guard, decision, score));
        NF_CHECK(decision == "hold");

        for (int frame = 0; frame < 20; ++frame) { // past the second
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.brain_decision(guard, decision, score));
        NF_CHECK(decision == "flee");
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_winning_hold_fire_action_stops_the_trigger) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_brain_hold";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    // The bandit is identical in both halves; the only difference is whether its
    // winning consideration asks for `hold_fire`.
    const auto run = [&](bool hold_fire, f32* out_health, bool* out_holds) {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Hold");
        const ecs::Entity player = add_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
        add_health(*game, player, 100.0f);
        add_follow_camera(*game, "Player", 4.0f);

        const ecs::Entity bandit = add_entity(*game, "Bandit", 0.0f, 1.5f, -10.0f);
        game->world().get<scene::Transform>(bandit)->rot_y = 180.0f; // facing +Z
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

        BrainComponent brain;
        brain.rate = 0.0f;
        // A consideration that always wants to run, so the decision is stable.
        BrainAction steady = action("steady", BrainInput::Health, 1.0f, 0.0f, 1.0f);
        steady.hold_fire = hold_fire;
        brain.actions.push_back(steady);
        game->world().add<BrainComponent>(bandit, brain);

        rt.adopt_scene(std::move(game), "content://Scenes/Hold.nfscene");
        for (int frame = 0; frame < 180; ++frame) { // 3 s: enough to be hit
            rt.update(1.0f / 60.0f);
        }
        if (out_health != nullptr) {
            const HealthComponent* h = rt.health_of(player);
            *out_health = h != nullptr ? h->health : -1.0f;
        }
        if (out_holds != nullptr) {
            *out_holds = rt.brain_holds_fire(bandit);
        }
    };

    f32 fired_health = 0.0f;
    run(false, &fired_health, nullptr);
    NF_CHECK(fired_health < 100.0f); // the control: this scene shoots

    f32 held_health = 0.0f;
    bool holds = false;
    run(true, &held_health, &holds);
    NF_CHECK(holds);
    NF_CHECK_NEAR(held_health, 100.0f, 1e-3f); // not a scratch more

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_script_reads_its_own_decision) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_brain_script";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    // The script writes the decision it reads into its own height: 3 for
    // "engage", 1 for anything else (or nothing at all).
    const std::string script =
        "function update(dt)\n"
        "  local id, gen = nf.self()\n"
        "  local x, y, z = nf.entity_pos(id, gen)\n"
        "  local name = nf.decision()\n"
        "  nf.set_entity_pos(id, gen, x, (name == 'engage') and 3.0 or 1.0, z)\n"
        "end\n";
    NF_CHECK(vfs.write_text("content://Scripts/watch.lua", script).ok);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);
        NoInput no_input;
        rt.set_input_source(&no_input);

        auto game = std::make_unique<scene::Scene>("Decider");
        const ecs::Entity target = add_entity(*game, "Target", 0.0f, 0.0f, -10.0f);
        add_health(*game, target, 100.0f);

        const ecs::Entity guard = add_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        game->world().add<PerceptionComponent>(guard, sharp_eyes());
        BrainComponent brain;
        brain.rate = 0.0f;
        brain.actions.push_back(engage_when_aware("engage"));
        game->world().add<BrainComponent>(guard, brain);
        scripting::ScriptComponent script_component;
        script_component.path = "content://Scripts/watch.lua";
        game->world().add<scripting::ScriptComponent>(guard, script_component);

        rt.adopt_scene(std::move(game), "content://Scenes/Decider.nfscene");
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        const scene::Scene* live = rt.scene();
        NF_CHECK(live != nullptr);
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
