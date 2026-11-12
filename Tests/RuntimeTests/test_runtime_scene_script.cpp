// Tests/RuntimeTests/test_runtime_scene_script.cpp — is Lua reachable from a scene?
//
// ScriptSystem is unit-tested in isolation (Tests/ScriptTests). This file is
// the Rule 0 proof: a scene that merely declares `Script:` ends up resolved,
// stepped by Runtime::update(), and drawn from — with no test calling the
// script system directly. Before this work the whole chain (parse -> resolve
// -> step -> propagate) was reachable only from tests: no loader line, no
// Runtime step, and no editor entry point touched it.
//
// Headless like the destruction reachability suite: the device exists because
// the Runtime constructor asks for one, and the suite NF_SKIPs when no Vulkan
// is available rather than reporting a pass that verified nothing.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scripting/ScriptEngine.hpp>

#include <filesystem>
#include <memory>
#include <string>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;

namespace {

constexpr float kDt = 1.0f / 60.0f;

/// Owns the device + runtime so a test body is one block and teardown order is
/// fixed (runtime before device).
struct HeadlessRuntime {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    HeadlessRuntime() : manager(vfs, registry) {
        tmp = nf::test::temp_root() / "nf_runtime_scene_script";
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp);
        vfs.mount("content://", tmp);

        device = rhi::create_device();
        if (!device) {
            return;
        }
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        desc.enable_validation = false;
        if (!device->init(desc)) {
            device.reset();
            return;
        }
        runtime = std::make_unique<Runtime>(vfs, registry, manager, *device, nullptr);
    }

