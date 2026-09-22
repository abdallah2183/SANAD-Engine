// Editor save/load: modifications through EditorApp survive a save/reload.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/PlayMode.hpp>
#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Transform.hpp>

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

// The shipped Content/ tree, found by walking up from the working directory so
// the test works whether it is launched from the repo root or from the bin dir.
static std::filesystem::path find_repo_content() {
    std::filesystem::path p = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(p / "Content" / "Scenes" / "Example.nfscene")) {
            return p / "Content";
        }
        if (!p.has_parent_path()) {
            break;
        }
        p = p.parent_path();
    }
    return {};
}

// --- A5: the shipped default scene is a room, not an empty world -------------
//
// The lead's report was "no ground": Example.nfscene had the camera AT the
// origin (degenerate — it only rendered because the viewport falls back to a
// (0,2,5) camera), a cube sitting at the origin, and no floor at all. These
// tests pin the corrected shape and the fact that it survives a .nfscene
// round trip at 9 significant digits.

NF_TEST(editor_default_scene_has_ground_cube_camera) {
    const std::filesystem::path content = find_repo_content();
    NF_CHECK(!content.empty());

    VirtualFileSystem vfs;
    vfs.mount("content://", content);
    auto loaded = runtime::load_scene_from_vfs(vfs, "content://Scenes/Example.nfscene");
    NF_CHECK(loaded.success);
    NF_CHECK(loaded.scene != nullptr);
    const ecs::World& w = loaded.scene->world();

    ecs::Entity cam = ecs::kInvalidEntity;
    ecs::Entity light = ecs::kInvalidEntity;
    ecs::Entity ground = ecs::kInvalidEntity;
    ecs::Entity cube = ecs::kInvalidEntity;
    for (ecs::Entity e : w.all_entities()) {
        if (const auto* c = w.get<runtime::CameraComponent>(e); c != nullptr && c->is_active) {
            cam = e;
        }
        if (w.get<runtime::DirectionalLight>(e) != nullptr) {
            light = e;
        }
        if (const auto* m = w.get<runtime::MeshComponent>(e); m != nullptr) {
            const auto* t = w.get<scene::Transform>(e);
            if (t == nullptr) {
                continue;
            }
            // A large, thin, flat mesh is the floor; a compact one is the cube.
            if (t->scale_x >= 10.0f && t->scale_z >= 10.0f && t->scale_y <= 1.0f) {
                ground = e;
            } else {
                cube = e;
            }
        }
    }

    NF_CHECK(cam.valid());
    NF_CHECK(light.valid());
    NF_CHECK(ground.valid());   // "no ground" was the report
    NF_CHECK(cube.valid());

    // The camera is off the floor and back from the origin, not degenerate.
    const auto* ct = w.get<scene::Transform>(cam);
    NF_CHECK(ct != nullptr);
    NF_CHECK_NEAR(ct->local_x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(ct->local_y, 2.0f, 1e-6f);
    NF_CHECK_NEAR(ct->local_z, 5.0f, 1e-6f);
    // ...and pitched down, so it actually looks at the origin.
    NF_CHECK(ct->rot_x < 0.0f);

    // The floor sits at y = 0 and is static.
    const auto* gt = w.get<scene::Transform>(ground);
    NF_CHECK(gt != nullptr);
    NF_CHECK_NEAR(gt->local_y, 0.0f, 1e-6f);
    const auto* grb = w.get<physics::RigidBodyComponent>(ground);
    NF_CHECK(grb != nullptr);
    NF_CHECK(grb->type == physics::BodyType::Static);

    // The cube rests ON the floor: above y = 0, below the floor's half-thickness
    // plus its own half-extent. A cube at the origin (the old file) fails this.
    const auto* ctb = w.get<scene::Transform>(cube);
    NF_CHECK(ctb != nullptr);
    NF_CHECK(ctb->local_y > 0.0f);
    NF_CHECK(ctb->local_y <= 1.0f);
}

NF_TEST(editor_default_scene_round_trips_at_nine_digits) {
    const std::filesystem::path content = find_repo_content();
    NF_CHECK(!content.empty());

    VirtualFileSystem vfs;
    vfs.mount("content://", content);
    auto loaded = runtime::load_scene_from_vfs(vfs, "content://Scenes/Example.nfscene");
    NF_CHECK(loaded.success);

    // Save it back out, reload, and compare the values that must be exact.
    //
    // The scratch tree gets its OWN scheme. Mounting content:// a second time
    // does NOT replace the first mount, so an earlier version of this test wrote
    // Rt.nfscene straight into the repository's Content/Scenes — a test must
    // never leave files in the tree it is testing.
    const auto tmp = temp_dir_for("nf_ed_default_roundtrip");
    std::filesystem::create_directories(tmp / "Scenes");
    vfs.mount("rt://", tmp);
    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "rt://Scenes/Rt.nfscene", *loaded.scene, err));

    auto again = runtime::load_scene_from_vfs(vfs, "rt://Scenes/Rt.nfscene");
    NF_CHECK(again.success);
    NF_CHECK(again.scene != nullptr);
    NF_CHECK(again.scene->world().alive_entity_count() ==
             loaded.scene->world().alive_entity_count());

    // 0.1 (the floor's y scale) is the value a %.6g writer would drift on, so it
    // is the one worth pinning at full f32 precision.
    for (ecs::Entity e : again.scene->world().all_entities()) {
        const auto* m = again.scene->world().get<runtime::MeshComponent>(e);
        const auto* t = again.scene->world().get<scene::Transform>(e);
        if (m != nullptr && t != nullptr && t->scale_x >= 10.0f) {
            NF_CHECK(t->scale_y == 0.1f);
        }
    }
}

