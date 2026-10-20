// Tests/EditorTests/test_vehicle_editor.cpp — Step 11 (vehicle authoring)
//
// Rule 0 for the editor, same contract as test_nav_editor.cpp: a component
// the inspector cannot author might as well not exist. These tests drive the
// vehicle authoring path end to end through EditorApp — attach, validated
// edit, debug-drive input, detach — plus the two lifecycle properties the
// feature depends on: live inputs never reach the file, and a play/stop
// cycle preserves the car (PlayMode copiers scrub session state, Stop
// re-binds from the restored tune).

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
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
#include <NF/Physics/Components.hpp>
#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path vehicle_temp_dir(const std::string& name) {
    auto p = nf::test::temp_root() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_probe_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene scene("VehicleWork");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Car"});

    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, logical, scene, err));
}

} // namespace

NF_TEST(editor_vehicle_attach_edit_detach) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = vehicle_temp_dir("nf_ed_vehicle");
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

    // A degenerate tune is refused at the door, nothing written.
    physics::VehicleComponent bad;
    bad.config.chassis_mass = -4.0f;
    err.clear();
    NF_CHECK(!app.set_vehicle(e, bad, err));
    NF_CHECK(!err.empty());
    NF_CHECK(!world.has<physics::VehicleComponent>(e));

    physics::VehicleComponent spec;
    spec.config.chassis_mass = 1800.0f;
    spec.config.max_steer_deg = 25.0f;
    spec.config.tire_peak_friction = 1.3f;
    NF_CHECK(app.set_vehicle(e, spec, err));
    const auto* comp = world.get<physics::VehicleComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_NEAR(comp->config.chassis_mass, 1800.0f, 1e-4f);
        NF_CHECK_NEAR(comp->config.max_steer_deg, 25.0f, 1e-4f);
        NF_CHECK_NEAR(comp->config.tire_peak_friction, 1.3f, 1e-5f);
    }
    // The mutator bound the car synchronously: authoring works without a
    // reopen, and the overlay/stats read a live binding.
    NF_CHECK_EQ(app.runtime()->vehicle_count(), static_cast<size_t>(1));

    NF_CHECK(app.detach_vehicle(e, err));
    NF_CHECK(!world.has<physics::VehicleComponent>(e));
    NF_CHECK_EQ(app.runtime()->vehicle_count(), static_cast<size_t>(0));
    err.clear();
    NF_CHECK(!app.detach_vehicle(e, err));
    NF_CHECK(!err.empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_vehicle_debug_input_is_live_only) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = vehicle_temp_dir("nf_ed_vehicle_input");
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
    NF_CHECK(app.attach_vehicle(e, err));

    // Out-of-range input is refused with a name, not pinned silently.
    err.clear();
    NF_CHECK(!app.set_vehicle_input(e, 2.0f, 0.0f, 0.0f, 0.0f, err));
    NF_CHECK(!err.empty());
    NF_CHECK(app.set_vehicle_input(e, 1.0f, -0.5f, 0.0f, 0.0f, err));
    const auto* comp = world.get<physics::VehicleComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->throttle, 1.0f);
        NF_CHECK_EQ(comp->steer, -0.5f);
    }

    // ...but none of it reaches the file: a reload starts parked.
    NF_CHECK(runtime.save_scene("content://Scenes/Work.nfscene", err));
    auto reloaded = runtime::load_scene_from_vfs(vfs, "content://Scenes/Work.nfscene");
    NF_CHECK(reloaded.success);
    const auto* back = reloaded.scene->world().get<physics::VehicleComponent>(e);
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK_EQ(back->throttle, 0.0f);
        NF_CHECK_EQ(back->steer, 0.0f);
        NF_CHECK_EQ(back->brake, 0.0f);
        NF_CHECK_EQ(back->handbrake, 0.0f);
        NF_CHECK(back->vehicle == nullptr);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_play_stop_preserves_the_car) {
    // The data-loss class Step 11 closed in the PlayMode copiers: clone and
    // restore used to know nothing about VehicleComponent (or NavMesh), so a
    // play/stop cycle deleted the author's car from the edit scene.
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = vehicle_temp_dir("nf_ed_vehicle_play");
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
    physics::VehicleComponent spec;
    spec.config.chassis_mass = 1800.0f;
    NF_CHECK(app.set_vehicle(e, spec, err));
    NF_CHECK(app.set_vehicle_input(e, 1.0f, 0.0f, 0.0f, 0.0f, err));

    NF_CHECK(app.play(err));
    NF_CHECK(app.stop(err));

    // The car survives with its tune; the live session state does not.
    const auto* comp = world.get<physics::VehicleComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_NEAR(comp->config.chassis_mass, 1800.0f, 1e-4f);
        NF_CHECK_EQ(comp->throttle, 0.0f);
    }
    // ...and it is bound again from the restored tune (Stop re-binds, like
    // it rebuilds physics): the overlay keeps drawing it.
    NF_CHECK_EQ(app.runtime()->vehicle_count(), static_cast<size_t>(1));

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_play_stop_preserves_navigation) {
    // Same copier gap, one phase earlier: NavMesh/NavAgent components were
    // dropped by the same two copiers. Pinned here because the vehicle work
    // is what exposed it.
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = vehicle_temp_dir("nf_ed_nav_play");
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
    nm.area_x = 12.0f;
    nm.area_y = 8.0f;
    nm.area_z = 12.0f;
    NF_CHECK(app.set_navmesh(e, nm, err));
    NF_CHECK(app.attach_nav_agent(e, err));

    NF_CHECK(app.play(err));
    NF_CHECK(app.stop(err));

    NF_CHECK(world.has<runtime::NavMeshComponent>(e));
    NF_CHECK(world.has<runtime::NavAgentComponent>(e));
    const auto* back = world.get<runtime::NavMeshComponent>(e);
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK_NEAR(back->area_x, 12.0f, 1e-5f);
    }

    std::filesystem::remove_all(tmp);
}