    ~HeadlessRuntime() {
        runtime.reset();
        if (device) {
            device->wait_idle();
            device->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    HeadlessRuntime(const HeadlessRuntime&) = delete;
    HeadlessRuntime& operator=(const HeadlessRuntime&) = delete;

    bool ok() const { return static_cast<bool>(runtime); }
};

void write_script(VirtualFileSystem& vfs, const std::string& logical, const std::string& source) {
    std::string err;
    auto res = vfs.write_text(logical, source);
    NF_CHECK(res.ok);
}

} // namespace

NF_TEST(script_line_round_trips_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_script_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("ScriptRoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    scripting::ScriptComponent comp;
    comp.path = "content://Scripts/spin.lua";
    comp.enabled = true;
    scene.world().add<scripting::ScriptComponent>(e, comp);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/script.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/script.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    auto loaded_e = result.scene->world().all_entities()[0];
    const auto* loaded = result.scene->world().get<scripting::ScriptComponent>(loaded_e);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK_EQ(loaded->path, std::string("content://Scripts/spin.lua"));
        NF_CHECK(loaded->enabled);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(script_line_rejects_unknown_lang_and_missing_path) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_script_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\n"
        "version: 1\n"
        "name: Bad\n"
        "entity_count: 2\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: A\n"
        "  Script: lang=csharp path=content://Scripts/a.lua enabled=true\n"
        "---\n"
        "entity: 1:0\n"
        "  Name: B\n"
        "  Script: lang=lua enabled=true\n";
    NF_CHECK(vfs.write_text("content://Scenes/bad.nfscene", text).ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/bad.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    // Both lines are rejected loudly (warnings) rather than stored as
    // components that look scripted but never tick.
    NF_CHECK_EQ(result.scene->world().query<scripting::ScriptComponent>().size(), static_cast<size_t>(0));
    NF_CHECK(!result.warnings.empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_script_is_resolved_and_stepped_by_update) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }

    // A script that moves its own entity +X every tick. The movement is what
    // proves update() drives it: nothing in the test calls the script system.
    write_script(env.vfs, "content://Scripts/move.lua",
                 "function update(dt)\n"
                 "  local id, gen = nf.self()\n"
                 "  local x, y, z = nf.entity_pos(id, gen)\n"
                 "  nf.set_entity_pos(id, gen, x + 1.0, y, z)\n"
                 "end\n");

    auto s = std::make_unique<Scene>("Scripted");
    ecs::Entity e = s->world().create_entity();
    Transform t{};
    s->world().add<Transform>(e, t);
    scripting::ScriptComponent comp;
    comp.path = "content://Scripts/move.lua";
    s->world().add<scripting::ScriptComponent>(e, std::move(comp));

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Scripted.nfscene");
    NF_CHECK_EQ(env.runtime->script_count(), static_cast<size_t>(1));
    NF_CHECK_EQ(env.runtime->scripts_runnable_count(), static_cast<size_t>(1));

    // The resolve happened inside adopt_scene: a path-only component became a
    // runnable source without the test touching the script system.
    {
        const scene::Scene* live = env.runtime->scene();
        NF_CHECK(live != nullptr);
        const auto* resolved =
            live->world().get<scripting::ScriptComponent>(live->world().all_entities().front());
        NF_CHECK(resolved != nullptr);
        if (resolved != nullptr) {
            NF_CHECK(!resolved->source.empty());
        }
    }

    for (int i = 0; i < 3; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK_EQ(env.runtime->script_updates(), static_cast<size_t>(3));

    const scene::Scene* live = env.runtime->scene();
    NF_CHECK(live != nullptr);
    const auto* tr = live->world().get<Transform>(live->world().all_entities().front());
    NF_CHECK(tr != nullptr);
    if (tr != nullptr) {
        // Three ticks of +1 on X, through the real frame path.
        NF_CHECK_NEAR(tr->local_x, 3.0f, 1e-4f);
    }
}

NF_TEST(disabled_or_missing_script_never_ticks) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }

    write_script(env.vfs, "content://Scripts/move.lua",
                 "function update(dt)\n"
                 "  local id, gen = nf.self()\n"
                 "  local x, y, z = nf.entity_pos(id, gen)\n"
                 "  nf.set_entity_pos(id, gen, x + 1.0, y, z)\n"
                 "end\n");

    auto s = std::make_unique<Scene>("ScriptedOff");
    ecs::Entity off = s->world().create_entity();
    s->world().add<Transform>(off, Transform{});
    scripting::ScriptComponent disabled;
    disabled.path = "content://Scripts/move.lua";
    disabled.enabled = false;
    s->world().add<scripting::ScriptComponent>(off, std::move(disabled));

    ecs::Entity missing = s->world().create_entity();
    s->world().add<Transform>(missing, Transform{});
    scripting::ScriptComponent ghost;
    ghost.path = "content://Scripts/does_not_exist.lua";
    s->world().add<scripting::ScriptComponent>(missing, std::move(ghost));

    env.runtime->adopt_scene(std::move(s), "content://Scenes/ScriptedOff.nfscene");
    NF_CHECK_EQ(env.runtime->script_count(), static_cast<size_t>(2));
    NF_CHECK_EQ(env.runtime->scripts_runnable_count(), static_cast<size_t>(0));

    env.runtime->update(kDt);
    NF_CHECK_EQ(env.runtime->script_updates(), static_cast<size_t>(0));
}

NF_TEST(script_cache_is_cleared_on_scene_adopt) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }

    // v1 moves +1, v2 moves +10. Same path, different bytes: without a cache
    // clear on adopt, the second scene would keep running v1's bytecode.
    write_script(env.vfs, "content://Scripts/move.lua",
                 "function update(dt)\n"
                 "  local id, gen = nf.self()\n"
                 "  local x, y, z = nf.entity_pos(id, gen)\n"
                 "  nf.set_entity_pos(id, gen, x + 1.0, y, z)\n"
                 "end\n");
    {
        auto s = std::make_unique<Scene>("V1");
        ecs::Entity e = s->world().create_entity();
        s->world().add<Transform>(e, Transform{});
        scripting::ScriptComponent comp;
        comp.path = "content://Scripts/move.lua";
        s->world().add<scripting::ScriptComponent>(e, std::move(comp));
        env.runtime->adopt_scene(std::move(s), "content://Scenes/V1.nfscene");
        env.runtime->update(kDt);
    }
    write_script(env.vfs, "content://Scripts/move.lua",
                 "function update(dt)\n"
                 "  local id, gen = nf.self()\n"
                 "  local x, y, z = nf.entity_pos(id, gen)\n"
                 "  nf.set_entity_pos(id, gen, x + 10.0, y, z)\n"
                 "end\n");
    {
        auto s = std::make_unique<Scene>("V2");
        ecs::Entity e = s->world().create_entity();
        s->world().add<Transform>(e, Transform{});
        scripting::ScriptComponent comp;
        comp.path = "content://Scripts/move.lua";
        s->world().add<scripting::ScriptComponent>(e, std::move(comp));
        env.runtime->adopt_scene(std::move(s), "content://Scenes/V2.nfscene");
        env.runtime->update(kDt);
        const scene::Scene* live = env.runtime->scene();
        NF_CHECK(live != nullptr);
        const auto* tr = live->world().get<Transform>(live->world().all_entities().front());
        NF_CHECK(tr != nullptr);
        if (tr != nullptr) {
            NF_CHECK_NEAR(tr->local_x, 10.0f, 1e-4f);
        }
    }
}