// --- A6: the sky preset dropdown actually changes the sky --------------------
//
// Two defects sat behind the report. The combo's item pointers dangled (A2 — the
// "????" boxes), and picking a preset on a scene that had no sky yet created a
// sky carrying the DEFAULT palette, so the dropdown changed nothing visible.
// `sky_preset_edit()` is the production path the toolbar calls; driving it here
// pins both that the three palettes differ and that they reach the component
// even when the scene started with no sky at all.

NF_TEST(editor_sky_preset_reaches_a_scene_that_had_no_sky) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_sky_preset");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    scene::Scene scene("NoSky");
    ecs::Entity only = scene.world().create_entity();
    scene.world().add<scene::Transform>(only, scene::Transform{});
    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/NoSky.nfscene", scene, err));

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);
    NF_CHECK(app.open_scene("content://Scenes/NoSky.nfscene", err));
    NF_CHECK(app.world()->query<runtime::SkyComponent>().empty()); // the premise

    // The three palettes must be distinguishable, or "pick night" cannot change
    // anything on screen.
    const runtime::SkyComponent day = editor::sky_preset_values(0);
    const runtime::SkyComponent dusk = editor::sky_preset_values(1);
    const runtime::SkyComponent night = editor::sky_preset_values(2);
    NF_CHECK(day.horizon_r != dusk.horizon_r);
    NF_CHECK(dusk.horizon_r != night.horizon_r);
    NF_CHECK(day.horizon_r != night.horizon_r);
    NF_CHECK(night.horizon_r < day.horizon_r);  // night is darker...
    NF_CHECK(night.zenith_b > night.zenith_r);  // ...and blue-shifted
    NF_CHECK(night.sun_disk < day.sun_disk);
    NF_CHECK(night.sun_glow < day.sun_glow);

    // Apply "night" through the path the toolbar uses.
    NF_CHECK(app.apply_sky_edit(editor::sky_preset_edit(2), err));

    const std::vector<ecs::Entity> skies = app.world()->query<runtime::SkyComponent>();
    NF_CHECK(skies.size() == 1u);
    const auto* installed = app.world()->get<runtime::SkyComponent>(skies.front());
    NF_CHECK(installed != nullptr);
    // The regression: all three of these used to hold the DEFAULT palette.
    NF_CHECK(installed->horizon_r == night.horizon_r);
    NF_CHECK(installed->zenith_r == night.zenith_r);
    NF_CHECK(installed->sun_disk == night.sun_disk);

    // Switching to golden hour updates the SAME sky instead of stacking another.
    NF_CHECK(app.apply_sky_edit(editor::sky_preset_edit(1), err));
    const std::vector<ecs::Entity> after = app.world()->query<runtime::SkyComponent>();
    NF_CHECK(after.size() == 1u);
    const auto* updated = app.world()->get<runtime::SkyComponent>(after.front());
    NF_CHECK(updated != nullptr);
    NF_CHECK(updated->horizon_r == dusk.horizon_r);
    NF_CHECK(updated->horizon_r != night.horizon_r);
}

NF_TEST(editor_save_load_after_mods) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_saveload");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    auto cube = rendering::StaticMesh::create_cube(2.0f);
    const AssetId mesh_id = AssetId::generate();
    auto asset = rendering::make_mesh_asset(*cube, mesh_id, "content://Meshes/cube.nfmesh");
    std::vector<uint8_t> bytes;
    asset->save_to_bytes(bytes);
    NF_CHECK(vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(bytes)).ok);
    AssetRegistry reg;
    AssetMetadata meta;
    meta.id = mesh_id;
    meta.type = AssetType::Mesh;
    meta.logical_path = "content://Meshes/cube.nfmesh";
    meta.cooked_path = "cache://Meshes/cube.nfmesh";
    std::string err;
    NF_CHECK(reg.add(meta, err));

    scene::Scene scene("SaveLoad");
    auto& w = scene.world();
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z = 5.0f;
    runtime::CameraComponent cam;
    cam.is_active = true;
    w.add<runtime::CameraComponent>(cam_e, cam);
    w.add<scene::NameComponent>(cam_e, scene::NameComponent{"Camera"});
    ecs::Entity mesh_e = w.create_entity();
    w.add<scene::Transform>(mesh_e, scene::Transform{});
    runtime::MeshComponent mc;
    mc.mesh_id = mesh_id;
    mc.material = "content://Materials/Default";
    w.add<runtime::MeshComponent>(mesh_e, mc);
    w.add<scene::NameComponent>(mesh_e, scene::NameComponent{"Cube"});
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/Work.nfscene", scene, err));

    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    NF_CHECK(!app.dirty());

    // Modify through the app (rename + transform), then save a copy.
    auto cube_e = editor::find_by_name(*runtime.scene(), "Cube");
    NF_CHECK(cube_e.has_value());
    NF_CHECK(app.rename_entity(*cube_e, "EditedCube", err));
    NF_CHECK(app.dirty());
    editor::TransformEdit te = editor::read_transform(*app.world(), *cube_e);
    te.px = 1.5f;
    NF_CHECK(app.set_transform(*cube_e, te, err));

    NF_CHECK(app.save_copy("cache://Edited.nfscene", err));
    auto reloaded = runtime::load_scene_from_vfs(vfs, "cache://Edited.nfscene");
    NF_CHECK(reloaded.success);
    std::string diff;
    NF_CHECK(editor::scenes_equal_structure(*runtime.scene(), *reloaded.scene, diff));
    auto edited = editor::find_by_name(*reloaded.scene, "EditedCube");
    NF_CHECK(edited.has_value());
    NF_CHECK_NEAR(reloaded.scene->world().get<scene::Transform>(*edited)->local_x, 1.5f, 1e-5f);

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}
