// Editor materials: .nfmat round-trip, assignment undo, GPU-visible parameter
// edits, missing-file fallback, and save/reload.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Inspector.hpp>
#include <NF/Rendering/MaterialAsset.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>

#include <filesystem>
#include <limits>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

// Minimal 24-bit BMP writer (solid color) — avoids any test-time encoder dep.
static std::vector<uint8_t> make_bmp_solid(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    const int stride = ((w * 3 + 3) / 4) * 4;
    std::vector<uint8_t> out(static_cast<size_t>(54) + static_cast<size_t>(stride) * static_cast<size_t>(h),
                             0);
    auto put32 = [&](size_t off, uint32_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    auto put16 = [&](size_t off, uint16_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    put16(26, 1);
    put16(28, 24);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = out.data() + 54 + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(stride);
        for (int x = 0; x < w; ++x) {
            row[x * 3] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
    }
    return out;
}

NF_TEST(material_asset_roundtrip) {
    rendering::MaterialAsset a;
    a.name = "TestMat";
    a.params.base_color[0] = 0.9f;
    a.params.base_color[1] = 0.1f;
    a.params.base_color[2] = 0.1f;
    a.params.metallic = 1.0f;
    a.params.roughness = 0.25f;
    const std::string text = a.save_to_text();
    rendering::MaterialAsset b;
    std::string err;
    NF_CHECK(rendering::MaterialAsset::load_from_text(text, b, err));
    NF_CHECK(b.name == "TestMat");
    NF_CHECK_NEAR(b.params.base_color[0], 0.9f, 1e-6f);
    NF_CHECK_NEAR(b.params.metallic, 1.0f, 1e-6f);
    NF_CHECK_NEAR(b.params.roughness, 0.25f, 1e-6f);

    // Tolerant: unknown keys ignored, missing keys keep defaults.
    rendering::MaterialAsset c;
    NF_CHECK(rendering::MaterialAsset::load_from_text(
        "# NOVAForge Material v1\nname: Sparse\nfuture_key: 42\nroughness: 0.5\n", c, err));
    NF_CHECK(c.name == "Sparse");
    NF_CHECK_NEAR(c.params.roughness, 0.5f, 1e-6f);
    NF_CHECK_NEAR(c.params.metallic, 0.0f, 1e-6f);

    // Not a material at all: hard failure, never garbage.
    rendering::MaterialAsset d;
    NF_CHECK(!rendering::MaterialAsset::load_from_text("hello world", d, err));
    NF_CHECK(!err.empty());

    // mip: key round-trips; silence keeps Linear; unknown values are ignored.
    rendering::MaterialAsset m;
    m.mip_mode = rhi::MipMapMode::None;
    rendering::MaterialAsset m2;
    NF_CHECK(rendering::MaterialAsset::load_from_text(m.save_to_text(), m2, err));
    NF_CHECK(m2.mip_mode == rhi::MipMapMode::None);
    rendering::MaterialAsset n;
    NF_CHECK(rendering::MaterialAsset::load_from_text(
        "# NOVAForge Material v1\nname: N\nmip: nearest\n", n, err));
    NF_CHECK(n.mip_mode == rhi::MipMapMode::Nearest);
    NF_CHECK(c.mip_mode == rhi::MipMapMode::Linear); // sparse text is silent
    rendering::MaterialAsset u;
    NF_CHECK(rendering::MaterialAsset::load_from_text(
        "# NOVAForge Material v1\nname: U\nmip: bogus\n", u, err));
    NF_CHECK(u.mip_mode == rhi::MipMapMode::Linear);
}

NF_TEST(material_assignment_undo) {
    scene::Scene scene("Assign");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    runtime::MeshComponent mc;
    mc.mesh_id = AssetId::generate();
    mc.material = "content://Materials/Default";
    scene.world().add<runtime::MeshComponent>(e, mc);

    editor::CommandStack stack;
    std::string err;
    auto cmd =
        editor::make_material_assignment_command(scene.world(), e, "content://Materials/Red", err);
    NF_CHECK(cmd != nullptr);
    stack.push(std::move(cmd), scene.world());
    NF_CHECK(scene.world().get<runtime::MeshComponent>(e)->material == "content://Materials/Red");
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(scene.world().get<runtime::MeshComponent>(e)->material == "content://Materials/Default");
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(scene.world().get<runtime::MeshComponent>(e)->material == "content://Materials/Red");

    // Entities without a mesh reject assignment without touching the scene.
    ecs::Entity bare = scene.world().create_entity();
    scene.world().add<scene::Transform>(bare, scene::Transform{});
    NF_CHECK(editor::make_material_assignment_command(scene.world(), bare, "x", err) == nullptr);
    NF_CHECK(!err.empty());
}

NF_TEST(material_params_invalid_rejected) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_matinvalid");
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    // No scene needed: range validation rejects before any renderer touch.
    std::string err;
    editor::MaterialEdit bad;
    bad.roughness = 42.0f;
    NF_CHECK(!app.set_material_params("content://Materials/Default", bad, err));
    NF_CHECK(!err.empty());
    editor::MaterialEdit nan;
    nan.metallic = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(!app.set_material_params("content://Materials/Default", nan, err));
    editor::MaterialEdit neg;
    neg.emission_strength = -1.0f;
    NF_CHECK(!app.set_material_params("content://Materials/Default", neg, err));

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(material_params_gpu_effect) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_matgpu");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Content" / "Materials");
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
    NF_CHECK(vfs.write_text("content://Materials/Default.nfmat",
                            "# NOVAForge Material v1\nname: Default\nbase_color: 0.8 0.8 0.8 1\n"
                            "metallic: 0\nroughness: 0.4\nao: 1\nemission: 0 0 0\n"
                            "emission_strength: 0\n")
                 .ok);

    scene::Scene scene("MatGpu");
    auto& w = scene.world();
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z = 5.0f;
    runtime::CameraComponent cam;
    cam.is_active = true;
    w.add<runtime::CameraComponent>(cam_e, cam);
    ecs::Entity light_e = w.create_entity();
    w.add<runtime::DirectionalLight>(light_e, runtime::DirectionalLight{});
    ecs::Entity mesh_e = w.create_entity();
    w.add<scene::Transform>(mesh_e, scene::Transform{});
    runtime::MeshComponent mc;
    mc.mesh_id = mesh_id;
    mc.material = "content://Materials/Default";
    w.add<runtime::MeshComponent>(mesh_e, mc);
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/Mat.nfscene", scene, err));

    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Mat.nfscene", err));
    auto handle = manager.load_mesh_sync(mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);

    auto render_lit = [&](uint32_t& out_lit, uint64_t& out_red) {
        out_lit = 0;
        out_red = 0;
        rhi::TextureDesc td{};
        td.width = 64;
        td.height = 64;
        td.format = rhi::Format::R8G8B8A8_UNorm;
        td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
        auto target = device.create_texture(td);
        rhi::BufferDesc bd{};
        bd.size = static_cast<usize>(64) * 64 * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryUsage::GPUToCPU;
        auto rb = device.create_buffer(bd);
        auto cmd = device.create_command_buffer();
        auto fence = device.create_fence(false);
        if (!target || !rb || !cmd || !fence) {
            return false;
        }
        cmd->begin();
        runtime.render_offscreen(*target, *cmd);
        cmd->copy_texture_to_buffer(*target, *rb, 0, 0, 64, 64, 0);
        cmd->end();
        device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
        if (!fence->wait(5000000000ULL)) {
            return false;
        }
        const auto* px = static_cast<const uint8_t*>(rb->map());
        if (px == nullptr) {
            return false;
        }
        for (size_t i = 0; i < 64u * 64u; ++i) {
            if (px[i * 4] > 10 || px[i * 4 + 1] > 10 || px[i * 4 + 2] > 10) {
                ++out_lit;
            }
            if (px[i * 4] > px[i * 4 + 1] + 40 && px[i * 4] > px[i * 4 + 2] + 40) {
                ++out_red;
            }
        }
        rb->unmap();
        device.wait_idle();
        return true;
    };

    // Gray default first.
    uint32_t lit0 = 0;
    uint64_t red0 = 0;
    NF_CHECK(render_lit(lit0, red0));
    NF_CHECK(lit0 > 100);

    // Turn the shared material red through the editor path (validated cmd).
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);
    editor::MaterialEdit red;
    red.base_color[0] = 0.9f;
    red.base_color[1] = 0.1f;
    red.base_color[2] = 0.1f;
    red.base_color[3] = 1.0f;
    red.metallic = 0.0f;
    red.roughness = 0.4f;
    NF_CHECK(app.set_material_params("content://Materials/Default", red, err));
    uint32_t lit1 = 0;
    uint64_t red1 = 0;
    NF_CHECK(render_lit(lit1, red1));
    NF_CHECK(red1 > 100); // viewport visibly red now

    // Undo restores gray.
    NF_CHECK(app.undo(err));
    uint32_t lit2 = 0;
    uint64_t red2 = 0;
    NF_CHECK(render_lit(lit2, red2));
    NF_CHECK(red2 * 4 < red1); // red essentially gone

    // Invalid values never reach the renderer.
    editor::MaterialEdit bad;
    bad.roughness = 5.0f;
    NF_CHECK(!app.set_material_params("content://Materials/Default", bad, err));
    NF_CHECK(!err.empty());

    // Missing material file: gray fallback, still renders, no crash.
    runtime::MeshComponent* mmc = runtime.edit_scene()->world().get<runtime::MeshComponent>(mesh_e);
    NF_CHECK(mmc != nullptr);
    mmc->material = "content://Materials/DoesNotExist";
    runtime.mark_scene_edited();
    uint32_t lit3 = 0;
    uint64_t red3 = 0;
    NF_CHECK(render_lit(lit3, red3));
    NF_CHECK(lit3 > 100);
    (void)red0;
    (void)lit2;
    (void)red3;

    // Save edited params (re-apply red first) and reload from disk.
    NF_CHECK(app.set_material_params("content://Materials/Default", red, err));
    NF_CHECK(app.materials_dirty()); // the edit dirtied the material
    NF_CHECK(runtime.save_material("content://Materials/Default", "cache://RedCopy.nfmat", err));
    auto rd = vfs.read_text("cache://RedCopy.nfmat");
    NF_CHECK(rd.ok);
    rendering::MaterialAsset reparsed;
    NF_CHECK(rendering::MaterialAsset::load_from_text(rd.value, reparsed, err));
    NF_CHECK_NEAR(reparsed.params.base_color[0], 0.9f, 1e-5f);
    NF_CHECK_NEAR(reparsed.params.base_color[1], 0.1f, 1e-5f);

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(material_albedo_gpu_effect) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_matalbedo");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Content" / "Materials");
    std::filesystem::create_directories(tmp / "Content" / "Textures");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    // Solid red 4x4 texture + gray material referencing it.
    const auto bmp = make_bmp_solid(4, 4, 255, 0, 0);
    NF_CHECK(vfs.write_bytes("content://Textures/red.bmp", std::span<const uint8_t>(bmp)).ok);
    NF_CHECK(vfs.write_text("content://Materials/Default.nfmat",
                            "# NOVAForge Material v1\nname: Default\nbase_color: 0.8 0.8 0.8 1\n"
                            "metallic: 0\nroughness: 0.4\nao: 1\nemission: 0 0 0\n"
                            "emission_strength: 0\nalbedo: content://Textures/red.bmp\n")
                 .ok);

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

    scene::Scene scene("MatAlbedo");
    auto& w = scene.world();
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z = 5.0f;
    runtime::CameraComponent cam;
    cam.is_active = true;
    w.add<runtime::CameraComponent>(cam_e, cam);
    ecs::Entity light_e = w.create_entity();
    w.add<runtime::DirectionalLight>(light_e, runtime::DirectionalLight{});
    ecs::Entity mesh_e = w.create_entity();
    w.add<scene::Transform>(mesh_e, scene::Transform{});
    runtime::MeshComponent mc;
    mc.mesh_id = mesh_id;
    mc.material = "content://Materials/Default";
    w.add<runtime::MeshComponent>(mesh_e, mc);
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/Alb.nfscene", scene, err));

    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Alb.nfscene", err));
    auto handle = manager.load_mesh_sync(mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);

    auto render_red = [&]() -> uint64_t {
        rhi::TextureDesc td{};
        td.width = 64;
        td.height = 64;
        td.format = rhi::Format::R8G8B8A8_UNorm;
        td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
        auto target = device.create_texture(td);
        rhi::BufferDesc bd{};
        bd.size = static_cast<usize>(64) * 64 * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryUsage::GPUToCPU;
        auto rb = device.create_buffer(bd);
        auto cmd = device.create_command_buffer();
        auto fence = device.create_fence(false);
        if (!target || !rb || !cmd || !fence) {
            return 0;
        }
        cmd->begin();
        runtime.render_offscreen(*target, *cmd);
        cmd->copy_texture_to_buffer(*target, *rb, 0, 0, 64, 64, 0);
        cmd->end();
        device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
        if (!fence->wait(5000000000ULL)) {
            return 0;
        }
        const auto* px = static_cast<const uint8_t*>(rb->map());
        if (px == nullptr) {
            return 0;
        }
        uint64_t red = 0;
        for (size_t i = 0; i < 64u * 64u; ++i) {
            if (px[i * 4] > px[i * 4 + 1] + 40 && px[i * 4] > px[i * 4 + 2] + 40) {
                ++red;
            }
        }
        rb->unmap();
        device.wait_idle();
        return red;
    };

    // File-declared albedo applies on load: red-dominant cube.
    NF_CHECK(render_red() > 100);

    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    // Unbind through the editor path: back to scalar gray.
    NF_CHECK(app.set_material_albedo("content://Materials/Default", "", err));
    NF_CHECK(app.material_albedo("content://Materials/Default").empty());
    NF_CHECK(render_red() < 50);
    NF_CHECK(app.undo(err)); // rebinds red
    NF_CHECK(app.material_albedo("content://Materials/Default") == "content://Textures/red.bmp");
    NF_CHECK(render_red() > 100);

    // Missing texture file: clean rejection, binding untouched.
    NF_CHECK(!app.set_material_albedo("content://Materials/Default", "content://Textures/nope.png", err));
    NF_CHECK(!err.empty());
    NF_CHECK(app.material_albedo("content://Materials/Default") == "content://Textures/red.bmp");

    // Save round-trip preserves the albedo line.
    NF_CHECK(runtime.save_material("content://Materials/Default", "cache://AlbCopy.nfmat", err));
    auto rd = vfs.read_text("cache://AlbCopy.nfmat");
    NF_CHECK(rd.ok);
    rendering::MaterialAsset reparsed;
    NF_CHECK(rendering::MaterialAsset::load_from_text(rd.value, reparsed, err));
    NF_CHECK(reparsed.albedo == "content://Textures/red.bmp");

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(material_mip_mode_save_reload) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_matmip");
    std::filesystem::create_directories(tmp / "Content" / "Materials");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    NF_CHECK(vfs.write_text("content://Materials/M.nfmat",
                            "# NOVAForge Material v1\nname: M\nbase_color: 0.8 0.8 0.8 1\n"
                            "metallic: 0\nroughness: 0.4\nao: 1\nemission: 0 0 0\n"
                            "emission_strength: 0\n")
                 .ok);

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    std::string err;
    NF_CHECK(runtime.material_for_path("content://Materials/M").valid());
    // A silent file loads as Linear.
    NF_CHECK(runtime.material_mip_mode("content://Materials/M") == rhi::MipMapMode::Linear);

    // Mode change persists through save (albedo-less: no texture upload needed).
    NF_CHECK(runtime.set_material_mip_mode("content://Materials/M", rhi::MipMapMode::Nearest, err));
    NF_CHECK(runtime.save_material("content://Materials/M", "cache://MCopy.nfmat", err));
    auto rd = vfs.read_text("cache://MCopy.nfmat");
    NF_CHECK(rd.ok);
    rendering::MaterialAsset reparsed;
    NF_CHECK(rendering::MaterialAsset::load_from_text(rd.value, reparsed, err));
    NF_CHECK(reparsed.mip_mode == rhi::MipMapMode::Nearest);

    // Saving over the source clears the dirty flag; a later external edit to
    // the mip key is then adopted by hot reload (no unsaved edits to protect).
    NF_CHECK(runtime.save_material("content://Materials/M", "content://Materials/M.nfmat", err));
    NF_CHECK(vfs.write_text("content://Materials/M.nfmat",
                            "# NOVAForge Material v1\nname: M\nbase_color: 0.8 0.8 0.8 1\n"
                            "metallic: 0\nroughness: 0.4\nao: 1\nemission: 0 0 0\n"
                            "emission_strength: 0\nmip: none\n")
                 .ok);
    NF_CHECK(runtime.reload_material_file("content://Materials/M", err));
    NF_CHECK(runtime.material_mip_mode("content://Materials/M") == rhi::MipMapMode::None);

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

