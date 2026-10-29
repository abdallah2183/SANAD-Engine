// Tests/RuntimeTests/test_perception_runtime.cpp — Phase 60: the senses, bound
// to entities and fed by the frame.
//
// `ai::Perception` has had its own suite since Phase 22 (30 cases in AITests),
// and it was reachable from nothing: no scene could declare a sensor, and no
// frame fed it a cast or an event. That is the gap these cases close, and they
// are deliberately about the BRIDGE rather than about the module:
//
//   1. the scene line: `Perception:` round-trips through the real writer/reader,
//      an absent line stays absent, an impossible value is refused by name;
//   2. the validator: a value outside the module's range is warned about by
//      name, and an agent whose senses govern its combat with every sense
//      switched off is an ERROR — it can never acquire a target, which is a
//      level that does not work rather than one that looks a little off;
//   3. the frame: a blind NPC hears a gunshot, sight needs the cone and a clear
//      line, a wall stops sight but not hearing, pain wakes the victim and
//      nobody else, and a sensor never detects itself;
//   4. the bridge: an agent with senses only engages what it has ACCUMULATED,
//      keeps aiming at the last known position after it loses sight, and holds
//      fire while it cannot sense — while `combat=false` keeps the pre-Phase-60
//      rule exactly.
//
// The pure half needs no device. The integration half skips without Vulkan, the
// same contract every other extraction test keeps.

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
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;
using namespace nf::runtime;

namespace {

/// The lip of the barrel: a weapon the agent can fire with, with no spread and
/// no recoil so the cases are about perception and not about luck.
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

/// A sensor that only hears: blindness and deafness are authored as a range of
/// zero, which is the module's own contract, not a flag invented here.
PerceptionComponent ears_only(f32 range) {
    PerceptionComponent pc;
    pc.sight_range = 0.0f;
    pc.hearing_range = range;
    pc.pain = false;
    pc.eye_height = 0.0f;
    return pc;
}

/// A camera that follows the named entity, which is what makes it "the player"
/// to the runtime.
void add_follow_camera(scene::Scene& scene, const char* target, f32 distance, f32 pivot_y) {
    const ecs::Entity camera = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
    scene.world().add<scene::Transform>(camera, scene::Transform{});
    CameraComponent cam;
    cam.is_active = true;
    cam.follow_target = target;
    cam.pivot_y = pivot_y;
    cam.distance = distance;
    cam.mouse_look = false;
    // Yaw and pitch EXPLICITLY zero. The camera's defaults aim slightly down
    // (the third-person default view), and a shot fired along that camera at a
    // target ten metres away passes two metres over its head — which reads as
    // "the perception bridge does not work" for a whole afternoon.
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    scene.world().add<CameraComponent>(camera, cam);
}

ecs::Entity add_world_entity(scene::Scene& scene, const char* name, f32 x, f32 y, f32 z,
                             bool with_collider = true) {
    const ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{name});
    auto& t = scene.world().add<scene::Transform>(e);
    t.local_x = x;
    t.local_y = y;
    t.local_z = z;
    if (with_collider) {
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

/// A scene with no camera is a validation ERROR of its own (Phase 43), so a case
/// about perception warnings has to carry one or it is testing two things.
void add_bare_camera(scene::Scene& scene) {
    const ecs::Entity camera = scene.world().create_entity();
    scene.world().add<scene::NameComponent>(camera, scene::NameComponent{"Camera"});
    scene.world().add<scene::Transform>(camera, scene::Transform{});
    CameraComponent cam;
    cam.is_active = true;
    scene.world().add<CameraComponent>(camera, cam);
}

/// The smallest input source there is.
///
/// A Runtime with NO input source leaves the whole `ScriptInput` empty — not
/// just the input callbacks but every world query the script API carries — so a
/// script test that wants `nf.*` to answer anything must hand one over. This one
/// answers "nothing is pressed" to everything.
struct NoInput final : gameplay::IInputSource {
    [[nodiscard]] bool action_pressed(std::string_view) const override { return false; }
    [[nodiscard]] f32 action_axis(std::string_view) const override { return 0.0f; }
};

} // namespace

// --- the scene line ----------------------------------------------------------

NF_TEST(perception_line_round_trips_every_key) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_perception_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    scene::Scene scene("PerceptionScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    PerceptionComponent pc;
    pc.enabled = false;
    pc.pain = false;
    pc.governs_combat = false;
    pc.sight_range = 12.5f;
    pc.sight_half_fov_deg = 45.0f;
    pc.sight_gain = 3.5f;
    pc.sight_loss = 0.25f;
    pc.sight_linger = 7.5f;
    pc.hearing_range = 22.0f;
    pc.touch_range = 2.5f;
    pc.noise = 0.3f;
    pc.visibility = 0.6f;
    pc.radius = 0.8f;
    pc.eye_height = 1.8f;
    world.add<PerceptionComponent>(e, pc);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/perception.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/perception.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const auto* loaded = result.scene->world().get<PerceptionComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(!loaded->enabled);
        NF_CHECK(!loaded->pain);
        NF_CHECK(!loaded->governs_combat);
        NF_CHECK_NEAR(loaded->sight_range, 12.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->sight_half_fov_deg, 45.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->sight_gain, 3.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->sight_loss, 0.25f, 1e-5f);
        NF_CHECK_NEAR(loaded->sight_linger, 7.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->hearing_range, 22.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->touch_range, 2.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->noise, 0.3f, 1e-5f);
        NF_CHECK_NEAR(loaded->visibility, 0.6f, 1e-5f);
        NF_CHECK_NEAR(loaded->radius, 0.8f, 1e-5f);
        NF_CHECK_NEAR(loaded->eye_height, 1.8f, 1e-5f);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(perception_absent_from_a_scene_stays_absent_when_saved) {
    scene::Scene scene("PlainScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    const std::string text = serialize_scene_to_text(scene);
    NF_CHECK(text.find("Perception:") == std::string::npos);

    auto parsed = load_scene_from_text(text);
    NF_CHECK(parsed.success);
    if (parsed.scene) {
        NF_CHECK(parsed.scene->world().get<PerceptionComponent>(
                      parsed.scene->world().all_entities()[0]) == nullptr);
    }
}

NF_TEST(perception_line_with_an_impossible_value_is_refused_by_name) {
    // A half-angle of 200 degrees is not a cone; the loader refuses the whole
    // line rather than clamping it, and names the key.
    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: BadPerception\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Guard\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
        "parent(4294967295:0)\n"
        "  Perception: sight=30 fov=200\n";
    auto result = load_scene_from_text(text);
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        return;
    }
    NF_CHECK(result.scene->world().get<PerceptionComponent>(
                  result.scene->world().all_entities()[0]) == nullptr);
    bool named = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("Perception") != std::string::npos &&
            warning.find("fov") != std::string::npos) {
            named = true;
            break;
        }
    }
    NF_CHECK(named);

    // A one-key line keeps every other default, which is the contract every
    // optional scene line in this engine keeps.
    const std::string minimal =
        "# SANAD Scene v1\nversion: 1\nname: Minimal\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Guard\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
        "parent(4294967295:0)\n"
        "  Perception: hearing=0\n";
    auto parsed = load_scene_from_text(minimal);
    NF_CHECK(parsed.success);
    if (parsed.scene) {
        const auto* component = parsed.scene->world().get<PerceptionComponent>(
            parsed.scene->world().all_entities()[0]);
        NF_CHECK(component != nullptr);
        if (component != nullptr) {
            NF_CHECK_NEAR(component->hearing_range, 0.0f, 1e-6f);
            NF_CHECK_NEAR(component->sight_range, 30.0f, 1e-6f);
            NF_CHECK(component->pain);
            NF_CHECK(component->governs_combat);
        }
    }
}

// --- squad radio (Phase 68) --------------------------------------------------

