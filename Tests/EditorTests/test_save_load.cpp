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
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
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
