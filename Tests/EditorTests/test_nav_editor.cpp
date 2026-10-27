// Tests/EditorTests/test_nav_editor.cpp — Phase 28 (navigation authoring)
//
// Rule 0 for the editor, same contract as test_wiring_editor.cpp: a component
// the inspector cannot author might as well not exist. These tests drive the
// navigation authoring path end to end through EditorApp — attach, validated
// edit, detach, and the save/load round trip that makes an authored volume
// survive a reopen.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Test/RHITestCommon.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <filesystem>
#include <limits>
#include <string>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path nav_temp_dir(const std::string& name) {
    auto p = nf::test::temp_root() / name;
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

NF_TEST(editor_nav_route_attach_edit_detach) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = nav_temp_dir("nf_ed_nav_route");
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

    // A point that is not a finite triple is refused before it can reach the
    // file; the existing route (if any) is left alone.
    runtime::NavRouteComponent bad;
    bad.points.push_back(runtime::NavPointSpec{
        std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f, -1.0f});
    err.clear();
    NF_CHECK(!app.set_nav_route(e, bad, err));
    NF_CHECK(!err.empty());
    NF_CHECK(!world.has<runtime::NavRouteComponent>(e));

    // Attaching authors a usable two-point route around the entity.
    NF_CHECK(app.attach_nav_route(e, err));
    const auto* attached = world.get<runtime::NavRouteComponent>(e);
    NF_CHECK(attached != nullptr);
    if (attached != nullptr) {
        NF_CHECK_EQ(attached->points.size(), static_cast<usize>(2));
        NF_CHECK(attached->loop);
        NF_CHECK(!attached->ping_pong);
    }
    // Idempotent, like the other attach methods.
    NF_CHECK(app.attach_nav_route(e, err));

    runtime::NavRouteComponent spec;
    spec.wait = 0.75f;
    spec.loop = false;
    spec.ping_pong = true;
    spec.enabled = false;
    // Phase 65: the inspector's two new fields travel through the same
    // validated setter the rest of the route does.
    spec.hold = runtime::NavRouteHoldPolicy::Sensing;
    spec.resume = 1.75f;
    spec.points.push_back(runtime::NavPointSpec{2.0f, 0.5f, 3.0f, -1.0f});
    spec.points.push_back(runtime::NavPointSpec{8.0f, 0.5f, 9.0f, 2.5f});
    NF_CHECK(app.set_nav_route(e, spec, err));
    const auto* comp = world.get<runtime::NavRouteComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_NEAR(comp->wait, 0.75f, 1e-6f);
        NF_CHECK(!comp->loop);
        NF_CHECK(comp->ping_pong);
        NF_CHECK(!comp->enabled);
        NF_CHECK(comp->hold == runtime::NavRouteHoldPolicy::Sensing);
        NF_CHECK_NEAR(comp->resume, 1.75f, 1e-6f);
        NF_CHECK_EQ(comp->points.size(), static_cast<usize>(2));
        NF_CHECK_NEAR(comp->points[0].x, 2.0f, 1e-6f);
        NF_CHECK_NEAR(comp->points[0].wait, -1.0f, 1e-6f);
        NF_CHECK_NEAR(comp->points[1].z, 9.0f, 1e-6f);
        NF_CHECK_NEAR(comp->points[1].wait, 2.5f, 1e-6f);
    }

    // The two new fields are validated like every other: a negative resume is
    // refused before it can reach the file, and the good route is untouched.
    runtime::NavRouteComponent bad_resume = spec;
    bad_resume.resume = -0.5f;
    err.clear();
    NF_CHECK(!app.set_nav_route(e, bad_resume, err));
    NF_CHECK(!err.empty());
    runtime::NavRouteComponent bad_hold = spec;
    bad_hold.hold = static_cast<runtime::NavRouteHoldPolicy>(7);
    err.clear();
    NF_CHECK(!app.set_nav_route(e, bad_hold, err));
    NF_CHECK(!err.empty());
    const auto* kept = world.get<runtime::NavRouteComponent>(e);
    NF_CHECK(kept != nullptr);
    if (kept != nullptr) {
        NF_CHECK(kept->hold == runtime::NavRouteHoldPolicy::Sensing);
        NF_CHECK_NEAR(kept->resume, 1.75f, 1e-6f);
    }

    NF_CHECK(app.detach_nav_route(e, err));
    NF_CHECK(!world.has<runtime::NavRouteComponent>(e));
    err.clear();
    NF_CHECK(!app.detach_nav_route(e, err));
    NF_CHECK(!err.empty());

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
    runtime::NavRouteComponent route;
    route.wait = 0.4f;
    route.loop = false;
    route.ping_pong = true;
    route.hold = runtime::NavRouteHoldPolicy::Aware;
    route.resume = 2.25f;
    route.points.push_back(runtime::NavPointSpec{1.0f, 0.0f, 2.0f, -1.0f});
    route.points.push_back(runtime::NavPointSpec{7.0f, 0.0f, 8.0f, 1.5f});
    NF_CHECK(app.set_nav_route(e, route, err));
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
    const auto* route2 = runtime2.edit_scene()->world().get<runtime::NavRouteComponent>(e2);
    NF_CHECK(route2 != nullptr);
    if (route2 != nullptr) {
        NF_CHECK_NEAR(route2->wait, 0.4f, 1e-5f);
        NF_CHECK(!route2->loop);
        NF_CHECK(route2->ping_pong);
        NF_CHECK(route2->hold == runtime::NavRouteHoldPolicy::Aware);
        NF_CHECK_NEAR(route2->resume, 2.25f, 1e-5f);
        NF_CHECK_EQ(route2->points.size(), static_cast<usize>(2));
        if (route2->points.size() == 2) {
            NF_CHECK_NEAR(route2->points[0].x, 1.0f, 1e-5f);
            NF_CHECK_NEAR(route2->points[0].wait, -1.0f, 1e-5f);
            NF_CHECK_NEAR(route2->points[1].z, 8.0f, 1e-5f);
            NF_CHECK_NEAR(route2->points[1].wait, 1.5f, 1e-5f);
        }
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
    NF_CHECK(app.attach_nav_route(e, err));
    // Idempotent: attaching what is already there is a success, not a failure.
    NF_CHECK(app.attach_navmesh(e, err));
    NF_CHECK(app.attach_nav_route(e, err));

    ecs::World& world = runtime.edit_scene()->world();
    NF_CHECK(world.has<runtime::NavMeshComponent>(e));
    NF_CHECK(world.has<runtime::NavAgentComponent>(e));
    NF_CHECK(world.has<runtime::NavRouteComponent>(e));

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_nav_debug_overlay_reads_the_live_bake) {
    // Step 8: the viewport overlay consumes Runtime::nav_debug_snapshot().
    // The volume mutators re-bake synchronously, so the overlay reads a live
    // mesh while authoring — no reopen, no play press. Detaching drops the
    // bake and the snapshot goes blank with it.
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = nav_temp_dir("nf_ed_nav_debug");
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

    // Off by default: a debug layer must never compete with the geometry it
    // annotates until the author asks for it. Pinned at the type level —
    // ui_settings() itself lives in the editor shell, outside what tests link.
    NF_CHECK(!editor::EditorUiSettings{}.show_navmesh);

    NF_CHECK(app.create_entity("Volume", ecs::kInvalidEntity, err));
    const ecs::Entity volume = app.stack().last_target();
    NF_CHECK(volume.valid());
    runtime::NavMeshComponent nm;
    nm.area_x = 12.0f;
    nm.area_y = 8.0f;
    nm.area_z = 12.0f;
    NF_CHECK(app.set_navmesh(volume, nm, err));
    NF_CHECK(app.runtime()->navmesh_built());
    runtime::NavDebugSnapshot snap = app.runtime()->nav_debug_snapshot();
    NF_CHECK(snap.has_mesh);
    NF_CHECK(!snap.mesh_polygons.empty());
    NF_CHECK(snap.agents.empty());

    NF_CHECK(app.create_entity("Walker", ecs::kInvalidEntity, err));
    const ecs::Entity walker = app.stack().last_target();
    NF_CHECK(walker.valid());
    runtime::NavAgentComponent na;
    na.goal_x = 10.0f;
    na.goal_z = 10.0f;
    NF_CHECK(app.set_nav_agent(walker, na, err));
    snap = app.runtime()->nav_debug_snapshot();
    NF_CHECK_EQ(snap.agents.size(), static_cast<size_t>(1));
    const runtime::NavDebugAgent& a = snap.agents[0];
    NF_CHECK(a.entity == walker);
    NF_CHECK_NEAR(a.goal.x, 10.0f, 1e-6f);
    NF_CHECK_NEAR(a.goal.z, 10.0f, 1e-6f);
    // Posed, never stepped (edit freeze): the marker and the goal ring draw,
    // but there is no route yet.
    const auto* wt = runtime.edit_scene()->world().get<scene::Transform>(walker);
    NF_CHECK(wt != nullptr);
    if (wt != nullptr) {
        NF_CHECK_NEAR(a.position.x, wt->local_x, 1e-6f);
        NF_CHECK_NEAR(a.position.z, wt->local_z, 1e-6f);
    }
    NF_CHECK(!a.has_path);

    // Detaching the volume drops the bake: blank overlay, not a stale mesh.
    NF_CHECK(app.detach_navmesh(volume, err));
    NF_CHECK(!app.runtime()->navmesh_built());
    NF_CHECK(!app.runtime()->nav_debug_snapshot().has_mesh);

    std::filesystem::remove_all(tmp);
}