NF_TEST(squad_line_round_trips_every_key) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_squad_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    scene::Scene scene("SquadScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    SquadComponent squad;
    squad.token = "guards";
    squad.radio = 45.5f;
    squad.lost = 2.5f;
    squad.cone = 35.5f;
    squad.enabled = false;
    world.add<SquadComponent>(e, squad);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/squad.nfscene", scene, err));
    NF_CHECK(err.empty());

    const auto text = vfs.read_text("content://Scenes/squad.nfscene");
    NF_CHECK(text.ok);
    NF_CHECK(text.value.find(
                 "Squad: token=guards radio=45.5 lost=2.5 cone=35.5 enabled=false") !=
             std::string::npos);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/squad.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const auto* loaded = result.scene->world().get<SquadComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(loaded->token == "guards");
        NF_CHECK_NEAR(loaded->radio, 45.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->lost, 2.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->cone, 35.5f, 1e-5f);
        NF_CHECK(!loaded->enabled);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(squad_absent_or_empty_token_writes_no_line) {
    scene::Scene plain("PlainSquadScene");
    plain.world().add<scene::Transform>(plain.world().create_entity(), scene::Transform{});
    const std::string plain_text = serialize_scene_to_text(plain);
    NF_CHECK(plain_text.find("Squad:") == std::string::npos);

    // An empty token is the "no radio" state, exactly like an empty Faction
    // team: the component may exist (the editor authored it) but it writes no
    // line, so the round trip cannot resurrect an inert radio.
    scene::Scene empty("EmptySquadScene");
    auto& world = empty.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    world.add<SquadComponent>(e, SquadComponent{});
    const std::string empty_text = serialize_scene_to_text(empty);
    NF_CHECK(empty_text.find("Squad:") == std::string::npos);

    auto parsed = load_scene_from_text(empty_text);
    NF_CHECK(parsed.success);
    if (parsed.scene) {
        NF_CHECK(parsed.scene->world().get<SquadComponent>(
                      parsed.scene->world().all_entities()[0]) == nullptr);
    }
}

NF_TEST(squad_line_defaults_and_refuses_bad_values_by_name) {
    const std::string minimal =
        "# SANAD Scene v1\nversion: 1\nname: MinimalSquad\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Guard\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
        "parent(4294967295:0)\n"
        "  Squad: token=guards\n";
    auto parsed = load_scene_from_text(minimal);
    NF_CHECK(parsed.success);
    if (parsed.scene) {
        const auto* squad = parsed.scene->world().get<SquadComponent>(
            parsed.scene->world().all_entities()[0]);
        NF_CHECK(squad != nullptr);
        if (squad != nullptr) {
            NF_CHECK(squad->token == "guards");
            NF_CHECK_NEAR(squad->radio, 30.0f, 1e-6f);
            NF_CHECK_NEAR(squad->lost, 0.0f, 1e-6f);
            NF_CHECK_NEAR(squad->cone, 0.0f, 1e-6f);
            NF_CHECK(squad->enabled);
        }
    }

    const auto refused = [](const std::string& line, const char* key) {
        const std::string text =
            "# SANAD Scene v1\nversion: 1\nname: BadSquad\nentity_count: 1\n"
            "---\nentity: 1:0\n  Name: Guard\n"
            "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
            "parent(4294967295:0)\n  " + line + "\n";
        auto r = load_scene_from_text(text);
        if (!r.success || !r.scene) {
            return false;
        }
        const ecs::Entity e = r.scene->world().all_entities()[0];
        bool named = false;
        for (const std::string& warning : r.warnings) {
            if (warning.find("Squad") != std::string::npos &&
                warning.find(key) != std::string::npos) {
                named = true;
                break;
            }
        }
        return r.scene->world().get<SquadComponent>(e) == nullptr && named;
    };

    // A token is required: without one the radio addresses nobody.
    NF_CHECK(refused("Squad: radio=45", "token"));
    // The loader's cap is 64 characters; 65 is refused rather than truncated,
    // because a silently shortened token would address a different squad.
    NF_CHECK(refused("Squad: token=" + std::string(65, 'a'), "token"));
    // The range bounds are the editor's and the validator's.
    NF_CHECK(refused("Squad: token=guards radio=20000", "radio"));
    NF_CHECK(refused("Squad: token=guards radio=-1", "radio"));
    // Phase 69 adds the lost-callout window with the same bounds.
    NF_CHECK(refused("Squad: token=guards lost=20000", "lost"));
    NF_CHECK(refused("Squad: token=guards lost=-1", "lost"));
    // Phase 71 adds the horizontal antenna half-angle, [0, 180].
    NF_CHECK(refused("Squad: token=guards cone=181", "cone"));
    NF_CHECK(refused("Squad: token=guards cone=-1", "cone"));

    // Exactly 64 characters is legal and round-trips.
    const std::string longest(64, 'a');
    const std::string cap_text =
        "# SANAD Scene v1\nversion: 1\nname: CapSquad\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Guard\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
        "parent(4294967295:0)\n  Squad: token=" + longest + " cone=180\n";
    auto cap = load_scene_from_text(cap_text);
    NF_CHECK(cap.success);
    if (cap.scene) {
        const auto* squad = cap.scene->world().get<SquadComponent>(
            cap.scene->world().all_entities()[0]);
        NF_CHECK(squad != nullptr);
        if (squad != nullptr) {
            NF_CHECK(squad->token == longest);
            NF_CHECK_NEAR(squad->cone, 180.0f, 1e-6f);
        }
    }
}

NF_TEST(squad_values_with_nothing_to_send_are_warned_by_name) {
    scene::Scene scene("SquadWarn");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_world_entity(scene, "Guard", 0.0f, 0.0f, 0.0f, false);
    SquadComponent squad;
    squad.token = "two words";
    squad.radio = 0.0f;
    squad.lost = 20000.0f;
    squad.cone = 200.0f;
    world.add<SquadComponent>(e, squad);

    const ValidationReport report = validate_scene(scene, "content://Scenes/squad_warn.nfscene");
    bool whitespace = false;
    bool silent = false;
    bool no_perception = false;
    bool lost_window = false;
    bool cone_window = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("whitespace") != std::string::npos) whitespace = true;
        if (issue.message.find("radio=0") != std::string::npos) silent = true;
        if (issue.message.find("Perception") != std::string::npos) no_perception = true;
        if (issue.message.find("lost") != std::string::npos) lost_window = true;
        if (issue.message.find("cone") != std::string::npos &&
            issue.message.find("outside") != std::string::npos) {
            cone_window = true;
        }
    }
    NF_CHECK(whitespace);
    NF_CHECK(silent);
    NF_CHECK(no_perception);
    NF_CHECK(lost_window);
    NF_CHECK(cone_window);

    // A well-formed radio with a sensor is not worth a warning.
    scene::Scene good("SquadGood");
    add_bare_camera(good);
    const ecs::Entity g = add_world_entity(good, "Guard", 0.0f, 0.0f, 0.0f, false);
    SquadComponent fine;
    fine.token = "guards";
    fine.radio = 45.0f;
    fine.lost = 2.0f;
    fine.cone = 90.0f;
    good.world().add<SquadComponent>(g, fine);
    good.world().add<PerceptionComponent>(g, PerceptionComponent{});
    const ValidationReport quiet =
        validate_scene(good, "content://Scenes/squad_good.nfscene");
    bool squad_issue = false;
    for (const ValidationIssue& issue : quiet.issues) {
        if (issue.message.find("Squad") != std::string::npos) squad_issue = true;
    }
    NF_CHECK(!squad_issue);
}

// --- the validator -----------------------------------------------------------

NF_TEST(perception_values_outside_the_module_range_are_warned_by_name) {
    scene::Scene scene("Warn");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_world_entity(scene, "Guard", 0.0f, 0.0f, 0.0f, false);
    PerceptionComponent pc;
    pc.sight_gain = -1.0f;
    pc.visibility = 4.0f;
    pc.eye_height = 1000.0f;
    world.add<PerceptionComponent>(e, pc);

    const ValidationReport report = validate_scene(scene, "content://Scenes/warn.nfscene");
    bool gain = false;
    bool visible = false;
    bool eye = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("gain") != std::string::npos) gain = true;
        if (issue.message.find("visible") != std::string::npos) visible = true;
        if (issue.message.find("eye") != std::string::npos) eye = true;
    }
    NF_CHECK(gain);
    NF_CHECK(visible);
    NF_CHECK(eye);
}

NF_TEST(an_agent_governing_combat_with_no_working_sense_is_an_error) {
    // The wiring trap: `combat=true` makes the senses the gate on engagement, so
    // an agent whose senses are all off can never fire. The validator calls it an
    // error because that level does not work, not because it looks slightly off.
    scene::Scene scene("Broken");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_world_entity(scene, "Guard", 0.0f, 0.0f, 0.0f, false);
    world.add<WeaponComponent>(e, WeaponComponent{straight_gun()});
    world.add<CombatAgentComponent>(e, CombatAgentComponent{});
    PerceptionComponent pc;
    pc.sight_range = 0.0f;
    pc.hearing_range = 0.0f;
    pc.touch_range = 0.0f;
    pc.pain = false;
    world.add<PerceptionComponent>(e, pc);

    const ValidationReport report = validate_scene(scene, "content://Scenes/broken.nfscene");
    NF_CHECK(!report.ok());
    bool named = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.severity == ValidationSeverity::Error &&
            issue.message.find("never acquire") != std::string::npos) {
            named = true;
            break;
        }
    }
    NF_CHECK(named);

    // The same sensor with the brain NOT governed by it is only a warning-free
    // curiosity: the agent keeps the old ray rule, so nothing is broken.
    scene::Scene other("Fine");
    auto& ow = other.world();
    add_bare_camera(other);
    const ecs::Entity o = add_world_entity(other, "Guard", 0.0f, 0.0f, 0.0f, false);
    ow.add<WeaponComponent>(o, WeaponComponent{straight_gun()});
    ow.add<CombatAgentComponent>(o, CombatAgentComponent{});
    PerceptionComponent off = pc;
    off.governs_combat = false;
    ow.add<PerceptionComponent>(o, off);
    NF_CHECK(validate_scene(other, "content://Scenes/fine.nfscene").ok());
}

// --- the frame ---------------------------------------------------------------

