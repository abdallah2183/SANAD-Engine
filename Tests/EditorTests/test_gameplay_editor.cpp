// Tests/EditorTests/test_gameplay_editor.cpp — Phase 10, W3 (editor attach path)
//
// The inspector's "Attach" button goes through EditorApp, not straight into the
// world, so the validation lives there: a module that is not registered in this
// build must be refused, and re-attaching must not quietly discard state the
// scene already holds.

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

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>

#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

class EditorGameplayProbe final : public gameplay::GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "EditorGameplayProbe"; }
};

std::filesystem::path gameplay_temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

/// One-entity scene on disk, so EditorApp has something to open.
void write_probe_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene scene("GameplayWork");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Rig"});

    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, logical, scene, err));
}

} // namespace

NF_GAMEPLAY_MODULE(EditorGameplayProbe, "EditorGameplayProbe")

NF_TEST(editor_attach_gameplay_module_requires_a_registered_module) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = gameplay_temp_dir("nf_ed_gameplay_attach");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg, &device);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));

    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    // A name nothing registered is refused, and nothing is added: a component
    // naming a module no build can construct is data the scene can never use.
    err.clear();
    NF_CHECK(!app.attach_gameplay_module(e, "NotARegisteredModule", err));
    NF_CHECK(!err.empty());
    NF_CHECK(!world.has<gameplay::GameplayModuleComponent>(e));

    err.clear();
    NF_CHECK(!app.attach_gameplay_module(e, "", err));
    NF_CHECK(!err.empty());

    // The registered one attaches.
    err.clear();
    NF_CHECK(app.attach_gameplay_module(e, "EditorGameplayProbe", err));
    const auto* comp = world.get<gameplay::GameplayModuleComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->module_name, std::string("EditorGameplayProbe"));
        NF_CHECK(comp->enabled);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_reattaching_the_same_module_preserves_its_state) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = gameplay_temp_dir("nf_ed_gameplay_reattach");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg, &device);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));

    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_gameplay_module(e, "EditorGameplayProbe", err));
    auto* comp = world.get<gameplay::GameplayModuleComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp == nullptr) {
        std::filesystem::remove_all(tmp);
        return;
    }

    // State a session has accumulated — the inspector's Attach button must not
    // be a way to lose it.
    comp->properties.emplace("radius", "12.5");
    comp->enabled = false;

    NF_CHECK(app.attach_gameplay_module(e, "EditorGameplayProbe", err));
    comp = world.get<gameplay::GameplayModuleComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->properties.size(), static_cast<size_t>(1));
        NF_CHECK_EQ(comp->properties.at("radius"), std::string("12.5"));
        NF_CHECK(!comp->enabled);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_detach_gameplay_module_removes_the_component) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = gameplay_temp_dir("nf_ed_gameplay_detach");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg, &device);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));

    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_gameplay_module(e, "EditorGameplayProbe", err));
    NF_CHECK(world.has<gameplay::GameplayModuleComponent>(e));

    NF_CHECK(app.detach_gameplay_module(e, err));
    NF_CHECK(!world.has<gameplay::GameplayModuleComponent>(e));

    // Detaching twice is an error rather than a silent success.
    err.clear();
    NF_CHECK(!app.detach_gameplay_module(e, err));
    NF_CHECK(!err.empty());

    std::filesystem::remove_all(tmp);
}
