// Tests/EditorTests/test_nav_editor.cpp — Phase 28 (navigation authoring)
//
// Rule 0 for the editor, same contract as test_wiring_editor.cpp: a component
// the inspector cannot author might as well not exist. These tests drive the
// navigation authoring path end to end through EditorApp — attach, validated
// edit, detach, and the save/load round trip that makes an authored volume
// survive a reopen.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path nav_temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_probe_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene scene("NavWork");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Volume"});

    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, logical, scene, err));
}

} // namespace

NF_TEST(editor_navmesh_attach_edit_detach) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = nav_temp_dir("nf_ed_navmesh");
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
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    // A degenerate volume is refused at the door, nothing written.
    runtime::NavMeshComponent bad;
    bad.area_y = 0.0f;
    err.clear();
    NF_CHECK(!app.set_navmesh(e, bad, err));
    NF_CHECK(!err.empty());
    NF_CHECK(!world.has<runtime::NavMeshComponent>(e));

    runtime::NavMeshComponent spec;
    spec.area_x = 24.0f;
    spec.cell_size = 0.75f;
    spec.walkable_climb = 0.3f;
    spec.max_verts_per_poly = 5;
    NF_CHECK(app.set_navmesh(e, spec, err));
    const auto* comp = world.get<runtime::NavMeshComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_NEAR(comp->area_x, 24.0f, 1e-6f);
        NF_CHECK_NEAR(comp->cell_size, 0.75f, 1e-6f);
        NF_CHECK_EQ(comp->max_verts_per_poly, 5u);
    }

    NF_CHECK(app.detach_navmesh(e, err));
    NF_CHECK(!world.has<runtime::NavMeshComponent>(e));
    err.clear();
    NF_CHECK(!app.detach_navmesh(e, err));
    NF_CHECK(!err.empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_nav_agent_attach_edit_detach) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = nav_temp_dir("nf_ed_nav_agent");
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
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    runtime::NavAgentComponent bad;
    bad.speed = -1.0f;
    err.clear();
    NF_CHECK(!app.set_nav_agent(e, bad, err));
    NF_CHECK(!world.has<runtime::NavAgentComponent>(e));

    runtime::NavAgentComponent spec;
    spec.speed = 6.0f;
    spec.goal_x = -4.5f;
    spec.goal_z = 12.0f;
    spec.arrive_radius = 0.4f;
    NF_CHECK(app.set_nav_agent(e, spec, err));
    const auto* comp = world.get<runtime::NavAgentComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_NEAR(comp->speed, 6.0f, 1e-6f);
        NF_CHECK_NEAR(comp->goal_x, -4.5f, 1e-6f);
        NF_CHECK_NEAR(comp->goal_z, 12.0f, 1e-6f);
        NF_CHECK_NEAR(comp->arrive_radius, 0.4f, 1e-6f);
    }

    NF_CHECK(app.detach_nav_agent(e, err));
    NF_CHECK(!world.has<runtime::NavAgentComponent>(e));

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_nav_components_round_trip_through_save_and_load) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = nav_temp_dir("nf_ed_nav_rt");
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
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    runtime::NavMeshComponent nm;
    nm.area_x = 18.0f;
    nm.area_z = 14.0f;
    nm.cell_size = 0.6f;
    nm.min_region_area = 1.5f;
    NF_CHECK(app.set_navmesh(e, nm, err));
    runtime::NavAgentComponent na;
    na.speed = 4.5f;
    na.goal_x = 3.0f;
    na.goal_z = -6.0f;
    NF_CHECK(app.set_nav_agent(e, na, err));
    NF_CHECK(app.save(err));

    AssetRegistry reg2;
    AssetManager manager2(vfs, reg2);
    runtime::Runtime runtime2(vfs, reg2, manager2, device, nullptr);
    editor::ConsoleBuffer console2;
    editor::EditorApp app2(vfs, reg2, manager2, console2);
    app2.attach_runtime(&runtime2);
    NF_CHECK(app2.open_scene("content://Scenes/Work.nfscene", err));
    const ecs::Entity e2 = runtime2.edit_scene()->world().all_entities().front();

    const auto* nm2 = runtime2.edit_scene()->world().get<runtime::NavMeshComponent>(e2);
    NF_CHECK(nm2 != nullptr);
    if (nm2 != nullptr) {
        NF_CHECK_NEAR(nm2->area_x, 18.0f, 1e-5f);
        NF_CHECK_NEAR(nm2->area_z, 14.0f, 1e-5f);
        NF_CHECK_NEAR(nm2->cell_size, 0.6f, 1e-5f);
        NF_CHECK_NEAR(nm2->min_region_area, 1.5f, 1e-5f);
    }
    const auto* na2 = runtime2.edit_scene()->world().get<runtime::NavAgentComponent>(e2);
    NF_CHECK(na2 != nullptr);
    if (na2 != nullptr) {
        NF_CHECK_NEAR(na2->speed, 4.5f, 1e-5f);
        NF_CHECK_NEAR(na2->goal_x, 3.0f, 1e-5f);
        NF_CHECK_NEAR(na2->goal_z, -6.0f, 1e-5f);
    }

    // A reopened scene's runtime bakes its own volume: the authoring path and
    // the frame path agree on what a volume means.
    NF_CHECK(runtime2.navmesh_built());
    NF_CHECK_EQ(runtime2.nav_agent_count(), static_cast<size_t>(1));

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_every_add_menu_component_attaches_nav_entries) {
    // The Add Component menu is one entry point over the same validated
    // setters the sections use. This drives the two navigation items through
    // EditorApp on a fresh entity: whatever the menu offers must attach.
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = nav_temp_dir("nf_ed_nav_menu");
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
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    NF_CHECK(app.create_entity("Nav", ecs::kInvalidEntity, err));
    const ecs::Entity e = app.stack().last_target();
    NF_CHECK(e.valid());

    NF_CHECK(app.attach_navmesh(e, err));
    NF_CHECK(app.attach_nav_agent(e, err));
    // Idempotent: attaching what is already there is a success, not a failure.
    NF_CHECK(app.attach_navmesh(e, err));

    ecs::World& world = runtime.edit_scene()->world();
    NF_CHECK(world.has<runtime::NavMeshComponent>(e));
    NF_CHECK(world.has<runtime::NavAgentComponent>(e));

    std::filesystem::remove_all(tmp);
}