NF_TEST(a_sensor_never_detects_itself) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_self";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Lonely");
        const ecs::Entity guard = add_world_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        // Everything on: if a sensor could detect itself, this one would fill its
        // own meter in half a second (sight at zero distance is a full-strength
        // stimulus) and touch would report instantly.
        PerceptionComponent pc;
        pc.eye_height = 0.0f;
        pc.pain = true;
        game->world().add<PerceptionComponent>(guard, pc);

        rt.adopt_scene(std::move(game), "content://Scenes/Lonely.nfscene");
        for (int frame = 0; frame < 120; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        NF_CHECK_EQ(rt.perception_count(), static_cast<size_t>(1));
        NF_CHECK(!rt.entity_aware_of(guard, guard));
        const ai::Perception* senses = rt.perception_of(guard);
        NF_CHECK(senses != nullptr);
        if (senses != nullptr) {
            NF_CHECK(senses->perceived().empty());
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_blind_npc_hears_a_gunshot_and_becomes_aware) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_hearing";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Alley");
        const ecs::Entity player = add_world_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
        add_health(*game, player, 100.0f);
        game->world().add<WeaponComponent>(player, WeaponComponent{straight_gun()});
        add_follow_camera(*game, "Player", 4.0f, 0.0f);

        // Ten metres away, deaf to nothing but blind: the only way this NPC can
        // learn about the player is the noise the gun makes.
        const ecs::Entity guard = add_world_entity(*game, "Guard", 0.0f, 1.5f, -10.0f);
        add_health(*game, guard, 100.0f);
        game->world().add<PerceptionComponent>(guard, ears_only(40.0f));

        rt.adopt_scene(std::move(game), "content://Scenes/Alley.nfscene");
        NF_CHECK(rt.perception_count() == 1u);
        NF_CHECK(!rt.entity_aware_of(guard, player)); // nothing has happened yet

        NF_CHECK_EQ(rt.fire_weapon(), 1u);
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        NF_CHECK(rt.entity_aware_of(guard, player));
        // Heard is not seen: the memory holds the position the NOISE came from —
        // the muzzle, a few metres behind the player — and the sense that
        // reported it was hearing, not sight.
        Vec3 known{};
        NF_CHECK(rt.entity_last_known(guard, player, known));
        NF_CHECK(known.z > -5.0f);
        NF_CHECK(known.z < 8.0f);
        const ai::Perception* senses = rt.perception_of(guard);
        NF_CHECK(senses != nullptr);
        if (senses != nullptr) {
            const ai::PerceivedTarget* memory = senses->most_threatening();
            NF_CHECK(memory != nullptr);
            if (memory != nullptr) {
                NF_CHECK(memory->detection_of(ai::StimulusKind::Sound) > 0.0f);
                NF_CHECK_NEAR(memory->detection_of(ai::StimulusKind::Sight), 0.0f, 1e-6f);
            }
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(sight_needs_the_cone_and_a_clear_line) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_sight";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Corridor");
        // The observer stands at the origin and looks down -Z (yaw 0). The
        // target is ten metres down -Z, so it starts inside the cone.
        const ecs::Entity target = add_world_entity(*game, "Target", 0.0f, 0.0f, -10.0f);
        add_health(*game, target, 100.0f);

        const ecs::Entity guard = add_world_entity(*game, "Guard", 0.0f, 0.0f, 0.0f);
        add_health(*game, guard, 100.0f);
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 45.0f;
        eyes.sight_gain = 4.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        eyes.eye_height = 0.0f;
        game->world().add<PerceptionComponent>(guard, eyes);

        rt.adopt_scene(std::move(game), "content://Scenes/Corridor.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.entity_aware_of(guard, target));

        // A wall between them: sight is the only sense that consults occlusion,
        // and the guard now sees the wall instead of the target. The cast is
        // rebuilt every frame, so a new collider needs no rebind — only a new
        // SENSOR does.
        scene::Scene* live = rt.edit_scene();
        NF_CHECK(live != nullptr);
        if (live != nullptr) {
            add_world_entity(*live, "Wall", 0.0f, 0.0f, -5.0f);
        }
        for (int frame = 0; frame < 120; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(!rt.entity_sensing(guard, target));
        // Awareness lingers: the guard does not forget the moment it looks away.
        NF_CHECK(rt.entity_aware_of(guard, target));

        // Look away and the cone does the rest — even with a clear line, a target
        // behind the guard is not seen. After the linger window (3 s) the memory
        // is gone entirely, which is the search window closing.
        scene::Transform* guard_transform = live != nullptr
                                                ? live->world().get<scene::Transform>(guard)
                                                : nullptr;
        NF_CHECK(guard_transform != nullptr);
        if (guard_transform != nullptr) {
            guard_transform->rot_y = 90.0f; // now looking down -X
        }
        for (int frame = 0; frame < 240; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(!rt.entity_sensing(guard, target));
        NF_CHECK(!rt.entity_aware_of(guard, target));
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(pain_wakes_the_victim_and_nobody_else) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_pain";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("PainRange");
        const ecs::Entity player = add_world_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
        add_health(*game, player, 100.0f);
        game->world().add<WeaponComponent>(player, WeaponComponent{straight_gun()});
        add_follow_camera(*game, "Player", 4.0f, 0.0f);

        // The victim, straight down the barrel.
        const ecs::Entity victim = add_world_entity(*game, "Victim", 0.0f, 1.5f, -10.0f);
        add_health(*game, victim, 100.0f);
        PerceptionComponent feeling;
        feeling.sight_range = 0.0f;
        feeling.hearing_range = 0.0f;
        feeling.pain = true;
        feeling.eye_height = 0.0f;
        game->world().add<PerceptionComponent>(victim, feeling);

        // The bystander, out of the line of fire and stone deaf with no sight.
        // If pain were broadcast through the shared cast, the damage sense
        // (which ignores distance entirely) would wake this one too.
        const ecs::Entity bystander = add_world_entity(*game, "Bystander", 9.0f, 1.5f, -10.0f);
        add_health(*game, bystander, 100.0f);
        game->world().add<PerceptionComponent>(bystander, feeling);

        rt.adopt_scene(std::move(game), "content://Scenes/PainRange.nfscene");
        NF_CHECK_EQ(rt.fire_weapon(), 1u);
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        NF_CHECK(rt.entity_aware_of(victim, player)); // the positive control
        NF_CHECK(!rt.entity_aware_of(bystander, player));
        NF_CHECK(rt.perception_of(bystander) != nullptr);
        if (rt.perception_of(bystander) != nullptr) {
            NF_CHECK(rt.perception_of(bystander)->perceived().empty());
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(pain_names_the_attacker_and_not_its_neighbour_in_the_cast) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_ordering";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("Ordering");
        // The victim is created FIRST, so it heads the cast. A sensor's own cast
        // drops the sensor, which shifts the id of every actor after it — so
        // without the shared-id to local-id table the wound would be attributed
        // to whichever actor sits at the shooter's old number, and the victim
        // would "know" the wrong entity was shooting at it.
        const ecs::Entity victim = add_world_entity(*game, "Victim", 0.0f, 1.5f, -10.0f);
        add_health(*game, victim, 100.0f);
        PerceptionComponent feeling;
        feeling.sight_range = 0.0f;
        feeling.hearing_range = 0.0f;
        feeling.pain = true;
        feeling.eye_height = 0.0f;
        game->world().add<PerceptionComponent>(victim, feeling);

        const ecs::Entity player = add_world_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
        add_health(*game, player, 100.0f);
        game->world().add<WeaponComponent>(player, WeaponComponent{straight_gun()});
        add_follow_camera(*game, "Player", 4.0f, 0.0f);

        // The third actor: what a shifted lookup would blame instead.
        const ecs::Entity other = add_world_entity(*game, "Other", 6.0f, 1.5f, -10.0f);
        add_health(*game, other, 100.0f);

        rt.adopt_scene(std::move(game), "content://Scenes/Ordering.nfscene");
        NF_CHECK_EQ(rt.fire_weapon(), 1u);
        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        NF_CHECK(rt.last_weapon_hit().hit);
        NF_CHECK(rt.last_weapon_hit().name == "Victim");
        NF_CHECK(rt.entity_aware_of(victim, player));
        NF_CHECK(!rt.entity_aware_of(victim, other));
        const ai::Perception* senses = rt.perception_of(victim);
        NF_CHECK(senses != nullptr);
        if (senses != nullptr) {
            NF_CHECK_EQ(senses->perceived().size(), static_cast<size_t>(1));
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

// --- the bridge ---------------------------------------------------------------
NF_TEST(an_agent_with_senses_only_engages_what_it_knows) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_bridge";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);

        // --- with senses governing combat: the agent cannot shoot what it has
        // --- not noticed yet.
        {
            Runtime rt(vfs, registry, manager, *device, nullptr);
            auto game = std::make_unique<scene::Scene>("Governed");

            const ecs::Entity player = add_world_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
            add_health(*game, player, 100.0f);
            add_follow_camera(*game, "Player", 4.0f, 0.0f);

            const ecs::Entity bandit = add_world_entity(*game, "Bandit", 0.0f, 1.5f, -10.0f);
            game->world().get<scene::Transform>(bandit)->rot_y = 180.0f; // facing +Z
            game->world().add<WeaponComponent>(bandit, WeaponComponent{straight_gun()});
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
            game->world().add<CombatAgentComponent>(bandit, agent);

            PerceptionComponent eyes;
            eyes.sight_range = 30.0f;
            eyes.sight_half_fov_deg = 90.0f;
            eyes.sight_gain = 4.0f;
            eyes.hearing_range = 0.0f;
            eyes.touch_range = 0.0f;
            eyes.pain = false;
            eyes.eye_height = 0.0f;
            eyes.governs_combat = true;
            game->world().add<PerceptionComponent>(bandit, eyes);

            rt.adopt_scene(std::move(game), "content://Scenes/Governed.nfscene");

            // Half a second: the guard has a clear line and a wide cone, but the
            // meter needs time to fill, so nothing is fired yet.
            for (int frame = 0; frame < 30; ++frame) {
                rt.update(1.0f / 60.0f);
            }
            const HealthComponent* early = rt.health_of(player);
            NF_CHECK(early != nullptr);
            if (early != nullptr) {
                NF_CHECK_NEAR(early->health, 100.0f, 1e-3f);
            }

            // Three more seconds: noticed, then aimed, then fired.
            for (int frame = 0; frame < 180; ++frame) {
                rt.update(1.0f / 60.0f);
            }
            NF_CHECK(rt.entity_aware_of(bandit, player));
            const HealthComponent* late = rt.health_of(player);
            NF_CHECK(late != nullptr);
            if (late != nullptr) {
                NF_CHECK(late->health < 100.0f);
            }
        }

        // --- the compatibility half: `combat=false` keeps the pre-Phase-60 rule
        // --- exactly, so the component can be added without changing a fight.
        {
            Runtime rt(vfs, registry, manager, *device, nullptr);
            auto game = std::make_unique<scene::Scene>("Ungoverned");

            const ecs::Entity player = add_world_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
            add_health(*game, player, 100.0f);
            add_follow_camera(*game, "Player", 4.0f, 0.0f);

            const ecs::Entity bandit = add_world_entity(*game, "Bandit", 0.0f, 1.5f, -10.0f);
            game->world().get<scene::Transform>(bandit)->rot_y = 180.0f;
            game->world().add<WeaponComponent>(bandit, WeaponComponent{straight_gun()});
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
            game->world().add<CombatAgentComponent>(bandit, agent);

            PerceptionComponent eyes;
            // Every sense off AND combat off: the sensor exists, does nothing,
            // and must not gate the trigger.
            eyes.sight_range = 0.0f;
            eyes.hearing_range = 0.0f;
            eyes.touch_range = 0.0f;
            eyes.pain = false;
            eyes.governs_combat = false;
            game->world().add<PerceptionComponent>(bandit, eyes);

            rt.adopt_scene(std::move(game), "content://Scenes/Ungoverned.nfscene");
            for (int frame = 0; frame < 30; ++frame) {
                rt.update(1.0f / 60.0f);
            }
            const HealthComponent* hurt = rt.health_of(player);
            NF_CHECK(hurt != nullptr);
            if (hurt != nullptr) {
                NF_CHECK(hurt->health < 100.0f);
            }
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(an_agent_lost_sight_tracks_the_memory_and_holds_fire) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_memory";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("LostAndFound");
        const ecs::Entity player = add_world_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
        add_health(*game, player, 100.0f);
        add_follow_camera(*game, "Player", 4.0f, 0.0f);

        const ecs::Entity bandit = add_world_entity(*game, "Bandit", 0.0f, 1.5f, -10.0f);
        game->world().get<scene::Transform>(bandit)->rot_y = 180.0f;
        game->world().add<WeaponComponent>(bandit, WeaponComponent{straight_gun()});
        CombatAgentComponent agent;
        agent.target = "Player";
        agent.eye_height = 0.0f;
        agent.target_height = 0.0f;
        agent.config.reaction_seconds = 0.0f;
        agent.config.aim_speed_deg_per_s = 1800.0f;
        agent.config.fire_cone_deg = 10.0f;
        agent.config.max_range = 60.0f;
        agent.config.burst_on_seconds = 5.0f;
        agent.config.burst_off_seconds = 0.0f;
        game->world().add<CombatAgentComponent>(bandit, agent);

        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 90.0f;
        eyes.sight_gain = 8.0f;
        eyes.sight_linger = 5.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        eyes.eye_height = 0.0f;
        game->world().add<PerceptionComponent>(bandit, eyes);

        rt.adopt_scene(std::move(game), "content://Scenes/LostAndFound.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        const HealthComponent* hit = rt.health_of(player);
        NF_CHECK(hit != nullptr);
        if (hit != nullptr) {
            NF_CHECK(hit->health < 100.0f); // it was fighting
        }
        const f32 before = hit != nullptr ? hit->health : 0.0f;

        // Teleport the player far out of sight. The memory keeps where it was,
        // and the agent must stop FIRING (a shot needs a sense) while it keeps
        // knowing (the linger window is the search behaviour).
        scene::Transform* player_transform =
            rt.edit_scene()->world().get<scene::Transform>(player);
        NF_CHECK(player_transform != nullptr);
        if (player_transform != nullptr) {
            player_transform->local_z = 500.0f;
        }
        for (int frame = 0; frame < 90; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        const HealthComponent* after = rt.health_of(player);
        NF_CHECK(after != nullptr);
        if (after != nullptr) {
            NF_CHECK_NEAR(after->health, before, 1e-3f); // not a scratch more
        }
        NF_CHECK(!rt.entity_sensing(bandit, player));
        // The remembered position is where the player WAS, not where it is.
        Vec3 known{};
        NF_CHECK(rt.entity_last_known(bandit, player, known));
        NF_CHECK(std::fabs(known.z) < 15.0f);

        // And a script can make it forget: the memory is not a permanent record.
        NF_CHECK(rt.entity_forget(bandit, player));
        NF_CHECK(!rt.entity_aware_of(bandit, player));
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
// --- the script half ----------------------------------------------------------

NF_TEST(a_script_reads_its_own_senses) {
    // The bridge a game actually touches: a guard's update(dt) asking its own
    // memory. These four functions are the whole script-visible surface of the
    // phase, and the observable is the guard's own height -- the only thing a
    // test can read back out of a Lua script.
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_perception_script";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    // 3 = I can sense it now, 2 = I know where it was, 4 = I made myself forget.
    const std::string script =
        "local frames = 0\n"
        "local forgot = false\n"
        "function update(dt)\n"
        "  frames = frames + 1\n"
        "  local id, gen = nf.self()\n"
        "  local x, y, z = nf.entity_pos(id, gen)\n"
        "  local pid, pgen = nf.find_entity('Player')\n"
        "  if not pid then return end\n"
        "  if forgot then\n"
        "    nf.set_entity_pos(id, gen, x, 4.0, z)\n"
        "  elseif nf.sensing(pid, pgen) then\n"
        "    nf.set_entity_pos(id, gen, x, 3.0, z)\n"
        "  elseif nf.aware(pid, pgen) then\n"
        "    if frames > 90 then\n"
        "      forgot = nf.forget(pid, pgen)\n"
        "    end\n"
        "    if forgot then\n"
        "      nf.set_entity_pos(id, gen, x, 4.0, z)\n"
        "    else\n"
        "      local kx, ky, kz = nf.last_known(pid, pgen)\n"
        "      if kx then nf.set_entity_pos(id, gen, kx, 2.0, kz) end\n"
        "    end\n"
        "  else\n"
        "    nf.set_entity_pos(id, gen, x, 1.0, z)\n"
        "  end\n"
        "end\n";
    NF_CHECK(vfs.write_text("content://Scripts/watch.lua", script).ok);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);
        NoInput no_input;
        rt.set_input_source(&no_input);

        auto game = std::make_unique<scene::Scene>("Watchtower");
        const ecs::Entity player = add_world_entity(*game, "Player", 0.0f, 1.5f, 0.0f);
        add_health(*game, player, 100.0f);
        add_follow_camera(*game, "Player", 0.0f, 0.0f);

        const ecs::Entity guard = add_world_entity(*game, "Guard", 0.0f, 1.5f, -10.0f);
        add_health(*game, guard, 100.0f);
        game->world().get<scene::Transform>(guard)->rot_y = 180.0f; // facing +Z
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 90.0f;
        eyes.sight_gain = 8.0f;
        eyes.sight_linger = 3.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        eyes.eye_height = 0.0f;
        game->world().add<PerceptionComponent>(guard, eyes);
        scripting::ScriptComponent script_component;
        script_component.path = "content://Scripts/watch.lua";
        game->world().add<scripting::ScriptComponent>(guard, script_component);

        rt.adopt_scene(std::move(game), "content://Scenes/Watchtower.nfscene");

        const auto guard_y = [&]() -> f32 {
            const scene::Scene* live = rt.scene();
            if (live == nullptr) {
                return -1.0f;
            }
            const auto* t = live->world().get<scene::Transform>(guard);
            return t != nullptr ? t->local_y : -1.0f;
        };

        for (int frame = 0; frame < 30; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(rt.entity_sensing(guard, player));
        NF_CHECK_NEAR(guard_y(), 3.0f, 1e-3f); // nf.sensing

        // Out of sight: aware, tracking where it was.
        scene::Transform* player_transform =
            rt.edit_scene()->world().get<scene::Transform>(player);
        NF_CHECK(player_transform != nullptr);
        if (player_transform != nullptr) {
            player_transform->local_z = 500.0f;
        }
        for (int frame = 0; frame < 15; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(!rt.entity_sensing(guard, player));
        NF_CHECK(rt.entity_aware_of(guard, player));
        NF_CHECK_NEAR(guard_y(), 2.0f, 1e-3f); // nf.aware + nf.last_known

        // Past its own frame 90 the script makes itself forget, and says so.
        for (int frame = 0; frame < 90; ++frame) {
            rt.update(1.0f / 60.0f);
        }
        NF_CHECK(!rt.entity_aware_of(guard, player));
        NF_CHECK_NEAR(guard_y(), 4.0f, 1e-3f); // nf.forget returned true
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

// --- squad radio (Phase 68) --------------------------------------------------
//
// The radio is the one path by which a memory enters a sensor without a sense.
// These cases are about what that memory can and cannot do: a same-token mate
// inside BOTH radios' reach learns the contact and its last known position, but
// the report never fills a meter, never becomes a personal sighting, and never
// travels a second hop. The filters (token, range, faction, corpse, same-token
// target) are the frame's, not the module's, so they are pinned here.

NF_TEST(squad_radio_shares_only_personal_live_contacts) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_squad_radio";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("RadioNet");

        // A radio member: hit points, a team, an enabled (but blind) sensor and
        // a radio. No collider, so the observer's sight line stays clear and
        // the case is about the radio, not about occlusion.
        const auto make_radio = [&](const char* name, f32 x, f32 z, const char* team,
                                    const char* token, f32 radio) {
            const ecs::Entity e = add_world_entity(*game, name, x, 0.0f, z, false);
            add_health(*game, e, 100.0f);
            FactionComponent faction;
            faction.team = team;
            game->world().add<FactionComponent>(e, faction);
            PerceptionComponent senses;
            senses.sight_range = 0.0f;
            senses.hearing_range = 0.0f;
            senses.touch_range = 0.0f;
            senses.pain = false;
            game->world().add<PerceptionComponent>(e, senses);
            SquadComponent squad;
            squad.token = token;
            squad.radio = radio;
            game->world().add<SquadComponent>(e, squad);
            return e;
        };

        // The observer can see; it is the only member with working senses.
        const ecs::Entity observer = add_world_entity(*game, "Observer", 1.0f, 0.0f, 1.0f, false);
        add_health(*game, observer, 100.0f);
        // The default forward is -Z; turn the observer to face the contact at +Z.
        if (auto* ot = game->world().get<scene::Transform>(observer); ot != nullptr) {
            ot->rot_y = 180.0f;
        }
        FactionComponent blue;
        blue.team = "Blue";
        game->world().add<FactionComponent>(observer, blue);
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 60.0f;
        eyes.sight_gain = 2.0f;
        eyes.sight_linger = 3.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        game->world().add<PerceptionComponent>(observer, eyes);
        SquadComponent observer_squad;
        observer_squad.token = "guards";
        observer_squad.radio = 8.0f;
        game->world().add<SquadComponent>(observer, observer_squad);

        // The contact, five metres straight ahead.
        const ecs::Entity target = add_world_entity(*game, "Contact", 1.0f, 0.0f, 6.0f, false);
        add_health(*game, target, 100.0f);
        FactionComponent red;
        red.team = "Red";
        game->world().add<FactionComponent>(target, red);

        // Same token, inside both radios: receives.
        const ecs::Entity mate = make_radio("Mate", 6.0f, 1.0f, "Blue", "guards", 8.0f);
        // Same token, but the observer cannot reach it: does not receive.
        const ecs::Entity far = make_radio("Far", 20.0f, 1.0f, "Blue", "guards", 8.0f);
        // Token comparison is case-sensitive: does not receive.
        const ecs::Entity stranger = make_radio("Stranger", 6.0f, 1.0f, "Blue", "Guards", 8.0f);
        // Same token inside the observer's reach, but its own antenna is short:
        // a delivery needs BOTH radios to reach.
        const ecs::Entity short_radio =
            make_radio("Short", 6.0f, 3.0f, "Blue", "guards", 2.0f);
        // A friendly target is not a contact to share.
        const ecs::Entity friendly = add_world_entity(*game, "Friendly", 1.5f, 0.0f, 4.0f, false);
        add_health(*game, friendly, 100.0f);
        game->world().add<FactionComponent>(friendly, blue);
        // A corpse is not a contact, the same rule the nav gate keeps.
        const ecs::Entity corpse = add_world_entity(*game, "Corpse", 1.0f, 0.0f, 8.0f, false);
        HealthComponent dead;
        dead.max_health = 100.0f;
        dead.health = 0.0f;
        game->world().add<HealthComponent>(corpse, dead);
        game->world().add<FactionComponent>(corpse, red);
        // Even on a faction-less scene, a same-token squadmate is never a
        // contact: reporting one would make the squad chase itself.
        const ecs::Entity squadmate =
            add_world_entity(*game, "Squadmate", 1.0f, 0.0f, 3.0f, false);
        add_health(*game, squadmate, 100.0f);
        game->world().add<FactionComponent>(squadmate, red);
        SquadComponent another;
        another.token = "guards";
        game->world().add<SquadComponent>(squadmate, another);

        rt.adopt_scene(std::move(game), "content://Scenes/RadioNet.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        // The observer really did see the contact, and the mate really did
        // receive it from the radio rather than from a sense of its own.
        NF_CHECK(rt.entity_aware_of(observer, target));
        NF_CHECK(rt.entity_aware_of(mate, target));
        Vec3 known{};
        NF_CHECK(rt.entity_last_known(mate, target, known));
        NF_CHECK_NEAR(known.x, 1.0f, 1e-4f);
        NF_CHECK_NEAR(known.z, 6.0f, 1e-4f);
        NF_CHECK(!rt.entity_sensing(mate, target));

        const ai::Perception* mate_senses = rt.perception_of(mate);
        NF_CHECK(mate_senses != nullptr);
        if (mate_senses != nullptr) {
            NF_CHECK_EQ(mate_senses->perceived().size(), static_cast<size_t>(1));
            const ai::PerceivedTarget* memory =
                mate_senses->perceived().empty() ? nullptr : &mate_senses->perceived().front();
            NF_CHECK(memory != nullptr);
            if (memory != nullptr) {
                NF_CHECK(!memory->sensed);
                NF_CHECK(!memory->personally_perceived);
                for (u32 k = 0; k < ai::kStimulusKindCount; ++k) {
                    NF_CHECK_NEAR(memory->detection[k], 0.0f, 1e-6f);
                }
            }
        }

        // Token (case), reach (both ends) and every filtered target stay out.
        NF_CHECK(!rt.entity_aware_of(stranger, target));
        NF_CHECK(!rt.entity_aware_of(far, target));
        NF_CHECK(!rt.entity_aware_of(short_radio, target));
        NF_CHECK(!rt.entity_aware_of(mate, friendly));
        NF_CHECK(!rt.entity_aware_of(mate, corpse));
        NF_CHECK(!rt.entity_aware_of(mate, squadmate));

        // Anti-relay: the stranger stands five metres from the mate and ten from
        // the observer, so only a SECOND hop could reach it. The report is
        // awareness, not a sense, so the mate has nothing to relay.
        const ai::Perception* stranger_senses = rt.perception_of(stranger);
        NF_CHECK(stranger_senses != nullptr);
        if (stranger_senses != nullptr) {
            NF_CHECK(stranger_senses->perceived().empty());
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

// --- directional antennas (Phase 71) -----------------------------------------
//
// A cone is a delivery gate, not a new kind of knowledge: if either antenna is
// not facing the other end the report does not arrive, and once both face the
// same awareness-only report arrives. The default `cone=0` is the Phase 70
// radio, so this is purely opt-in.

NF_TEST(squad_radio_cone_needs_both_antennas_to_face) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_squad_cone";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("ConeNet");
        const auto make_radio = [&](const char* name, f32 x, f32 z, f32 cone) {
            const ecs::Entity e = add_world_entity(*game, name, x, 0.0f, z, false);
            add_health(*game, e, 100.0f);
            FactionComponent faction;
            faction.team = "Blue";
            game->world().add<FactionComponent>(e, faction);
            PerceptionComponent senses;
            senses.sight_range = 0.0f;
            senses.hearing_range = 0.0f;
            senses.touch_range = 0.0f;
            senses.pain = false;
            game->world().add<PerceptionComponent>(e, senses);
            SquadComponent squad;
            squad.token = "guards";
            squad.radio = 12.0f;
            squad.cone = cone;
            game->world().add<SquadComponent>(e, squad);
            return e;
        };

        // The observer looks at +Z (the default forward is -Z).
        const ecs::Entity observer = add_world_entity(*game, "Observer", 1.0f, 0.0f, 1.0f,
                                                      false);
        add_health(*game, observer, 100.0f);
        if (auto* ot = game->world().get<scene::Transform>(observer); ot != nullptr) {
            ot->rot_y = 180.0f;
        }
        FactionComponent blue;
        blue.team = "Blue";
        game->world().add<FactionComponent>(observer, blue);
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 60.0f;
        eyes.sight_gain = 2.0f;
        eyes.sight_linger = 3.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        game->world().add<PerceptionComponent>(observer, eyes);
        SquadComponent observer_squad;
        observer_squad.token = "guards";
        observer_squad.radio = 12.0f;
        observer_squad.cone = 45.0f;
        game->world().add<SquadComponent>(observer, observer_squad);

        const ecs::Entity target = add_world_entity(*game, "Contact", 1.0f, 0.0f, 6.0f,
                                                    false);
        add_health(*game, target, 100.0f);
        FactionComponent red;
        red.team = "Red";
        game->world().add<FactionComponent>(target, red);

        // In front of the observer and omnidirectional: receives.
        const ecs::Entity clear = make_radio("Clear", 2.0f, 3.0f, 0.0f);
        // Behind the directional observer: the reporter's cone rejects it.
        const ecs::Entity behind = make_radio("Behind", 0.0f, 0.0f, 0.0f);
        // In front of the observer but facing +Z, away from the reporter: the
        // RECEIVER's cone rejects it until it turns around.
        const ecs::Entity picky = make_radio("Picky", 2.5f, 3.0f, 45.0f);
        if (auto* pt = game->world().get<scene::Transform>(picky); pt != nullptr) {
            pt->rot_y = 180.0f;
        }

        rt.adopt_scene(std::move(game), "content://Scenes/ConeNet.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        NF_CHECK(rt.entity_aware_of(clear, target));
        NF_CHECK(!rt.entity_aware_of(behind, target));
        NF_CHECK(!rt.entity_aware_of(picky, target));

        // Moving the behind mate into the observer's beam delivers the next
        // report without touching the observer's facing.
        if (auto* bt = rt.edit_scene()->world().get<scene::Transform>(behind);
            bt != nullptr) {
            bt->local_x = 0.5f;
            bt->world_x = 0.5f;
            bt->local_z = 3.0f;
            bt->world_z = 3.0f;
        }
        // Turning the picky receiver to face the reporter opens its side.
        if (auto* pt = rt.edit_scene()->world().get<scene::Transform>(picky);
            pt != nullptr) {
            pt->rot_y = 0.0f;
        }
        for (int frame = 0; frame < 5; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        NF_CHECK(rt.entity_aware_of(behind, target));
        NF_CHECK(rt.entity_aware_of(picky, target));

        // A received report is still knowledge, never a sense of its own.
        NF_CHECK(!rt.entity_sensing(clear, target));
        NF_CHECK(!rt.entity_sensing(behind, target));
        NF_CHECK(!rt.entity_sensing(picky, target));
        const ai::Perception* picky_senses = rt.perception_of(picky);
        NF_CHECK(picky_senses != nullptr);
        if (picky_senses != nullptr && !picky_senses->perceived().empty()) {
            NF_CHECK(!picky_senses->perceived().front().personally_perceived);
            for (u32 k = 0; k < ai::kStimulusKindCount; ++k) {
                NF_CHECK_NEAR(picky_senses->perceived().front().detection[k], 0.0f, 1e-6f);
            }
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(squad_lost_callout_respects_the_cone) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_squad_cone_lost";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    constexpr f32 dt = 1.0f / 60.0f;
    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("ConeLostNet");
        const ecs::Entity observer = add_world_entity(*game, "Observer", 1.0f, 0.0f, 1.0f,
                                                      false);
        add_health(*game, observer, 100.0f);
        if (auto* ot = game->world().get<scene::Transform>(observer); ot != nullptr) {
            ot->rot_y = 180.0f; // looks at +Z, where the mate also stands
        }
        FactionComponent blue;
        blue.team = "Blue";
        game->world().add<FactionComponent>(observer, blue);
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 60.0f;
        eyes.sight_gain = 2.0f;
        eyes.sight_linger = 3.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        game->world().add<PerceptionComponent>(observer, eyes);
        SquadComponent observer_squad;
        observer_squad.token = "guards";
        observer_squad.radio = 8.0f;
        observer_squad.lost = 2.0f;
        observer_squad.cone = 45.0f;
        game->world().add<SquadComponent>(observer, observer_squad);

        const ecs::Entity mate = add_world_entity(*game, "Mate", 2.0f, 0.0f, 3.0f, false);
        add_health(*game, mate, 100.0f);
        game->world().add<FactionComponent>(mate, blue);
        PerceptionComponent mate_senses;
        mate_senses.sight_range = 0.0f;
        mate_senses.hearing_range = 0.0f;
        mate_senses.touch_range = 0.0f;
        mate_senses.pain = false;
        // A 0.5 s window makes a missing callout visible: only the callout can
        // keep the mate aware a full second after the last live report.
        mate_senses.sight_linger = 0.5f;
        game->world().add<PerceptionComponent>(mate, mate_senses);
        SquadComponent mate_squad;
        mate_squad.token = "guards";
        mate_squad.radio = 8.0f;
        mate_squad.lost = 2.0f;
        game->world().add<SquadComponent>(mate, mate_squad);

        const ecs::Entity target = add_world_entity(*game, "Contact", 1.0f, 0.0f, 6.0f,
                                                    false);
        add_health(*game, target, 100.0f);
        FactionComponent red;
        red.team = "Red";
        game->world().add<FactionComponent>(target, red);

        rt.adopt_scene(std::move(game), "content://Scenes/ConeLostNet.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(dt);
        }
        NF_CHECK(rt.entity_aware_of(mate, target));

        // Turn the antenna around: the target leaves the observer's sight and
        // the mate now sits behind the same cone. The loss is noticed, but the
        // one callout is shouted at nobody.
        if (auto* ot = rt.edit_scene()->world().get<scene::Transform>(observer);
            ot != nullptr) {
            ot->rot_y = 0.0f;
        }
        for (int frame = 0; frame < 72; ++frame) {
            rt.update(dt);
        }
        NF_CHECK(!rt.entity_aware_of(mate, target));

        // Face the mate and the target again. A fresh sighting restores the
        // live report; when it is lost again the callout goes into the cone and
        // the receiver's own lost window keeps it.
        if (auto* ot = rt.edit_scene()->world().get<scene::Transform>(observer);
            ot != nullptr) {
            ot->rot_y = 180.0f;
        }
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(dt);
        }
        NF_CHECK(rt.entity_aware_of(mate, target));

        if (auto* tt = rt.edit_scene()->world().get<scene::Transform>(target);
            tt != nullptr) {
            tt->local_z = -100.0f;
            tt->world_z = -100.0f;
        }
        rt.update(dt); // notices the loss and queues the callout
        rt.update(dt); // the delivery, now inside both cones
        for (int frame = 0; frame < 72; ++frame) {
            rt.update(dt);
        }

        // Past the live report's 0.5 s window, still inside the 2 s callout
        // window: only a delivered callout can explain this awareness.
        NF_CHECK(rt.entity_aware_of(mate, target));
        const ai::Perception* after = rt.perception_of(mate);
        NF_CHECK(after != nullptr);
        if (after != nullptr && !after->perceived().empty()) {
            NF_CHECK(!after->perceived().front().sensed);
            NF_CHECK(!after->perceived().front().personally_perceived);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

// --- squad lost-contact callouts (Phase 69) -----------------------------------
//
// A live report says "I see them now". A lost callout says "I saw them THERE,
// and I have lost them" — exactly once. These cases pin the three consequences:
// it derives only from a personal sighting (the radio is still not a relay), it
// needs both radios to opt in, and its memory window is the RECEIVER's `lost`
// with that receiver's own linger as the floor.

NF_TEST(squad_lost_callout_uses_the_last_position_once) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_squad_lost";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    constexpr f32 dt = 1.0f / 60.0f;
    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("LostNet");
        const auto make_radio = [&](const char* name, f32 x, f32 z, f32 lost,
                                    f32 sight_linger) {
            const ecs::Entity e = add_world_entity(*game, name, x, 0.0f, z, false);
            add_health(*game, e, 100.0f);
            FactionComponent faction;
            faction.team = "Blue";
            game->world().add<FactionComponent>(e, faction);
            PerceptionComponent senses;
            senses.sight_range = 0.0f;
            senses.hearing_range = 0.0f;
            senses.touch_range = 0.0f;
            senses.pain = false;
            senses.sight_linger = sight_linger;
            game->world().add<PerceptionComponent>(e, senses);
            SquadComponent squad;
            squad.token = "guards";
            squad.radio = 8.0f;
            squad.lost = lost;
            game->world().add<SquadComponent>(e, squad);
            return e;
        };

        // The observer is the only member with eyes. It faces +Z, where the
        // contact starts five metres ahead.
        const ecs::Entity observer = add_world_entity(*game, "Observer", 1.0f, 0.0f, 1.0f,
                                                      false);
        add_health(*game, observer, 100.0f);
        if (auto* ot = game->world().get<scene::Transform>(observer); ot != nullptr) {
            ot->rot_y = 180.0f;
        }
        FactionComponent blue;
        blue.team = "Blue";
        game->world().add<FactionComponent>(observer, blue);
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 60.0f;
        eyes.sight_gain = 2.0f;
        eyes.sight_linger = 3.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        game->world().add<PerceptionComponent>(observer, eyes);
        SquadComponent observer_squad;
        observer_squad.token = "guards";
        observer_squad.radio = 8.0f;
        observer_squad.lost = 2.0f;
        game->world().add<SquadComponent>(observer, observer_squad);

        // The mate opted in (lost=2) and has a SHORT sight linger, so the lost
        // window is distinguishable from a live report's memory. The second
        // mate did not opt in (lost=0) and must keep Phase 68 exactly.
        const ecs::Entity mate = make_radio("Mate", 6.0f, 1.0f, 2.0f, 0.5f);
        const ecs::Entity no_opt_in = make_radio("NoOptIn", 6.0f, 3.0f, 0.0f, 0.5f);

        const ecs::Entity target = add_world_entity(*game, "Contact", 1.0f, 0.0f, 6.0f,
                                                    false);
        add_health(*game, target, 100.0f);
        FactionComponent red;
        red.team = "Red";
        game->world().add<FactionComponent>(target, red);

        rt.adopt_scene(std::move(game), "content://Scenes/LostNet.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(dt);
        }

        // Both opted-in and opted-out mates got the LIVE report first: the
        // callout is an addition, not a replacement for Phase 68.
        NF_CHECK(rt.entity_aware_of(mate, target));
        NF_CHECK(rt.entity_aware_of(no_opt_in, target));
        NF_CHECK(!rt.entity_sensing(mate, target));

        // The contact vanishes from every sense but stays alive and in the cast.
        if (auto* tt = rt.edit_scene()->world().get<scene::Transform>(target); tt != nullptr) {
            tt->local_z = -100.0f;
            tt->world_z = -100.0f;
        }

        // One step notices the loss and queues it; the next exchange delivers it.
        rt.update(dt);
        rt.update(dt);

        Vec3 known{};
        NF_CHECK(rt.entity_last_known(mate, target, known));
        NF_CHECK_NEAR(known.x, 1.0f, 1e-4f);
        NF_CHECK_NEAR(known.z, 6.0f, 1e-4f); // the last sighting, never -100
        NF_CHECK(!rt.entity_sensing(mate, target));
        const ai::Perception* mate_senses = rt.perception_of(mate);
        NF_CHECK(mate_senses != nullptr);
        if (mate_senses != nullptr) {
            const ai::PerceivedTarget* memory =
                mate_senses->perceived().empty() ? nullptr : &mate_senses->perceived().front();
            NF_CHECK(memory != nullptr);
            if (memory != nullptr) {
                // The receiver's own lost=2 is the window, above its 0.5 linger.
                NF_CHECK_NEAR(memory->linger_deadline, 2.0f, 1e-5f);
                NF_CHECK(!memory->personally_perceived);
                NF_CHECK(!memory->sensed);
            }
        }
        const ai::Perception* no_opt_senses = rt.perception_of(no_opt_in);
        NF_CHECK(no_opt_senses != nullptr);
        if (no_opt_senses != nullptr) {
            const ai::PerceivedTarget* memory =
                no_opt_senses->perceived().empty() ? nullptr : &no_opt_senses->perceived().front();
            NF_CHECK(memory != nullptr);
            if (memory != nullptr) {
                // An opted-out receiver got only the Phase 68 live window.
                NF_CHECK_NEAR(memory->linger_deadline, 0.5f, 1e-5f);
            }
        }

        // One-shot: after the receiver's own 2 s window nothing refreshes it,
        // even though the reporter is still alive and its radio still works.
        for (int frame = 0; frame < 150; ++frame) {
            rt.update(dt);
        }
        NF_CHECK(!rt.entity_aware_of(mate, target));
        NF_CHECK(!rt.entity_aware_of(no_opt_in, target));
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

// --- radio jammers (Phase 70) -------------------------------------------------

NF_TEST(jammer_line_round_trips_every_key) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_jammer_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    scene::Scene scene("JammerScene");
    auto& world = scene.world();
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    JammerComponent jammer;
    jammer.radius = 9.5f;
    jammer.strength = 0.5f;
    jammer.enabled = false;
    world.add<JammerComponent>(e, jammer);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/jammer.nfscene", scene, err));
    NF_CHECK(err.empty());

    const auto text = vfs.read_text("content://Scenes/jammer.nfscene");
    NF_CHECK(text.ok);
    NF_CHECK(text.value.find("Jammer: radius=9.5 strength=0.5 enabled=false") !=
             std::string::npos);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/jammer.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const auto* loaded = result.scene->world().get<JammerComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK_NEAR(loaded->radius, 9.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->strength, 0.5f, 1e-5f);
        NF_CHECK(!loaded->enabled);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(jammer_line_defaults_and_refuses_bad_values_by_name) {
    // A scene that never authored a jammer writes no line, so every older
    // scene keeps its exact Phase 69 radio behaviour.
    scene::Scene plain("PlainJammerScene");
    plain.world().add<scene::Transform>(plain.world().create_entity(), scene::Transform{});
    const std::string plain_text = serialize_scene_to_text(plain);
    NF_CHECK(plain_text.find("Jammer:") == std::string::npos);

    // A bare line is a real field: the defaults are 12 m at full strength,
    // because an attached field that did nothing would be a trap.
    const std::string minimal =
        "# SANAD Scene v1\nversion: 1\nname: MinimalJammer\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Jammer\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
        "parent(4294967295:0)\n"
        "  Jammer:\n";
    auto parsed = load_scene_from_text(minimal);
    NF_CHECK(parsed.success);
    if (parsed.scene) {
        const auto* jammer = parsed.scene->world().get<JammerComponent>(
            parsed.scene->world().all_entities()[0]);
        NF_CHECK(jammer != nullptr);
        if (jammer != nullptr) {
            NF_CHECK_NEAR(jammer->radius, 12.0f, 1e-6f);
            NF_CHECK_NEAR(jammer->strength, 1.0f, 1e-6f);
            NF_CHECK(jammer->enabled);
        }
    }

    const auto refused = [](const std::string& line, const char* key) {
        const std::string text =
            "# SANAD Scene v1\nversion: 1\nname: BadJammer\nentity_count: 1\n"
            "---\nentity: 1:0\n  Name: Jammer\n"
            "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
            "parent(4294967295:0)\n  " + line + "\n";
        auto r = load_scene_from_text(text);
        if (!r.success || !r.scene) {
            return false;
        }
        const ecs::Entity e = r.scene->world().all_entities()[0];
        bool named = false;
        for (const std::string& warning : r.warnings) {
            if (warning.find("Jammer") != std::string::npos &&
                warning.find(key) != std::string::npos) {
                named = true;
                break;
            }
        }
        return r.scene->world().get<JammerComponent>(e) == nullptr && named;
    };

    // The loader's bounds are the editor's and the validator's.
    NF_CHECK(refused("Jammer: radius=20000", "radius"));
    NF_CHECK(refused("Jammer: radius=-1", "radius"));
    NF_CHECK(refused("Jammer: strength=1.5", "strength"));
    NF_CHECK(refused("Jammer: strength=-0.1", "strength"));

    // Zero is legal: it is an authored field that does nothing, which the
    // validator names rather than the loader refusing the line.
    const std::string inert_text =
        "# SANAD Scene v1\nversion: 1\nname: InertJammer\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Jammer\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) "
        "parent(4294967295:0)\n  Jammer: radius=0 strength=0\n";
    auto inert = load_scene_from_text(inert_text);
    NF_CHECK(inert.success);
    if (inert.scene) {
        const auto* jammer = inert.scene->world().get<JammerComponent>(
            inert.scene->world().all_entities()[0]);
        NF_CHECK(jammer != nullptr);
        if (jammer != nullptr) {
            NF_CHECK_NEAR(jammer->radius, 0.0f, 1e-6f);
            NF_CHECK_NEAR(jammer->strength, 0.0f, 1e-6f);
        }
    }
}

NF_TEST(jammer_values_that_do_nothing_are_warned_by_name) {
    scene::Scene scene("JammerWarn");
    auto& world = scene.world();
    add_bare_camera(scene);
    const ecs::Entity e = add_world_entity(scene, "Jammer", 0.0f, 0.0f, 0.0f, false);
    JammerComponent jammer;
    jammer.radius = 0.0f;
    jammer.strength = 0.0f;
    world.add<JammerComponent>(e, jammer);

    const ValidationReport report = validate_scene(scene, "content://Scenes/jammer_warn.nfscene");
    bool radius_zero = false;
    bool strength_zero = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("radius=0") != std::string::npos) radius_zero = true;
        if (issue.message.find("strength=0") != std::string::npos) strength_zero = true;
    }
    NF_CHECK(radius_zero);
    NF_CHECK(strength_zero);

    // Out-of-range values are reported as outside the bounds, not as "inert":
    // the field is not doing nothing, it is doing something undefined.
    JammerComponent outside;
    outside.radius = 20000.0f;
    outside.strength = 2.0f;
    world.add<JammerComponent>(e, outside);
    const ValidationReport bad =
        validate_scene(scene, "content://Scenes/jammer_bad.nfscene");
    bool radius_out = false;
    bool strength_out = false;
    for (const ValidationIssue& issue : bad.issues) {
        if (issue.message.find("radius") != std::string::npos &&
            issue.message.find("outside") != std::string::npos) {
            radius_out = true;
        }
        if (issue.message.find("strength") != std::string::npos &&
            issue.message.find("outside") != std::string::npos) {
            strength_out = true;
        }
    }
    NF_CHECK(radius_out);
    NF_CHECK(strength_out);

    // A field with no Transform has no position, and a good field is silent.
    const ecs::Entity floating = world.create_entity();
    world.add<scene::NameComponent>(floating, scene::NameComponent{"FloatingJammer"});
    world.add<JammerComponent>(floating, JammerComponent{});
    const ValidationReport no_transform =
        validate_scene(scene, "content://Scenes/jammer_float.nfscene");
    bool missing_transform = false;
    for (const ValidationIssue& issue : no_transform.issues) {
        if (issue.message.find("Jammer") != std::string::npos &&
            issue.message.find("Transform") != std::string::npos) {
            missing_transform = true;
        }
    }
    NF_CHECK(missing_transform);

    scene::Scene good("JammerGood");
    add_bare_camera(good);
    const ecs::Entity g = add_world_entity(good, "Jammer", 0.0f, 0.0f, 0.0f, false);
    JammerComponent fine;
    fine.radius = 9.0f;
    fine.strength = 0.5f;
    good.world().add<JammerComponent>(g, fine);
    const ValidationReport quiet =
        validate_scene(good, "content://Scenes/jammer_good.nfscene");
    bool jammer_issue = false;
    for (const ValidationIssue& issue : quiet.issues) {
        if (issue.message.find("Jammer") != std::string::npos) jammer_issue = true;
    }
    NF_CHECK(!jammer_issue);
}

NF_TEST(jammer_cuts_live_reports_at_the_receiving_antenna) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_jammer_live";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("JammerNet");
        const auto make_radio = [&](const char* name, f32 x, f32 z) {
            const ecs::Entity e = add_world_entity(*game, name, x, 0.0f, z, false);
            add_health(*game, e, 100.0f);
            FactionComponent faction;
            faction.team = "Blue";
            game->world().add<FactionComponent>(e, faction);
            PerceptionComponent senses;
            senses.sight_range = 0.0f;
            senses.hearing_range = 0.0f;
            senses.touch_range = 0.0f;
            senses.pain = false;
            game->world().add<PerceptionComponent>(e, senses);
            SquadComponent squad;
            squad.token = "guards";
            squad.radio = 8.0f;
            game->world().add<SquadComponent>(e, squad);
            return e;
        };
        const auto add_field = [&](f32 x, f32 z, f32 radius, f32 strength) {
            const ecs::Entity e = add_world_entity(*game, "Jammer", x, 0.0f, z, false);
            JammerComponent jammer;
            jammer.radius = radius;
            jammer.strength = strength;
            game->world().add<JammerComponent>(e, jammer);
            return e;
        };

        const ecs::Entity observer = add_world_entity(*game, "Observer", 1.0f, 0.0f, 1.0f, false);
        add_health(*game, observer, 100.0f);
        if (auto* ot = game->world().get<scene::Transform>(observer); ot != nullptr) {
            ot->rot_y = 180.0f;
        }
        FactionComponent blue;
        blue.team = "Blue";
        game->world().add<FactionComponent>(observer, blue);
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 60.0f;
        eyes.sight_gain = 2.0f;
        eyes.sight_linger = 3.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        game->world().add<PerceptionComponent>(observer, eyes);
        SquadComponent observer_squad;
        observer_squad.token = "guards";
        observer_squad.radio = 8.0f;
        game->world().add<SquadComponent>(observer, observer_squad);

        const ecs::Entity target = add_world_entity(*game, "Contact", 1.0f, 0.0f, 6.0f, false);
        add_health(*game, target, 100.0f);
        FactionComponent red;
        red.team = "Red";
        game->world().add<FactionComponent>(target, red);

        // Five metres from the observer, both antennas full: receives.
        const ecs::Entity clear = make_radio("Clear", 6.0f, 1.0f);
        // Same distance, but a receiver-side field leaves 2 m of antenna:
        // a delivery needs the receiver to reach as well, so it stays blind.
        const ecs::Entity rx_jammed = make_radio("RxJammed", 6.0f, 3.0f);
        add_field(6.0f, 3.0f, 1.0f, 0.75f);
        // 3.6 m away under TWO 50% fields: the strongest field wins, so the
        // antenna is 4 m and the link arrives. If fields stacked it would be
        // 2 m and the same link would fail — this pins "no stacking".
        const ecs::Entity stacked = make_radio("Stacked", 4.5f, 2.0f);
        add_field(4.5f, 2.0f, 1.5f, 0.5f);
        add_field(4.5f, 2.0f, 1.5f, 0.5f);

        rt.adopt_scene(std::move(game), "content://Scenes/JammerNet.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(1.0f / 60.0f);
        }

        NF_CHECK(rt.entity_aware_of(clear, target));
        NF_CHECK(rt.entity_aware_of(stacked, target));
        NF_CHECK(!rt.entity_aware_of(rx_jammed, target));
        NF_CHECK(!rt.entity_sensing(clear, target));

        // The link is not one-way through the sender: the same field over the
        // reporter would have cut the clear mate too. Jamming is team-agnostic,
        // so a field over a Blue member is not a friendly buff.
        const auto* rx_senses = rt.perception_of(rx_jammed);
        NF_CHECK(rx_senses != nullptr);
        if (rx_senses != nullptr) {
            NF_CHECK(rx_senses->perceived().empty());
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(jammer_cuts_lost_callouts_at_the_reporting_antenna) {
    auto device = rhi::create_device();
    if (!device) {
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        NF_SKIP("headless Vulkan device init failed");
    }

    auto tmp = nf::test::temp_root() / "nf_jammer_lost";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);

    constexpr f32 dt = 1.0f / 60.0f;
    {
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        auto game = std::make_unique<scene::Scene>("JammedLostNet");
        const ecs::Entity observer = add_world_entity(*game, "Observer", 1.0f, 0.0f, 1.0f,
                                                      false);
        add_health(*game, observer, 100.0f);
        if (auto* ot = game->world().get<scene::Transform>(observer); ot != nullptr) {
            ot->rot_y = 180.0f;
        }
        FactionComponent blue;
        blue.team = "Blue";
        game->world().add<FactionComponent>(observer, blue);
        PerceptionComponent eyes;
        eyes.sight_range = 30.0f;
        eyes.sight_half_fov_deg = 60.0f;
        eyes.sight_gain = 2.0f;
        eyes.sight_linger = 3.0f;
        eyes.hearing_range = 0.0f;
        eyes.touch_range = 0.0f;
        eyes.pain = false;
        game->world().add<PerceptionComponent>(observer, eyes);
        SquadComponent observer_squad;
        observer_squad.token = "guards";
        observer_squad.radio = 8.0f;
        observer_squad.lost = 2.0f;
        game->world().add<SquadComponent>(observer, observer_squad);

        const ecs::Entity mate = add_world_entity(*game, "Mate", 6.0f, 0.0f, 1.0f, false);
        add_health(*game, mate, 100.0f);
        game->world().add<FactionComponent>(mate, blue);
        PerceptionComponent mate_senses;
        mate_senses.sight_range = 0.0f;
        mate_senses.hearing_range = 0.0f;
        mate_senses.touch_range = 0.0f;
        mate_senses.pain = false;
        // A short linger makes the suppressed callout visible: the receiver
        // forgets the live report after half a second unless the lost window
        // refreshes it, and that refresh is precisely what jamming prevents.
        mate_senses.sight_linger = 0.5f;
        game->world().add<PerceptionComponent>(mate, mate_senses);
        SquadComponent mate_squad;
        mate_squad.token = "guards";
        mate_squad.radio = 8.0f;
        mate_squad.lost = 2.0f;
        game->world().add<SquadComponent>(mate, mate_squad);

        const ecs::Entity target = add_world_entity(*game, "Contact", 1.0f, 0.0f, 6.0f,
                                                    false);
        add_health(*game, target, 100.0f);
        FactionComponent red;
        red.team = "Red";
        game->world().add<FactionComponent>(target, red);

        // The field sits on the REPORTER and starts off, so the live report
        // arrives first; enabling it cuts the antenna only for the callout.
        const ecs::Entity tx_field =
            add_world_entity(*game, "Jammer", 1.0f, 0.0f, 1.0f, false);
        JammerComponent jammer;
        jammer.radius = 1.0f;
        jammer.strength = 0.75f;
        jammer.enabled = false;
        game->world().add<JammerComponent>(tx_field, jammer);

        rt.adopt_scene(std::move(game), "content://Scenes/JammedLostNet.nfscene");
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(dt);
        }

        NF_CHECK(rt.entity_aware_of(mate, target));
        const ai::Perception* mate_senses_live = rt.perception_of(mate);
        NF_CHECK(mate_senses_live != nullptr);
        float live_deadline = 0.0f;
        if (mate_senses_live != nullptr && !mate_senses_live->perceived().empty()) {
            live_deadline = mate_senses_live->perceived().front().linger_deadline;
        }

        // Silence the reporter's antenna, then make the contact vanish. The
        // observer still sees the loss (jamming is radio-only): it queues the
        // callout, but a 2 m antenna cannot shout 5.4 m.
        if (auto* field = rt.edit_scene()->world().get<JammerComponent>(tx_field);
            field != nullptr) {
            field->enabled = true;
        }
        if (auto* tt = rt.edit_scene()->world().get<scene::Transform>(target); tt != nullptr) {
            tt->local_z = -100.0f;
            tt->world_z = -100.0f;
        }
        rt.update(dt); // notices the loss and queues the callout
        rt.update(dt); // the failed delivery attempt

        const ai::Perception* after_jam = rt.perception_of(mate);
        NF_CHECK(after_jam != nullptr);
        if (after_jam != nullptr && !after_jam->perceived().empty()) {
            // No refresh: the live report's 0.5 s deadline is untouched.
            NF_CHECK_NEAR(after_jam->perceived().front().linger_deadline, live_deadline, 1e-5f);
        }

        // Past the live window and well inside the 2 s lost window, the mate is
        // still deaf: the callout never arrived.
        for (int frame = 0; frame < 72; ++frame) {
            rt.update(dt);
        }
        NF_CHECK(!rt.entity_aware_of(mate, target));

        // Clearing the field restores the link: a fresh sighting delivers the
        // live report again, and the following loss extends the window to the
        // receiver's own lost=2.
        if (auto* field = rt.edit_scene()->world().get<JammerComponent>(tx_field);
            field != nullptr) {
            field->enabled = false;
        }
        if (auto* tt = rt.edit_scene()->world().get<scene::Transform>(target); tt != nullptr) {
            tt->local_z = 6.0f;
            tt->world_z = 6.0f;
        }
        for (int frame = 0; frame < 60; ++frame) {
            rt.update(dt);
        }
        NF_CHECK(rt.entity_aware_of(mate, target));
        const ai::Perception* after_clear = rt.perception_of(mate);
        NF_CHECK(after_clear != nullptr);
        float clear_deadline = 0.0f;
        if (after_clear != nullptr && !after_clear->perceived().empty()) {
            clear_deadline = after_clear->perceived().front().linger_deadline;
        }

        if (auto* tt = rt.edit_scene()->world().get<scene::Transform>(target); tt != nullptr) {
            tt->local_z = -100.0f;
            tt->world_z = -100.0f;
        }
        rt.update(dt);
        rt.update(dt);

        const ai::Perception* refreshed = rt.perception_of(mate);
        NF_CHECK(refreshed != nullptr);
        if (refreshed != nullptr && !refreshed->perceived().empty()) {
            // The lost window is 2 s, so the deadline moved at least ~1.5 s
            // past the live report's 0.5 s window.
            NF_CHECK(refreshed->perceived().front().linger_deadline >
                      clear_deadline + 1.0f);
            NF_CHECK(!refreshed->perceived().front().personally_perceived);
            NF_CHECK(!refreshed->perceived().front().sensed);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
