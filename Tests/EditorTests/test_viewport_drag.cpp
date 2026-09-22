// Editor viewport drag: moving the selection with the pointer (Unity-style).
//
// The gizmo math itself (delta mapping, single-command commit) is covered in
// the gizmo unit tests. These tests drive the app-level gesture state
// machine headlessly: press hit-tests + selects, drag live-moves, release
// folds exactly one undo step, a click without movement folds nothing, a
// miss deselects, and abort restores the start transform.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Gizmo.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>

#include <cmath>
#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

editor::ViewCamera drag_camera() {
    editor::ViewCamera vc;
    vc.px = 0.0f;
    vc.py = 0.0f;
    vc.pz = 5.0f;
    vc.tx = 0.0f;
    vc.ty = 0.0f;
    vc.tz = 0.0f;
    vc.fov_y_deg = 60.0f;
    vc.aspect = 1.0f;
    vc.near_plane = 0.1f;
    vc.far_plane = 100.0f;
    return vc;
}

// One cube at the origin, wired through the asset manager so CPU picking
// sees its bounds. Returns the entity.
ecs::Entity setup_drag_scene(VirtualFileSystem& vfs, AssetRegistry& reg, AssetManager& manager,
                             runtime::Runtime& runtime, editor::EditorApp& app, std::string& err) {
    (void)runtime; // scene goes through app.open_scene; runtime only renders
    auto cube = rendering::StaticMesh::create_cube(2.0f);
    const AssetId mesh_id = AssetId::generate();
    auto asset = rendering::make_mesh_asset(*cube, mesh_id, "content://Meshes/cube.nfmesh");
    std::vector<uint8_t> bytes;
    asset->save_to_bytes(bytes);
    if (!vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(bytes)).ok) {
        return ecs::kInvalidEntity;
    }
    AssetMetadata meta;
    meta.id = mesh_id;
    meta.type = AssetType::Mesh;
    meta.logical_path = "content://Meshes/cube.nfmesh";
    meta.cooked_path = "cache://Meshes/cube.nfmesh";
    if (!reg.add(meta, err)) {
        return ecs::kInvalidEntity;
    }
    auto handle = manager.load_mesh_sync(mesh_id);
    if (!handle || handle->state != AssetState::Ready) {
        err = "mesh sync load failed";
        return ecs::kInvalidEntity;
    }
    manager.update();

    scene::Scene scene("Drag");
    auto& w = scene.world();
    // GpuPicker can't represent entity id 0: pack_pick_id adds 1 so the clear
    // value can't alias a real id, but unpack subtracts 1 unconditionally and
    // pick() only reports a hit for a non-zero id, so the first entity in a
    // fresh world round-trips through the pick image as "nothing drawn here".
    // Reserving id 0 keeps this fixture's cube pickable; the engine-side gap
    // is reported in .workbuddy-ai/COORDINATION.md.
    ecs::Entity placeholder = w.create_entity();
    w.add<scene::NameComponent>(placeholder, scene::NameComponent{"Placeholder"});
    ecs::Entity e = w.create_entity();
    w.add<scene::Transform>(e, scene::Transform{});
    runtime::MeshComponent mc;
    mc.mesh_id = mesh_id;
    mc.material = "content://Materials/Default";
    w.add<runtime::MeshComponent>(e, mc);
    if (!runtime::save_scene_to_vfs(vfs, "content://Scenes/Drag.nfscene", scene, err)) {
        return ecs::kInvalidEntity;
    }
    if (!app.open_scene("content://Scenes/Drag.nfscene", err)) {
        return ecs::kInvalidEntity;
    }
    for (ecs::Entity found : app.world()->all_entities()) {
        if (app.world()->has<runtime::MeshComponent>(found)) {
            return found;
        }
    }
    err = "entity missing after open";
    return ecs::kInvalidEntity;
}

} // namespace

