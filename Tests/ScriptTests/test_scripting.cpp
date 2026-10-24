// ScriptTests — Lua gameplay scripting: VM, sandboxing, entity bindings,
// per-entity ticking. No GPU, no window, no hardware.

#include <NF/Test/TestFramework.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::scripting;

// ---------------------------------------------------------------------------
// VM core
// ---------------------------------------------------------------------------

NF_TEST(script_factorial_through_lua_call) {
    LuaVM vm;
    NF_CHECK(vm.valid());
    std::string err;
    NF_CHECK(vm.run_string("function fact(n) if n <= 1 then return 1 end return n * fact(n-1) end",
                           "fact", &err));
    std::vector<double> results;
    NF_CHECK(vm.call_numbers("fact", {10.0}, results, 1, &err));
    NF_CHECK(results.size() == 1);
    NF_CHECK_NEAR(results[0], 3628800.0, 1e-6);
}

NF_TEST(script_eval_and_globals) {
    LuaVM vm;
    double out = 0;
    NF_CHECK(vm.eval_number("2 + 3 * 4", out, nullptr));
    NF_CHECK_NEAR(out, 14.0, 1e-9);
    NF_CHECK(vm.eval_number("nf.clamp(99, 0, 10)", out, nullptr));
    NF_CHECK_NEAR(out, 10.0, 1e-9);
    NF_CHECK(vm.eval_number("nf.lerp(10, 20, 0.25)", out, nullptr));
    NF_CHECK_NEAR(out, 12.5, 1e-9);
    vm.set_global_number("answer", 42.0);
    NF_CHECK(vm.eval_number("answer * 2", out, nullptr));
    NF_CHECK_NEAR(out, 84.0, 1e-9);
    double back = 0;
    NF_CHECK(vm.get_global_number("answer", back));
    NF_CHECK_NEAR(back, 42.0, 1e-9);
    NF_CHECK(!vm.get_global_number("no_such_global_xyz", back));
}

NF_TEST(script_syntax_error_reports_cleanly) {
    LuaVM vm;
    std::string err;
    NF_CHECK(!vm.run_string("function broken( ", "bad", &err));
    NF_CHECK(!err.empty());
    // The VM survives a bad chunk.
    NF_CHECK(vm.run_string("ok_marker = 123", "good", nullptr));
    double back = 0;
    NF_CHECK(vm.get_global_number("ok_marker", back));
    NF_CHECK_NEAR(back, 123.0, 1e-9);
}

NF_TEST(script_runtime_error_reports_cleanly) {
    LuaVM vm;
    std::string err;
    NF_CHECK(vm.run_string("function boom() error('kablam') end", "boom", nullptr));
    std::vector<double> results;
    NF_CHECK(!vm.call_numbers("boom", {}, results, 1, &err));
    NF_CHECK(err.find("kablam") != std::string::npos);
    NF_CHECK(!vm.call_numbers("missing_fn_xyz", {}, results, 1, &err));
    NF_CHECK(!err.empty());
}

NF_TEST(script_instruction_limit_aborts_loops) {
    LuaVM vm;
    vm.set_instruction_limit(20000);
    std::string err;
    NF_CHECK(vm.run_string("while true do end", "loop", &err) == false);
    NF_CHECK(err.find("instruction limit") != std::string::npos);
    // Unlimited by default: a bounded loop runs fine.
    LuaVM vm2;
    NF_CHECK(vm2.run_string("s = 0 for i = 1, 100000 do s = s + i end", "sum", &err));
    double back = 0;
    NF_CHECK(vm2.get_global_number("s", back));
    NF_CHECK_NEAR(back, 5000050000.0, 1.0);
}