NF_TEST(runtime_pause_freezes_counter_timer_and_entities_without_dt) {
    HeadlessRuntime env;
    if (!env.ok()) { NF_SKIP("no Vulkan device"); return; }
    // Counter increments without reading dt; timer accumulates dt.
    write_script(env.vfs, "content://Scripts/counter.lua",
                 "local count = 0\n"
                 "local timer = 0\n"
                 "function update(dt)\n"
                 "  count = count + 1\n"
                 "  timer = timer + dt\n"
                 "  local id, gen = nf.self()\n"
                 "  local x, y, z = nf.entity_pos(id, gen)\n"
                 "  nf.set_entity_pos(id, gen, x + count * 0.0 + 1.0, y, z)\n"
                 "  print('tick ' .. count .. ' ' .. timer)\n"
                 "end\n");
    auto s = std::make_unique<Scene>("PauseCounter");
    ecs::Entity e = s->world().create_entity();
    s->world().add<Transform>(e, Transform{});
    scripting::ScriptComponent comp;
    comp.path = "content://Scripts/counter.lua";
    s->world().add<scripting::ScriptComponent>(e, std::move(comp));
    env.runtime->adopt_scene(std::move(s), "content://Scenes/PauseCounter.nfscene");
    for (int i = 0; i < 5; ++i) env.runtime->update(kDt);
    NF_CHECK_EQ(env.runtime->script_updates(), static_cast<size_t>(5));
    const auto start_updates = env.runtime->script_updates();
    // Pause freezes even with a real dt — dt alone is not the guard.
    env.runtime->set_paused(true);
    NF_CHECK(env.runtime->paused());
    for (int i = 0; i < 10; ++i) env.runtime->update(kDt);
    NF_CHECK_EQ(env.runtime->script_updates(), start_updates);
    const auto* live1 = env.runtime->scene();
    const auto* tr1 = live1->world().get<Transform>(live1->world().all_entities().front());
    const float x_paused = tr1 ? tr1->local_x : -1.0f;
    NF_CHECK_NEAR(x_paused, 5.0f, 1e-4f);
    // Resume without jump: one normal frame advances exactly one tick.
    env.runtime->set_paused(false);
    env.runtime->update(1.0f / 60.0f);
    NF_CHECK_EQ(env.runtime->script_updates(), start_updates + 1);
    const auto* live2 = env.runtime->scene();
    const auto* tr2 = live2->world().get<Transform>(live2->world().all_entities().front());
    NF_CHECK(tr2 != nullptr);
    if (tr2) NF_CHECK_NEAR(tr2->local_x, 6.0f, 1e-4f);
    // Play/Stop replay: clear cache does not accumulate.
    for (int i = 0; i < 3; ++i) {
        auto s2 = std::make_unique<Scene>("PauseCounter2");
        ecs::Entity e2 = s2->world().create_entity();
        s2->world().add<Transform>(e2, Transform{});
        scripting::ScriptComponent c2;
        c2.path = "content://Scripts/counter.lua";
        s2->world().add<scripting::ScriptComponent>(e2, std::move(c2));
        env.runtime->adopt_scene(std::move(s2), "content://Scenes/PauseCounter2.nfscene");
        NF_CHECK_EQ(env.runtime->script_count(), static_cast<size_t>(1));
        env.runtime->set_paused(true);
        env.runtime->update(kDt);
        NF_CHECK(env.runtime->paused());
        env.runtime->set_paused(false);
        env.runtime->update(kDt);
    }
    NF_CHECK_EQ(env.runtime->script_count(), static_cast<size_t>(1));
}

