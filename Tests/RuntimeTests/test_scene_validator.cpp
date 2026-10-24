// Tests/RuntimeTests/test_scene_validator.cpp — the rules a scene must satisfy
// before it is played (Phase 43).
//
// The loader refuses a line it cannot parse; the editor refuses a value its
// command rejects. Neither can see a problem that only exists BETWEEN
// entities — a duplicate name, a camera following a name nothing has, a render
// scale the renderer will reject. Those are the failures that cost an hour to
// find by playing, so this suite pins them one rule at a time.
//
// Every case builds a scene in memory. No device, no files, no clock.

#include <NF/Physics/Components.hpp>
#include <NF/Runtime/SceneValidator.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Test/TestFramework.hpp>

#include <string>

using namespace nf;
using namespace nf::runtime;

namespace {

bool has_issue(const ValidationReport& r, ValidationSeverity severity, std::string_view needle) {
    for (const auto& i : r.issues) {
        if (i.severity == severity && i.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A scene that passes every rule: one named player, one camera following it,
/// one post-process block with defaults.
struct CleanScene {
    scene::Scene scene{"Clean"};

    CleanScene() {
        ecs::World& w = scene.world();
        const ecs::Entity player = w.create_entity();
        w.add<scene::NameComponent>(player, scene::NameComponent{"Player"});
        w.add<scene::Transform>(player, scene::Transform{});

        const ecs::Entity cam = w.create_entity();
        w.add<scene::NameComponent>(cam, scene::NameComponent{"Camera"});
        CameraComponent c;
        c.is_active = true;
        c.follow_target = "Player";
        w.add<CameraComponent>(cam, c);
        w.add<scene::Transform>(cam, scene::Transform{});

        const ecs::Entity pp = w.create_entity();
        w.add<scene::NameComponent>(pp, scene::NameComponent{"PostProcess"});
        w.add<PostProcessComponent>(pp, PostProcessComponent{});
    }
};

} // namespace

NF_TEST(a_clean_scene_reports_nothing) {
    const CleanScene s;
    const ValidationReport r = validate_scene(s.scene, "content://Scenes/clean.nfscene");
    NF_CHECK(r.ok());
    NF_CHECK(r.issues.empty());
    NF_CHECK(r.errors == 0u);
    NF_CHECK(r.warnings == 0u);
    NF_CHECK(format_validation_report(r).find("no issues") != std::string::npos);
}

NF_TEST(a_duplicate_name_is_an_error_because_references_are_ambiguous) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    const ecs::Entity extra = w.create_entity();
    w.add<scene::NameComponent>(extra, scene::NameComponent{"Player"}); // second Player
    w.add<scene::Transform>(extra, scene::Transform{});

    const ValidationReport r = validate_scene(s.scene, "dup");
    NF_CHECK(!r.ok());
    NF_CHECK(has_issue(r, ValidationSeverity::Error, "2 entities"));
}

NF_TEST(a_camera_that_follows_a_name_nothing_has_is_an_error) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    for (const ecs::Entity e : w.all_entities()) {
        if (auto* cam = w.get<CameraComponent>(e); cam != nullptr) {
            cam->follow_target = "Nobody";
        }
    }
    const ValidationReport r = validate_scene(s.scene, "missing-follow");
    NF_CHECK(!r.ok());
    NF_CHECK(has_issue(r, ValidationSeverity::Error, "Nobody"));
}

NF_TEST(camera_numbers_that_would_break_the_orbit_are_warnings) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    for (const ecs::Entity e : w.all_entities()) {
        if (auto* cam = w.get<CameraComponent>(e); cam != nullptr) {
            cam->distance = 0.0f;
            cam->fov_y = 400.0f;
            cam->pitch_min = 30.0f;
            cam->pitch_max = -30.0f;
        }
    }
    const ValidationReport r = validate_scene(s.scene, "bad-camera");
    NF_CHECK(r.ok()); // warnings, not errors: it still runs
    NF_CHECK(has_issue(r, ValidationSeverity::Warning, "distance"));
    NF_CHECK(has_issue(r, ValidationSeverity::Warning, "fov"));
    NF_CHECK(has_issue(r, ValidationSeverity::Warning, "pitch_min"));
}

NF_TEST(a_scene_with_no_active_camera_says_so) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    for (const ecs::Entity e : w.all_entities()) {
        if (auto* cam = w.get<CameraComponent>(e); cam != nullptr) {
            cam->is_active = false;
        }
    }
    const ValidationReport r = validate_scene(s.scene, "no-camera");
    NF_CHECK(has_issue(r, ValidationSeverity::Warning, "no active camera"));
}