NF_TEST(script_reads_previous_frame_impacts) {
    ScriptInput input;
    input.impact_count = []() { return 1u; };
    input.impact_at = [](u32 index, ScriptImpact& out) {
        if (index != 0u) {
            return false;
        }
        out.shooter_id = 7u;
        out.target_id = 9u;
        out.target_name = "Crate";
        out.x = 1.0f;
        out.y = 2.0f;
        out.z = 3.0f;
        out.damage = 12.5f;
        out.applied = 10.0f;
        out.hit_zone = "head";
        out.armor_absorbed = 2.5f;
        out.damage_type = "fire";
        out.blocked_by_faction = true;
        out.killed = false;
        out.splash = true;
        return true;
    };

    LuaVM vm;
    install_input_bindings(vm, &input);
    std::string err;
    NF_CHECK(vm.run_string(
        "count = nf.impact_count(); "
        "local i = nf.impact(1); "
        "name_ok = (i.name == 'Crate') and 1 or 0; "
        "target = i.target; "
        "damage = i.damage; "
        "zone_ok = (i.zone == 'head') and 1 or 0; "
        "armor_absorbed = i.armor_absorbed; "
        "type_ok = (i.damage_type == 'fire') and 1 or 0; "
        "splash = i.splash and 1 or 0; "
        "blocked = i.blocked and 1 or 0; "
        "missing = (nf.impact(2) == nil) and 1 or 0",
        "impacts", &err));

    double value = 0.0;
    NF_CHECK(vm.get_global_number("count", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("name_ok", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("target", value));
    NF_CHECK_NEAR(value, 9.0, 1e-9);
    NF_CHECK(vm.get_global_number("damage", value));
    NF_CHECK_NEAR(value, 12.5, 1e-9);
    NF_CHECK(vm.get_global_number("zone_ok", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("armor_absorbed", value));
    NF_CHECK_NEAR(value, 2.5, 1e-9);
    NF_CHECK(vm.get_global_number("type_ok", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("splash", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("blocked", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("missing", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
}

NF_TEST(script_reads_previous_frame_shots) {
    ScriptInput input;
    input.shot_count = []() { return 1u; };
    input.shot_at = [](u32 index, ScriptShot& out) {
        if (index != 0u) {
            return false;
        }
        out.shooter_id = 4u;
        out.x = 1.0f;
        out.y = 2.0f;
        out.z = 3.0f;
        out.dx = 0.0f;
        out.dy = 0.0f;
        out.dz = -1.0f;
        out.damage = 9.5f;
        out.pellet = 2u;
        out.pellets = 4u;
        out.seed = 77u;
        out.projectile = true;
        return true;
    };

    LuaVM vm;
    install_input_bindings(vm, &input);
    std::string err;
    NF_CHECK(vm.run_string(
        "count = nf.shot_count(); "
        "local s = nf.shot(1); "
        "shooter = s.shooter; "
        "pellet = s.pellet; "
        "pellets = s.pellets; "
        "projectile = s.projectile and 1 or 0; "
        "dir_z = s.dz; "
        "missing = (nf.shot(2) == nil) and 1 or 0",
        "shots", &err));

    double value = 0.0;
    NF_CHECK(vm.get_global_number("count", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("shooter", value));
    NF_CHECK_NEAR(value, 4.0, 1e-9);
    NF_CHECK(vm.get_global_number("pellet", value));
    NF_CHECK_NEAR(value, 2.0, 1e-9);
    NF_CHECK(vm.get_global_number("pellets", value));
    NF_CHECK_NEAR(value, 4.0, 1e-9);
    NF_CHECK(vm.get_global_number("projectile", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("dir_z", value));
    NF_CHECK_NEAR(value, -1.0, 1e-9);
    NF_CHECK(vm.get_global_number("missing", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
}

NF_TEST(script_applies_queries_and_clears_status_effects) {
    ScriptInput input;
    bool applied = false;
    bool cleared = false;
    f32 captured_dps = 0.0f;
    input.apply_status = [&](u32, u32, std::string_view kind, f32 dps, f32 seconds, f32 tick) {
        applied = kind == "burn" && seconds == 3.0f && tick == 0.5f;
        captured_dps = dps;
        return applied;
    };
    input.status_remaining = [](u32, u32, std::string_view kind) {
        return kind == "burn" ? 2.5f : 0.0f;
    };
    input.clear_status = [&](u32, u32, std::string_view kind) {
        cleared = kind == "burn";
        return cleared;
    };

    LuaVM vm;
    install_input_bindings(vm, &input);
    std::string err;
    NF_CHECK(vm.run_string(
        "applied = nf.apply_status(1, 0, 'burn', 4, 3, 0.5) and 1 or 0; "
        "remaining = nf.status_remaining(1, 0, 'burn'); "
        "cleared = nf.clear_status(1, 0, 'burn') and 1 or 0; "
        "unknown = nf.apply_status(1, 0, 'poison', 4, 3, 0.5) and 1 or 0",
        "status", &err));

    double value = 0.0;
    NF_CHECK(vm.get_global_number("applied", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK_NEAR(captured_dps, 4.0f, 1e-5f);
    NF_CHECK(vm.get_global_number("remaining", value));
    NF_CHECK_NEAR(value, 2.5, 1e-9);
    NF_CHECK(vm.get_global_number("cleared", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
    NF_CHECK(vm.get_global_number("unknown", value));
    NF_CHECK_NEAR(value, 0.0, 1e-9);
}

NF_TEST(script_reads_combat_memory_and_suppression) {
    ScriptInput input;
    input.last_attacker = [](u32 id, u32, u32& attacker, u32& gen) {
        if (id != 1u) {
            return false;
        }
        attacker = 7u;
        gen = 2u;
        return true;
    };
    input.killer = [](u32 id, u32, u32& killer, u32& gen) {
        if (id != 1u) {
            return false;
        }
        killer = 7u;
        gen = 2u;
        return true;
    };
    input.damage_taken = [](u32 id, u32, f32& health, f32& armor, u32& hits, f32& seconds) {
        if (id != 1u) {
            return;
        }
        health = 12.0f;
        armor = 3.0f;
        hits = 4u;
        seconds = 1.5f;
    };
    input.suppression = [](u32 id, u32) { return id == 1u ? 0.5f : 0.0f; };

    LuaVM vm;
    install_input_bindings(vm, &input);
    std::string err;
    NF_CHECK(vm.run_string(
        "attacker, attacker_gen = nf.last_attacker(1, 0); "
        "killer, killer_gen = nf.killer(1, 0); "
        "damage_health, damage_armor, damage_hits, damage_seconds = "
        "    nf.damage_taken(1, 0); "
        "supp = nf.suppression(1, 0); "
        "missing = (nf.last_attacker(2, 0) == nil) and 1 or 0",
        "memory", &err));

    double value = 0.0;
    NF_CHECK(vm.get_global_number("attacker", value));
    NF_CHECK_NEAR(value, 7.0, 1e-9);
    NF_CHECK(vm.get_global_number("attacker_gen", value));
    NF_CHECK_NEAR(value, 2.0, 1e-9);
    NF_CHECK(vm.get_global_number("killer", value));
    NF_CHECK_NEAR(value, 7.0, 1e-9);
    NF_CHECK(vm.get_global_number("killer_gen", value));
    NF_CHECK_NEAR(value, 2.0, 1e-9);
    NF_CHECK(vm.get_global_number("damage_health", value));
    NF_CHECK_NEAR(value, 12.0, 1e-9);
    NF_CHECK(vm.get_global_number("damage_armor", value));
    NF_CHECK_NEAR(value, 3.0, 1e-9);
    NF_CHECK(vm.get_global_number("damage_hits", value));
    NF_CHECK_NEAR(value, 4.0, 1e-9);
    NF_CHECK(vm.get_global_number("damage_seconds", value));
    NF_CHECK_NEAR(value, 1.5, 1e-9);
    NF_CHECK(vm.get_global_number("supp", value));
    NF_CHECK_NEAR(value, 0.5, 1e-9);
    NF_CHECK(vm.get_global_number("missing", value));
    NF_CHECK_NEAR(value, 1.0, 1e-9);
}

NF_TEST(script_sandbox_blocks_file_and_os_access) {    LuaVM vm;
    double out = 0;
    // dofile/loadfile/io/os/package are all nil in the sandbox.
    NF_CHECK(vm.eval_number("dofile == nil and 1 or 0", out, nullptr));
    NF_CHECK_NEAR(out, 1.0, 1e-9);
    NF_CHECK(vm.eval_number("io == nil and 1 or 0", out, nullptr));
    NF_CHECK_NEAR(out, 1.0, 1e-9);
    NF_CHECK(vm.eval_number("os == nil and 1 or 0", out, nullptr));
    NF_CHECK_NEAR(out, 1.0, 1e-9);
    NF_CHECK(vm.eval_number("package == nil and 1 or 0", out, nullptr));
    NF_CHECK_NEAR(out, 1.0, 1e-9);
    NF_CHECK(vm.eval_number("debug == nil and 1 or 0", out, nullptr));
    NF_CHECK_NEAR(out, 1.0, 1e-9);
}

NF_TEST(script_log_routes_through_callback) {
    LuaVM vm;
    std::vector<std::string> captured;
    vm.set_log_callback([&](const std::string& level, const std::string& msg) {
        captured.push_back(level + ":" + msg);
    });
    NF_CHECK(vm.run_string("nf.log_info('hello', 1, true)\nprint('via print')", "log", nullptr));
    NF_CHECK(captured.size() == 2);
    NF_CHECK(captured[0] == "info:hello\t1\ttrue");
    NF_CHECK(captured[1] == "info:via print");
}

// ---------------------------------------------------------------------------
// Entity bindings
// ---------------------------------------------------------------------------

NF_TEST(script_entity_transform_roundtrip) {
    ecs::World world;
    ecs::Entity e = world.create_entity();
    scene::Transform t;
    t.local_x = 1.0f;
    t.local_y = 2.0f;
    t.local_z = 3.0f;
    world.add<scene::Transform>(e, t);

    LuaVM vm;
    install_ecs_bindings(vm, world);
    vm.set_global_number("eid", static_cast<double>(e.id));
    vm.set_global_number("egen", static_cast<double>(e.generation));

    double x = 0, y = 0, z = 0;
    NF_CHECK(vm.eval_number("nf.entity_alive(eid, egen) and 1 or 0", x, nullptr));
    NF_CHECK_NEAR(x, 1.0, 1e-9);
    NF_CHECK(vm.run_string("px, py, pz = nf.entity_pos(eid, egen)", "get", nullptr));
    NF_CHECK(vm.get_global_number("px", x));
    NF_CHECK(vm.get_global_number("py", y));
    NF_CHECK(vm.get_global_number("pz", z));
    NF_CHECK_NEAR(x, 1.0, 1e-6);
    NF_CHECK_NEAR(y, 2.0, 1e-6);
    NF_CHECK_NEAR(z, 3.0, 1e-6);

    NF_CHECK(vm.run_string("ok = nf.set_entity_pos(eid, egen, 7, 8, 9)", "set", nullptr));
    NF_CHECK(vm.eval_number("ok and 1 or 0", x, nullptr));
    NF_CHECK_NEAR(x, 1.0, 1e-9);
    const auto* back = world.get<scene::Transform>(e);
    NF_CHECK(back);
    NF_CHECK_NEAR(back->local_x, 7.0f, 1e-6f);
    NF_CHECK_NEAR(back->local_z, 9.0f, 1e-6f);

    // Dead entity: alive=false, pos=nil, set=false — never a crash.
    world.remove<scene::Transform>(e);
    NF_CHECK(vm.eval_number("nf.entity_pos(eid, egen) == nil and 1 or 0", x, nullptr));
    NF_CHECK_NEAR(x, 1.0, 1e-9);
    NF_CHECK(vm.run_string("ok2 = nf.set_entity_pos(eid, egen, 0, 0, 0)", "set2", nullptr));
    NF_CHECK(vm.eval_number("ok2 and 1 or 0", x, nullptr));
    NF_CHECK_NEAR(x, 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// ScriptSystem ticking
// ---------------------------------------------------------------------------

NF_TEST(script_system_moves_entity_over_ticks) {
    ecs::World world;
    ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    ScriptComponent comp;
    comp.source =
        "function update(dt)\n"
        "  local id, gen = nf.self()\n"
        "  local x, y, z = nf.entity_pos(id, gen)\n"
        "  nf.set_entity_pos(id, gen, x + 2 * dt, y, z)\n"
        "end\n";
    world.add<ScriptComponent>(e, comp);

    ScriptSystem sys;
    for (int i = 0; i < 10; ++i) sys.update(world, 0.5f);
    const auto* t = world.get<scene::Transform>(e);
    NF_CHECK(t);
    // 10 ticks * 0.5s * 2 units/s = 10 units.
    NF_CHECK_NEAR(t->local_x, 10.0f, 1e-4f);
}

NF_TEST(script_system_isolates_environments) {
    ecs::World world;
    ecs::Entity a = world.create_entity();
    ecs::Entity b = world.create_entity();
    world.add<scene::Transform>(a, scene::Transform{});
    world.add<scene::Transform>(b, scene::Transform{});

    // Same global name, different sources: each script must see only its own.
    ScriptComponent ca;
    ca.source =
        "counter = 0\n"
        "function update(dt)\n"
        "  counter = counter + 1\n"
        "  local id, gen = nf.self()\n"
        "  nf.set_entity_pos(id, gen, counter, 0, 0)\n"
        "end\n";
    ScriptComponent cb;
    cb.source =
        "counter = 100\n"
        "function update(dt)\n"
        "  counter = counter + 10\n"
        "  local id, gen = nf.self()\n"
        "  nf.set_entity_pos(id, gen, counter, 0, 0)\n"
        "end\n";
    world.add<ScriptComponent>(a, ca);
    world.add<ScriptComponent>(b, cb);

    ScriptSystem sys;
    sys.update(world, 0.016f);
    sys.update(world, 0.016f);

    // A ticked twice from 0 (+1 each): 2. B ticked twice from 100 (+10): 120.
    // Any _ENV leak between the two would corrupt one of these.
    NF_CHECK_NEAR(world.get<scene::Transform>(a)->local_x, 2.0f, 1e-6f);
    NF_CHECK_NEAR(world.get<scene::Transform>(b)->local_x, 120.0f, 1e-6f);
}

NF_TEST(script_system_parks_broken_scripts) {
    ecs::World world;
    ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});

    ScriptComponent comp;
    comp.source = "function update(dt) error('always fails') end\n";
    world.add<ScriptComponent>(e, comp);

    ScriptSystem sys;
    // 10 ticks: 5 errors logged, then parked (no hang, no crash).
    for (int i = 0; i < 10; ++i) sys.update(world, 0.016f);

    // A source edit recompiles and runs again.
    auto* c = world.get<ScriptComponent>(e);
    NF_CHECK(c);
    c->source = "function update(dt) nf.log_info('healed') end\n";
    std::vector<std::string> captured;
    sys.vm().set_log_callback([&](const std::string& level, const std::string& msg) {
        (void)level;
        captured.push_back(msg);
    });
    sys.update(world, 0.016f);
    NF_CHECK(!captured.empty());
    NF_CHECK(captured[0] == "healed");
}

NF_TEST(script_system_missing_update_is_an_error) {
    ecs::World world;
    ecs::Entity e = world.create_entity();
    ScriptComponent comp;
    comp.source = "x = 1\n"; // no update()
    world.add<ScriptComponent>(e, comp);
    ScriptSystem sys;
    sys.update(world, 0.016f); // logs, parks, never crashes
    sys.update(world, 0.016f);
}

NF_TEST(script_system_disabled_scripts_do_not_run) {
    ecs::World world;
    ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    ScriptComponent comp;
    comp.source = "function update(dt) error('must not run') end\n";
    comp.enabled = false;
    world.add<ScriptComponent>(e, comp);
    ScriptSystem sys;
    sys.update(world, 0.016f); // silent
    NF_CHECK_NEAR(world.get<scene::Transform>(e)->local_x, 0.0f, 1e-6f);
}