NF_TEST(runtime_script_canvas_reaches_scene_scripts_and_resets_on_scene_change) {
    HeadlessRuntime env;
    if (!env.ok()) { NF_SKIP("Vulkan is unavailable"); }
    write_script(env.vfs, "content://ui.lua", R"LUA(
        function update(dt)
            nf.ui.label("title", "Runtime UI", .1,.05,.4,.1)
            if nf.ui.button("go", "Go", .1,.2,.3,.1) then
                nf.set_entity_pos(self_id, self_gen, 7,0,0)
            end
        end
    )LUA");
    scene::Scene scene("UI scene");
    const auto entity = scene.world().create_entity();
    scene.world().add<scene::Transform>(entity, scene::Transform{});
    scripting::ScriptComponent component;
    component.path = "content://ui.lua";
    scene.world().add<scripting::ScriptComponent>(entity, component);
    std::string error;
    NF_CHECK(save_scene_to_vfs(env.vfs, "content://UI.nfscene", scene, error));
    NF_CHECK(env.runtime->load_scene("content://UI.nfscene", error));
    env.runtime->update(kDt);
    NF_CHECK_EQ(env.runtime->game_ui().size(),2u);
    env.runtime->set_ui_pointer({.2f,.25f,true,true});
    env.runtime->update(kDt);
    env.runtime->set_ui_pointer({.2f,.25f,false,true});
    env.runtime->update(kDt);
    const auto entities = env.runtime->edit_scene()->world().query<scripting::ScriptComponent>();
    NF_CHECK_EQ(entities.size(),1u);
    NF_CHECK_NEAR(env.runtime->edit_scene()->world().get<scene::Transform>(entities[0])->local_x,7,.001f);
    env.runtime->set_ui_pointer({.2f,.25f,true,true});
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->load_scene("content://UI.nfscene", error));
    NF_CHECK_EQ(env.runtime->game_ui().size(),0u);
    env.runtime->set_ui_pointer({.2f,.25f,false,true});
    env.runtime->update(kDt);
    const auto reloaded = env.runtime->edit_scene()->world().query<scripting::ScriptComponent>();
    NF_CHECK_NEAR(env.runtime->edit_scene()->world().get<scene::Transform>(reloaded[0])->local_x,0,.001f);
}


namespace {
void write_flow_scene(HeadlessRuntime& env, const std::string& path, const std::string& name,
                      const std::string& source) {
    const auto script = "content://Scripts/" + name + ".lua";
    write_script(env.vfs, script, source);
    Scene scene(name);
    const auto entity = scene.world().create_entity();
    scene.world().add<Transform>(entity, Transform{});
    scripting::ScriptComponent comp;
    comp.path = script;
    scene.world().add<scripting::ScriptComponent>(entity, comp);
    std::string error;
    NF_CHECK(save_scene_to_vfs(env.vfs, path, scene, error));
}
float first_flow_x(Runtime& runtime) {
    const auto& world = runtime.scene()->world();
    const auto entities = world.query<Transform>();
    return world.get<Transform>(entities[0])->local_x;
}
}