NF_TEST(viewport_ndc_to_pixel_maps_ndc_up_to_the_image_top) {
    // Pins the mapping the GPU picker depends on. Mat4::perspective negates
    // m[1][1] (the Vulkan Y-flip), so world-up/window-up NDC +Y lands in
    // LOW-numbered framebuffer rows — pinned independently by
    // render_memory_rows_follow_vulkan_top_left_origin and RHITests'
    // lighting_pass. Picking passes through unflipped used to address the
    // mirror image of the click while the centre still hit.
    float x = -1.0f, y = -1.0f;
    editor::viewport_ndc_to_pixel(0.0f, 0.0f, 64.0f, 64.0f, x, y);
    NF_CHECK_NEAR(x, 32.0f, 1e-5f);
    NF_CHECK_NEAR(y, 32.0f, 1e-5f);
    editor::viewport_ndc_to_pixel(-1.0f, -1.0f, 64.0f, 64.0f, x, y);
    NF_CHECK_NEAR(x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(y, 64.0f, 1e-5f);
    editor::viewport_ndc_to_pixel(1.0f, 1.0f, 64.0f, 64.0f, x, y);
    NF_CHECK_NEAR(x, 64.0f, 1e-5f);
    NF_CHECK_NEAR(y, 0.0f, 1e-5f);
    editor::viewport_ndc_to_pixel(0.0f, 0.5f, 64.0f, 64.0f, x, y);
    NF_CHECK_NEAR(x, 32.0f, 1e-5f);
    NF_CHECK_NEAR(y, 16.0f, 1e-5f); // upper NDC -> upper rows (row 0 is the image top)
}

NF_TEST(viewport_pick_hits_the_pixel_the_ndc_addresses) {
    // Regression for the mirrored GPU pick: the pixel row handed to the picker
    // used to be the vertical mirror of the click, so a click in the empty half
    // of the viewport grabbed geometry from the other half. Everything near the
    // centre still worked, which is why this hid. The cube sits ABOVE the view
    // centre — the same fixture render_memory_rows_follow_vulkan_top_left_origin
    // uses — so "above" is low-numbered framebuffer rows.
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_pick_orient");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    const ecs::Entity cube = setup_drag_scene(vfs, reg, manager, runtime, app, err);
    NF_CHECK(cube.valid());

    // The fixture scene has no camera, so extract_camera would fall back to a
    // default eye at (0,2,5) — not the (0,0,5) drag_camera() the NDC maths
    // below assume. Render through the camera the assertions compute with, or
    // the raised cube lands at a different row than the comments claim and the
    // pick is empty for a reason that has nothing to do with the mirror.
    {
        const ecs::Entity cam_e = app.world()->create_entity();
        scene::Transform ct{};
        ct.local_z = 5.0f;
        app.world()->add<scene::Transform>(cam_e, ct);
        runtime::CameraComponent cc;
        cc.fov_y = 60.0f;
        cc.aspect = 1.0f;
        cc.near_plane = 0.1f;
        cc.far_plane = 100.0f;
        cc.is_active = true;
        app.world()->add<runtime::CameraComponent>(cam_e, cc);
        runtime.update(0.016f); // local -> world, so extract_camera sees (0,0,5)
    }

    // Move the cube up and let the runtime propagate, so both the render and the
    // CPU bounds agree on where it is.
    app.world()->get<scene::Transform>(cube)->local_y = 2.5f;
    runtime.update(0.016f);
    NF_CHECK_NEAR(app.world()->get<scene::Transform>(cube)->world_y, 2.5f, 1e-5f);
    app.viewport().width = 64;
    app.viewport().height = 64;

    // One 64x64 frame: this is what sizes the renderer, the picker and the
    // mapping under test.
    rhi::TextureDesc td{};
    td.width = 64;
    td.height = 64;
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
    auto target = device.create_texture(td);
    NF_CHECK(target != nullptr);
    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    NF_CHECK(cmd != nullptr && fence != nullptr);
    cmd->begin();
    runtime.render_offscreen(*target, *cmd);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));

    const editor::ViewCamera vc = drag_camera();
    // NDC (0, +0.6) is the upper half: row 12 of 64, inside the cube.
    NF_CHECK(app.pick(vc, 0.0f, 0.6f) == cube);
    // NDC (0, -0.6) is the lower half: haze, nothing to pick. A mirrored row
    // resolves this click to the cube and the grab lands on the far side.
    NF_CHECK(app.pick(vc, 0.0f, -0.6f) == ecs::kInvalidEntity);

    // The picker's own coordinates, without the CPU fallback in the way.
    float px = 0.0f, py = 0.0f;
    editor::viewport_ndc_to_pixel(0.0f, 0.6f, 64.0f, 64.0f, px, py);
    uint32_t picked = 0;
    NF_CHECK(runtime.pick_entity_gpu(static_cast<uint32_t>(px), static_cast<uint32_t>(py), picked));
    NF_CHECK_EQ(picked, cube.id);
    editor::viewport_ndc_to_pixel(0.0f, -0.6f, 64.0f, 64.0f, px, py);
    uint32_t empty = 0;
    NF_CHECK(!runtime.pick_entity_gpu(static_cast<uint32_t>(px), static_cast<uint32_t>(py), empty));

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(viewport_drag_moves_selection_as_one_undo) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_viewdrag");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    const ecs::Entity cube = setup_drag_scene(vfs, reg, manager, runtime, app, err);
    NF_CHECK(cube.valid());

    const editor::ViewCamera vc = drag_camera();
    NF_CHECK(app.viewport_press(0.0f, 0.0f, vc, false, err));
    NF_CHECK(app.selection().primary() == cube);
    NF_CHECK(app.viewport_dragging());

    // 0.2 NDC right at distance 5, fov 60, aspect 1 moves ~0.577 world units.
    NF_CHECK(app.viewport_drag(0.2f, 0.0f, vc, err));
    const float moved_x = app.world()->get<scene::Transform>(cube)->local_x;
    NF_CHECK(moved_x > 0.3f && moved_x < 1.0f);
    // Pure horizontal gesture: no vertical drift (the reported symptom was a
    // slow upward creep while moving sideways).
    NF_CHECK(std::abs(app.world()->get<scene::Transform>(cube)->local_y) < 0.05f);

    NF_CHECK(app.viewport_release(err));
    NF_CHECK(!app.viewport_dragging());
    // Exactly one undo step for the whole gesture...
    NF_CHECK_EQ(app.stack().undo_size(), 1u);
    // ...and it restores the start, then redoes the drag.
    NF_CHECK(app.undo(err));
    NF_CHECK_NEAR(app.world()->get<scene::Transform>(cube)->local_x, 0.0f, 1e-5f);
    NF_CHECK(app.redo(err));
    NF_CHECK_NEAR(app.world()->get<scene::Transform>(cube)->local_x, moved_x, 1e-5f);

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(viewport_click_without_drag_adds_no_undo) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_viewclick");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    const ecs::Entity cube = setup_drag_scene(vfs, reg, manager, runtime, app, err);
    NF_CHECK(cube.valid());

    const editor::ViewCamera vc = drag_camera();
    NF_CHECK(app.viewport_press(0.0f, 0.0f, vc, false, err));
    NF_CHECK(app.selection().primary() == cube);
    NF_CHECK(app.viewport_release(err));
    NF_CHECK(!app.viewport_dragging());
    // Selected, unmoved, and the undo stack is untouched.
    NF_CHECK_EQ(app.stack().undo_size(), 0u);
    NF_CHECK_NEAR(app.world()->get<scene::Transform>(cube)->local_x, 0.0f, 1e-5f);

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(viewport_press_miss_clears_selection) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_viewmiss");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    const ecs::Entity cube = setup_drag_scene(vfs, reg, manager, runtime, app, err);
    NF_CHECK(cube.valid());

    const editor::ViewCamera vc = drag_camera();
    NF_CHECK(app.viewport_press(0.0f, 0.0f, vc, false, err));
    NF_CHECK(app.selection().has_selection());
    // Far corner ray misses the 2-unit cube entirely.
    NF_CHECK(app.viewport_press(0.9f, 0.9f, vc, false, err));
    NF_CHECK(!app.selection().has_selection());
    NF_CHECK(!app.viewport_dragging());

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(viewport_abort_restores_start_transform) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_viewabort");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    const ecs::Entity cube = setup_drag_scene(vfs, reg, manager, runtime, app, err);
    NF_CHECK(cube.valid());

    const editor::ViewCamera vc = drag_camera();
    NF_CHECK(app.viewport_press(0.0f, 0.0f, vc, false, err));
    NF_CHECK(app.viewport_drag(0.2f, 0.0f, vc, err));
    NF_CHECK(app.world()->get<scene::Transform>(cube)->local_x > 0.3f);
    NF_CHECK(app.viewport_abort_drag(err));
    NF_CHECK(!app.viewport_dragging());
    // Start restored, and no undo entry for a gesture the user cancelled.
    NF_CHECK_NEAR(app.world()->get<scene::Transform>(cube)->local_x, 0.0f, 1e-5f);
    NF_CHECK_EQ(app.stack().undo_size(), 0u);

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}
