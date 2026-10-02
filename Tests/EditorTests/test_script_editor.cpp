// Tests/EditorTests/test_script_editor.cpp — Phase 24 (editor script authoring)
//
// The inspector's Script section goes through EditorApp, not straight into
// the world: file creation, path validation, source loading and cache
// invalidation all live there. These tests prove that path end to end,
// including the save/load round trip that makes a scripted scene survive a
// reopen.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Scripting/ScriptEngine.hpp>

#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path script_temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_probe_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene scene("ScriptWork");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Rig"});

    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, logical, scene, err));
}

} // namespace

NF_TEST(editor_create_script_file_writes_a_runnable_template) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = script_temp_dir("nf_ed_script_create");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.create_script_file("content://Scripts/spin.lua", err));
    auto read = vfs.read_text("content://Scripts/spin.lua");
    NF_CHECK(read.ok);
    if (read.ok) {
        NF_CHECK(read.value.find("function update(dt)") != std::string::npos);
    }

    // Refuses to clobber: replacing a script file is not something the user
    // can undo by re-running the command.
    err.clear();
    NF_CHECK(!app.create_script_file("content://Scripts/spin.lua", err));
    NF_CHECK(!err.empty());

    // Paths with spaces never reach the scene format (field_value stops at
    // the first one), so they are rejected at the door.
    err.clear();
    NF_CHECK(!app.create_script_file("content://Scripts/my script.lua", err));

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_attach_script_loads_source_and_detaches) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = script_temp_dir("nf_ed_script_attach");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.create_script_file("content://Scripts/spin.lua", err));
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));

    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    // Missing files are refused: a component naming a file no build can load
    // is data the scene can never use.
    err.clear();
    NF_CHECK(!app.attach_script(e, "content://Scripts/ghost.lua", err));
    NF_CHECK(!err.empty());
    NF_CHECK(!world.has<scripting::ScriptComponent>(e));

    err.clear();
    NF_CHECK(app.attach_script(e, "content://Scripts/spin.lua", err));
    const auto* comp = world.get<scripting::ScriptComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->path, std::string("content://Scripts/spin.lua"));
        NF_CHECK(!comp->source.empty());
        NF_CHECK(comp->enabled);
    }

    // Re-attaching the same path is a no-op rather than a wipe.
    NF_CHECK(app.attach_script(e, "content://Scripts/spin.lua", err));

    // Disable + re-path + detach all behave.
    NF_CHECK(app.set_script_enabled(e, false, err));
    NF_CHECK(!world.get<scripting::ScriptComponent>(e)->enabled);

    NF_CHECK(app.create_script_file("content://Scripts/other.lua", err));
    NF_CHECK(app.set_script_path(e, "content://Scripts/other.lua", err));
    NF_CHECK_EQ(world.get<scripting::ScriptComponent>(e)->path, std::string("content://Scripts/other.lua"));

    NF_CHECK(app.detach_script(e, err));
    NF_CHECK(!world.has<scripting::ScriptComponent>(e));
    err.clear();
    NF_CHECK(!app.detach_script(e, err));
    NF_CHECK(!err.empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_scripted_scene_survives_save_and_reload) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = script_temp_dir("nf_ed_script_roundtrip");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.create_script_file("content://Scripts/spin.lua", err));
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));

    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();
    NF_CHECK(app.attach_script(e, "content://Scripts/spin.lua", err));
    NF_CHECK(app.save(err));

    // Reopen through a fresh app+runtime over the same VFS: the path must
    // survive, and the source must resolve again without the test touching
    // the script system.
    AssetRegistry reg2;
    AssetManager manager2(vfs, reg2);
    runtime::Runtime runtime2(vfs, reg2, manager2, device, nullptr);
    editor::ConsoleBuffer console2;
    editor::EditorApp app2(vfs, reg2, manager2, console2);
    app2.attach_runtime(&runtime2);
    NF_CHECK(app2.open_scene("content://Scenes/Work.nfscene", err));
    const ecs::World& world2 = runtime2.edit_scene()->world();
    NF_CHECK_EQ(world2.query<scripting::ScriptComponent>().size(), static_cast<size_t>(1));
    const auto* comp = world2.get<scripting::ScriptComponent>(world2.all_entities().front());
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->path, std::string("content://Scripts/spin.lua"));
        NF_CHECK(!comp->source.empty());
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_create_unique_script_file_never_clobbers) {
    // The regression from the live session: "New script" with an empty field
    // always tried script.lua, so once it existed the button failed forever.
    // The unique variant hands out script.lua, script_01.lua, ... instead.
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = script_temp_dir("nf_ed_script_unique");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err, path;
    NF_CHECK(app.create_unique_script_file("content://Scripts/", "script", path, err));
    NF_CHECK_EQ(path, std::string("content://Scripts/script.lua"));
    NF_CHECK(app.create_unique_script_file("content://Scripts/", "script", path, err));
    NF_CHECK_EQ(path, std::string("content://Scripts/script_01.lua"));
    NF_CHECK(app.create_unique_script_file("content://Scripts", "spin", path, err));
    NF_CHECK_EQ(path, std::string("content://Scripts/spin.lua"));

    err.clear();
    NF_CHECK(!app.create_unique_script_file("other://Scripts/", "script", path, err));
    NF_CHECK(!err.empty());
    err.clear();
    NF_CHECK(!app.create_unique_script_file("content://Scripts/", "my script", path, err));
    NF_CHECK(!err.empty());

    std::filesystem::remove_all(tmp);
}
