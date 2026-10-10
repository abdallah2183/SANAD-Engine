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
        tmp = std::filesystem::temp_directory_path() / "nf_runtime_scene_script";
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
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_script_rt";
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
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_script_bad";
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