NF_TEST(runtime_scene_transition_is_deferred_and_callback_finishes) {
    HeadlessRuntime env;
    if (!env.ok()) NF_SKIP("no Vulkan device");
    const std::string a = "content://Scenes/A.nfscene", b = "content://Scenes/B.nfscene";
    write_flow_scene(env, a, "A", R"lua(
        function update(dt)
            assert(nf.scene.load('content://Scenes/B.nfscene'))
            assert(not nf.scene.load('content://Scenes/A.nfscene'))
            assert(nf.scene.current() == 'content://Scenes/A.nfscene')
            nf.set_entity_pos(self_id, self_gen, 9, 0, 0)
            assert(nf.session.set('finished_callback', true))
        end
    )lua");
    write_flow_scene(env, b, "B", R"lua(
        function update(dt)
            assert(nf.session.get('finished_callback') == true)
            nf.set_entity_pos(self_id, self_gen, 23, 0, 0)
            nf.ui.label('new', 'NEW SCENE', .1,.1,.4,.1)
        end
    )lua");
    std::string error;
    NF_CHECK(env.runtime->load_scene(a, error));
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->loaded_scene_path() == a);
    NF_CHECK_EQ(first_flow_x(*env.runtime), 9.0f);
    NF_CHECK(env.runtime->game_flow().pending());
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->loaded_scene_path() == b);
    NF_CHECK_EQ(first_flow_x(*env.runtime), 23.0f);
    NF_CHECK(env.runtime->game_flow().status() == "ready");
    NF_CHECK_EQ(env.runtime->game_ui().size(), size_t(1));
}

NF_TEST(runtime_failed_scene_transition_keeps_world_and_allows_retry) {
    HeadlessRuntime env;
    if (!env.ok()) NF_SKIP("no Vulkan device");
    const std::string a = "content://Scenes/A.nfscene";
    write_flow_scene(env, a, "A", R"lua(
        local count = 0
        function update(dt)
            count = count + 1
            nf.set_entity_pos(self_id, self_gen, count, 0, 0)
        end
    )lua");
    std::string error;
    NF_CHECK(env.runtime->load_scene(a, error));
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->game_flow().request_scene("content://Scenes/Missing.nfscene", error));
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->loaded_scene_path() == a);
    NF_CHECK_EQ(first_flow_x(*env.runtime), 2.0f);
    NF_CHECK(env.runtime->game_flow().status() == "failed");
    NF_CHECK(!env.runtime->game_flow().error().empty());
    NF_CHECK(env.vfs.write_text("content://Scenes/Bad.nfscene", "not a scene").ok);
    NF_CHECK(env.runtime->game_flow().request_scene("content://Scenes/Bad.nfscene", error));
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->loaded_scene_path() == a);
    NF_CHECK_EQ(first_flow_x(*env.runtime), 3.0f);
    NF_CHECK(env.runtime->game_flow().status() == "failed");
    NF_CHECK(env.runtime->game_flow().request_restart(error));
    env.runtime->update(kDt);
    NF_CHECK_EQ(first_flow_x(*env.runtime), 1.0f);
    NF_CHECK(env.runtime->game_flow().status() == "ready");
}

NF_TEST(runtime_scene_requests_wait_during_pause_and_session_reset_cancels) {
    HeadlessRuntime env;
    if (!env.ok()) NF_SKIP("no Vulkan device");
    const std::string a = "content://Scenes/A.nfscene", b = "content://Scenes/B.nfscene";
    write_flow_scene(env, a, "A", "function update(dt) end");
    write_flow_scene(env, b, "B", "function update(dt) end");
    std::string error;
    NF_CHECK(env.runtime->load_scene(a, error));
    NF_CHECK(env.runtime->game_flow().request_scene(b, error));
    env.runtime->set_paused(true);
    env.runtime->update(1.0f);
    NF_CHECK(env.runtime->loaded_scene_path() == a);
    NF_CHECK(env.runtime->game_flow().pending());
    env.runtime->set_paused(false);
    env.runtime->update(0.0f);
    NF_CHECK(env.runtime->loaded_scene_path() == a);
    env.runtime->reset_game_session();
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->loaded_scene_path() == a);
    NF_CHECK(!env.runtime->game_flow().pending());
    NF_CHECK(env.runtime->game_flow().request_scene(b, error));
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->loaded_scene_path() == b);
}