NF_TEST(post_process_bounds_the_renderer_would_reject_are_errors) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    for (const ecs::Entity e : w.all_entities()) {
        if (auto* pp = w.get<PostProcessComponent>(e); pp != nullptr) {
            pp->render_scale = 9.0f;      // far above kMaxRenderScale
            pp->min_render_scale = 8.0f;  // above the ceiling as well
            pp->target_fps = 5000.0f;     // above the target range
        }
    }
    const ValidationReport r = validate_scene(s.scene, "bad-post");
    NF_CHECK(!r.ok());
    NF_CHECK(has_issue(r, ValidationSeverity::Error, "render_scale"));
    NF_CHECK(has_issue(r, ValidationSeverity::Error, "min_render_scale"));
    NF_CHECK(has_issue(r, ValidationSeverity::Warning, "target_fps"));
}

NF_TEST(frame_policy_without_dynamic_resolution_is_worth_mentioning) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    for (const ecs::Entity e : w.all_entities()) {
        if (auto* pp = w.get<PostProcessComponent>(e); pp != nullptr) {
            pp->frame_policy = true;
            pp->dynamic_resolution = false;
        }
    }
    const ValidationReport r = validate_scene(s.scene, "policy-only");
    // Info, not error: the policy owns the scale on its own, so the scene is
    // legal — the author just probably meant to switch both on.
    NF_CHECK(r.ok());
    NF_CHECK(has_issue(r, ValidationSeverity::Info, "dynamic_resolution"));
}

NF_TEST(the_report_puts_errors_first_and_counts_them) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    // One error (duplicate name), one warning (bad fov).
    const ecs::Entity extra = w.create_entity();
    w.add<scene::NameComponent>(extra, scene::NameComponent{"Player"});
    for (const ecs::Entity e : w.all_entities()) {
        if (auto* cam = w.get<CameraComponent>(e); cam != nullptr) {
            cam->fov_y = 5.0f;
        }
    }

    const ValidationReport r = validate_scene(s.scene, "content://Scenes/mixed.nfscene");
    NF_CHECK(!r.ok());
    NF_CHECK(r.errors >= 1u);
    NF_CHECK(r.warnings >= 1u);
    NF_CHECK(!r.issues.empty());
    if (!r.issues.empty()) {
        NF_CHECK(r.issues.front().severity == ValidationSeverity::Error);
    }

    const std::string text = format_validation_report(r);
    NF_CHECK(text.find("mixed.nfscene") != std::string::npos);
    NF_CHECK(text.find("[error]") != std::string::npos);
    NF_CHECK(text.find("error(s)") != std::string::npos);
    // The error line precedes the warning line.
    NF_CHECK(text.find("[error]") < text.find("[warning]"));
}

// --- asset references --------------------------------------------------------

NF_TEST(missing_assets_are_errors_only_when_the_caller_can_answer) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    const ecs::Entity mesh = w.create_entity();
    w.add<scene::NameComponent>(mesh, scene::NameComponent{"Rock"});
    MeshComponent mc;
    mc.material = "content://Materials/rock.nfmat";
    w.add<MeshComponent>(mesh, mc);

    // No resolver: the asset rules are skipped, and the scene is still clean.
    // A validator that guessed would report a false error on every scene.
    const ValidationReport without = validate_scene(s.scene, "no-resolver");
    NF_CHECK(without.ok());

    // A resolver that has never heard of the file: an error naming the path.
    const ValidationReport missing = validate_scene(
        s.scene, "missing", [](std::string_view) { return false; });
    NF_CHECK(!missing.ok());
    NF_CHECK(has_issue(missing, ValidationSeverity::Error, "rock.nfmat"));

    // A resolver that knows it: clean again.
    const ValidationReport present = validate_scene(
        s.scene, "present", [](std::string_view path) {
            return path == "content://Materials/rock.nfmat";
        });
    NF_CHECK(present.ok());
}

NF_TEST(a_bare_material_name_is_not_a_file_lookup) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    const ecs::Entity mesh = w.create_entity();
    MeshComponent mc;
    mc.material = "Default"; // a NAME, which the asset system resolves
    w.add<MeshComponent>(mesh, mc);

    // Even a resolver that denies everything must not flag a bare name: it is
    // not a path, and treating it as one would error on every hand-authored
    // scene in the repository.
    const ValidationReport r =
        validate_scene(s.scene, "bare-name", [](std::string_view) { return false; });
    NF_CHECK(r.ok());
}