// The material descriptor set is cached on the material instance and reused
// across frames. Before the cache the renderer allocated one set per visible
// object per frame, so a scene whose objects share a material paid N
// allocations and N vkUpdateDescriptorSets for what is one binding.
//
// This asserts the cache directly via Renderer3D::Stats::material_sets_built.
// A timing benchmark cannot make this distinction — "fast enough" and "cached"
// look identical in a microsecond number.
NF_TEST(material_descriptor_set_is_cached) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_matcache");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Content" / "Materials");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    // A real material file, so no gray fallback instance is created and the
    // expected set count is unambiguous.
    NF_CHECK(vfs.write_text("content://Materials/Shared.nfmat",
                            "# NOVAForge Material v1\nname: Shared\n"
                            "base_color: 0.8 0.2 0.2 1\nmetallic: 0\nroughness: 0.4\n"
                            "ao: 1\nemission: 0 0 0\nemission_strength: 0\n")
                 .ok);

    auto cube = rendering::StaticMesh::create_cube(1.0f);
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

    constexpr int kObjects = 6;

    scene::Scene scene("MatCache");
    auto& w = scene.world();
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z = 8.0f;
    runtime::CameraComponent cam;
    cam.is_active = true;
    w.add<runtime::CameraComponent>(cam_e, cam);
    ecs::Entity light_e = w.create_entity();
    w.add<runtime::DirectionalLight>(light_e, runtime::DirectionalLight{});
    // Every entity points at the SAME material path, so all of them resolve to
    // one shared instance.
    for (int i = 0; i < kObjects; ++i) {
        ecs::Entity e = w.create_entity();
        w.add<scene::Transform>(e, scene::Transform{});
        w.get<scene::Transform>(e)->local_x = static_cast<float>(i) * 2.5f - 6.0f;
        runtime::MeshComponent mc;
        mc.mesh_id = mesh_id;
        mc.material = "content://Materials/Shared";
        w.add<runtime::MeshComponent>(e, mc);
    }
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/MatCache.nfscene", scene, err));

    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/MatCache.nfscene", err));
    auto handle = manager.load_mesh_sync(mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);

    auto render_once = [&]() -> u32 {
        rhi::TextureDesc td{};
        td.width = 64;
        td.height = 64;
        td.format = rhi::Format::R8G8B8A8_UNorm;
        td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
        auto target = device.create_texture(td);
        NF_CHECK(target != nullptr);
        auto cmd = device.create_command_buffer();
        auto fence = device.create_fence(false);
        NF_CHECK(cmd && fence);
        cmd->begin();
        runtime.render_offscreen(*target, *cmd);
        cmd->end();
        device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
        NF_CHECK(fence->wait(kGpuTimeoutNs));
        device.wait_idle();
        NF_CHECK(runtime.renderer() != nullptr);
        return runtime.renderer()->last_stats().material_sets_built;
    };

    // First frame builds the shared material's set once — strictly fewer sets
    // than objects, which is the whole point.
    const u32 first = render_once();
    NF_CHECK(first >= 1);
    NF_CHECK(first < static_cast<u32>(kObjects));

    // Steady state rebuilds nothing, no matter how many objects share it.
    NF_CHECK(render_once() == 0);
    NF_CHECK(render_once() == 0);

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    NF_CHECK(rhi::validation_error_count() == 0);
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(render_memory_rows_follow_vulkan_top_left_origin) {
    // Orientation contract the whole viewport UI relies on: with a standard
    // (positive-height) Vulkan viewport, NDC +Y lands in memory-BOTTOM rows
    // (framebuffer y=0 is the top row, NDC -1 maps there). The ImGui viewport
    // Image() therefore samples with flipped V — and every NDC<->pixel
    // conversion assumes standard NDC (+Y up). A red cube above the view
    // centre must concentrate in rows 32..63, not 0..31.
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_orient");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Content" / "Materials");
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
    NF_CHECK(vfs.write_text("content://Materials/Red.nfmat",
                            "# NOVAForge Material v1\nname: Red\nbase_color: 0.9 0.1 0.1 1\n"
                            "metallic: 0\nroughness: 0.4\nao: 1\nemission: 0 0 0\n"
                            "emission_strength: 0\n")
                 .ok);

    scene::Scene scene("Orient");
    auto& w = scene.world();
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z = 5.0f;
    runtime::CameraComponent cam;
    cam.is_active = true;
    w.add<runtime::CameraComponent>(cam_e, cam);
    ecs::Entity light_e = w.create_entity();
    w.add<runtime::DirectionalLight>(light_e, runtime::DirectionalLight{});
    ecs::Entity mesh_e = w.create_entity();
    w.add<scene::Transform>(mesh_e, scene::Transform{});
    // Above the view centre (world +Y).
    w.get<scene::Transform>(mesh_e)->local_y = 2.5f;
    runtime::MeshComponent mc;
    mc.mesh_id = mesh_id;
    mc.material = "content://Materials/Red";
    w.add<runtime::MeshComponent>(mesh_e, mc);
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/Orient.nfscene", scene, err));

    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Orient.nfscene", err));
    auto handle = manager.load_mesh_sync(mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);

    rhi::TextureDesc td{};
    td.width = 64;
    td.height = 64;
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
    auto target = device.create_texture(td);
    rhi::BufferDesc bd{};
    bd.size = static_cast<usize>(64) * 64 * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = device.create_buffer(bd);
    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    NF_CHECK(target && rb && cmd && fence);
    cmd->begin();
    runtime.render_offscreen(*target, *cmd);
    cmd->copy_texture_to_buffer(*target, *rb, 0, 0, 64, 64, 0);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    const auto* px = static_cast<const uint8_t*>(rb->map());
    NF_CHECK(px != nullptr);
    uint64_t red_top = 0, red_bottom = 0;
    for (uint32_t y = 0; y < 64u; ++y) {
        for (uint32_t x = 0; x < 64u; ++x) {
            const size_t i = static_cast<size_t>(y) * 64u + x;
            const bool red = px[i * 4] > px[i * 4 + 1] + 40 && px[i * 4] > px[i * 4 + 2] + 40;
            if (red) {
                if (y < 32u) {
                    ++red_top;
                } else {
                    ++red_bottom;
                }
            }
        }
    }
    rb->unmap();
    NF_CHECK(red_bottom > 100);        // the cube rendered, above centre...
    NF_CHECK(red_top * 4 < red_bottom); // ...into memory-bottom rows (NDC +Y)

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}