NF_TEST(scripts_are_checked_for_a_path_or_a_source) {
    CleanScene s;
    ecs::World& w = s.scene.world();

    const ecs::Entity with_path = w.create_entity();
    w.add<scene::NameComponent>(with_path, scene::NameComponent{"Spin"});
    scripting::ScriptComponent sc;
    sc.path = "content://Scripts/spin.lua";
    w.add<scripting::ScriptComponent>(with_path, sc);

    const ecs::Entity empty_script = w.create_entity();
    w.add<scene::NameComponent>(empty_script, scene::NameComponent{"Idle"});
    w.add<scripting::ScriptComponent>(empty_script, scripting::ScriptComponent{});

    const ValidationReport r = validate_scene(
        s.scene, "scripts", [](std::string_view path) { return path == "content://Scripts/spin.lua"; });
    // The one with a resolvable path is fine; the enabled-but-empty one is a
    // warning, because it will never run anything.
    NF_CHECK(r.ok());
    NF_CHECK(has_issue(r, ValidationSeverity::Warning, "neither a path nor source"));

    // A path that does not resolve is an error, naming the file.
    const ValidationReport broken =
        validate_scene(s.scene, "scripts-broken", [](std::string_view) { return false; });
    NF_CHECK(!broken.ok());
    NF_CHECK(has_issue(broken, ValidationSeverity::Error, "spin.lua"));
}

NF_TEST(a_dynamic_body_without_mass_is_an_error) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    const ecs::Entity box = w.create_entity();
    w.add<scene::NameComponent>(box, scene::NameComponent{"Crate"});
    physics::RigidBodyComponent rb;
    rb.type = physics::BodyType::Dynamic;
    rb.mass = 0.0f;
    w.add<physics::RigidBodyComponent>(box, rb);

    const ValidationReport r = validate_scene(s.scene, "massless");
    NF_CHECK(!r.ok());
    NF_CHECK(has_issue(r, ValidationSeverity::Error, "mass"));

    // A static body may have any mass: it is not simulated, and refusing it
    // would flag every wall in every level.
    rb.type = physics::BodyType::Static;
    rb.mass = 0.0f;
    for (const ecs::Entity e : w.all_entities()) {
        if (auto* body = w.get<physics::RigidBodyComponent>(e); body != nullptr) {
            *body = rb;
        }
    }
    NF_CHECK(validate_scene(s.scene, "static").ok());
}

NF_TEST(the_json_report_is_valid_json_and_escapes_names) {
    CleanScene s;
    ecs::World& w = s.scene.world();
    // A name with a quote and a backslash: the two characters that break a
    // hand-rolled writer.
    const ecs::Entity tricky = w.create_entity();
    w.add<scene::NameComponent>(tricky, scene::NameComponent{"Player"});
    w.add<scene::Transform>(tricky, scene::Transform{});

    ValidationReport report;
    report.scene_path = "content://Scenes/a\"b\\c.nfscene";
    report.add(ValidationSeverity::Error, "Player \"one\"", "quote \" and slash \\");
    report.add(ValidationSeverity::Info, {}, "plain");

    const std::string json = format_validation_json(report);
    // Structure
    NF_CHECK(json.find("\"scene\":\"") != std::string::npos);
    NF_CHECK(json.find("\"errors\":1") != std::string::npos);
    NF_CHECK(json.find("\"infos\":1") != std::string::npos);
    NF_CHECK(json.find("\"issues\":[") != std::string::npos);
    NF_CHECK(json.back() == '\n');
    // Escaping: the quote inside the name is \" and the backslash is \\.
    NF_CHECK(json.find("a\\\"b\\\\c.nfscene") != std::string::npos);
    NF_CHECK(json.find("Player \\\"one\\\"") != std::string::npos);
    // Balanced braces is the cheapest sanity check a parser would also do.
    size_t open = 0;
    size_t close = 0;
    for (const char c : json) {
        open += (c == '{') ? 1 : 0;
        close += (c == '}') ? 1 : 0;
    }
    NF_CHECK(open == close);
}

// --- combat (Phase 44) -------------------------------------------------------
//
// The loader refuses a `Weapon:` line it cannot parse, so these rules exist for
// the other two ways a weapon reaches a scene (C++ and the API) and for the
// mistakes that only show up between fields.

NF_TEST(a_weapon_that_cannot_fire_is_an_error) {
    CleanScene clean;
    ecs::World& w = clean.scene.world();

    const ecs::Entity gun = w.create_entity();
    w.add<scene::NameComponent>(gun, scene::NameComponent{"Gun"});
    w.add<scene::Transform>(gun, scene::Transform{});
    WeaponProfile broken;
    broken.magazine = 0u;
    w.add<WeaponComponent>(gun, WeaponComponent{broken});

    const ValidationReport report = validate_scene(clean.scene, "content://Scenes/x.nfscene");
    NF_CHECK(has_issue(report, ValidationSeverity::Error, "cannot fire"));
    NF_CHECK(report.errors >= 1);
}

NF_TEST(a_weapon_no_camera_follows_is_reported_as_info) {
    CleanScene clean; // the camera follows "Player"
    ecs::World& w = clean.scene.world();

    const ecs::Entity npc = w.create_entity();
    w.add<scene::NameComponent>(npc, scene::NameComponent{"Bandit"});
    w.add<scene::Transform>(npc, scene::Transform{});
    w.add<WeaponComponent>(npc, WeaponComponent{WeaponProfile{}});

    const ValidationReport report = validate_scene(clean.scene, "content://Scenes/x.nfscene");
    NF_CHECK(has_issue(report, ValidationSeverity::Info, "no active camera follows"));
    // An NPC with a gun is not an error: the scene is still playable.
    NF_CHECK_EQ(report.errors, 0);
}

NF_TEST(a_weapon_the_camera_follows_is_not_reported) {
    CleanScene clean;
    ecs::World& w = clean.scene.world();

    ecs::Entity player = ecs::kInvalidEntity;
    for (ecs::Entity e : w.all_entities()) {
        const scene::NameComponent* n = w.get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Player") {
            player = e;
        }
    }
    NF_CHECK(player.valid());
    w.add<WeaponComponent>(player, WeaponComponent{WeaponProfile{}});

    const ValidationReport report = validate_scene(clean.scene, "content://Scenes/x.nfscene");
    NF_CHECK(!has_issue(report, ValidationSeverity::Info, "no active camera follows"));
    NF_CHECK_EQ(report.errors, 0);
}

NF_TEST(hit_points_above_the_ceiling_and_a_backwards_falloff_are_warnings) {
    CleanScene clean;
    ecs::World& w = clean.scene.world();

    const ecs::Entity actor = w.create_entity();
    w.add<scene::NameComponent>(actor, scene::NameComponent{"Actor"});
    w.add<scene::Transform>(actor, scene::Transform{});

    HealthComponent hurt;
    hurt.max_health = 50.0f;
    hurt.health = 80.0f;
    w.add<HealthComponent>(actor, hurt);

    WeaponProfile p;
    p.falloff_start = 40.0f;
    p.falloff_end = 10.0f;
    w.add<WeaponComponent>(actor, WeaponComponent{p});

    const ValidationReport report = validate_scene(clean.scene, "content://Scenes/x.nfscene");
    NF_CHECK(has_issue(report, ValidationSeverity::Warning, "above max_health"));
    NF_CHECK(has_issue(report, ValidationSeverity::Warning, "falloff_end is not past"));
}

NF_TEST(a_broken_health_block_is_an_error) {
    CleanScene clean;
    ecs::World& w = clean.scene.world();

    const ecs::Entity actor = w.create_entity();
    w.add<scene::NameComponent>(actor, scene::NameComponent{"Actor"});
    w.add<scene::Transform>(actor, scene::Transform{});
    HealthComponent broken;
    broken.max_health = 0.0f;
    broken.health = 10.0f;
    w.add<HealthComponent>(actor, broken);

    const ValidationReport report = validate_scene(clean.scene, "content://Scenes/x.nfscene");
    NF_CHECK(has_issue(report, ValidationSeverity::Error, "max_health"));
}

// --- combat agents (Phase 45) ------------------------------------------------

NF_TEST(a_combat_agent_targeting_a_missing_name_is_an_error) {
    CleanScene clean;
    ecs::World& w = clean.scene.world();

    const ecs::Entity npc = w.create_entity();
    w.add<scene::NameComponent>(npc, scene::NameComponent{"Bandit"});
    w.add<scene::Transform>(npc, scene::Transform{});
    w.add<WeaponComponent>(npc, WeaponComponent{WeaponProfile{}});
    CombatAgentComponent agent;
    agent.target = "Nobody";
    w.add<CombatAgentComponent>(npc, agent);

    const ValidationReport report = validate_scene(clean.scene, "content://Scenes/x.nfscene");
    NF_CHECK(has_issue(report, ValidationSeverity::Error, "not the name of any entity"));
}

NF_TEST(a_combat_agent_without_a_weapon_is_a_warning) {
    CleanScene clean;
    ecs::World& w = clean.scene.world();

    const ecs::Entity npc = w.create_entity();
    w.add<scene::NameComponent>(npc, scene::NameComponent{"Bandit"});
    w.add<scene::Transform>(npc, scene::Transform{});
    CombatAgentComponent agent;
    agent.target = "Player"; // the clean scene has one
    w.add<CombatAgentComponent>(npc, agent);

    const ValidationReport report = validate_scene(clean.scene, "content://Scenes/x.nfscene");
    NF_CHECK(has_issue(report, ValidationSeverity::Warning, "no weapon"));
    NF_CHECK_EQ(report.errors, 0);
}
