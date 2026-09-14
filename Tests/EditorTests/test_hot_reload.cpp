// Editor hot reload: watcher detection, material/texture/mesh live refresh,
// dirty protection, and deletion safety — all pixel-proven where it matters.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/FileWatcher.hpp>
#include <NF/Editor/HotReload.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>

#include <filesystem>
#include <fstream>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

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

static void write_bytes(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static void write_text(const std::filesystem::path& p, const std::string& text) {
    write_bytes(p, std::vector<uint8_t>(text.begin(), text.end()));
}

NF_TEST(hotwatch_detects_changes) {
    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_watch");
    std::filesystem::create_directories(tmp / "Content");
    vfs.mount("content://", tmp / "Content");

    write_text(tmp / "Content" / "a.txt", "v1-padded!!");
    editor::FileWatcher w;
    NF_CHECK(!w.is_watched("content://a.txt"));
    w.watch(vfs, "content://a.txt");
    NF_CHECK(w.is_watched("content://a.txt"));
    NF_CHECK(w.poll(vfs).empty()); // baseline adoption is silent

    write_text(tmp / "Content" / "a.txt", "v2-padded!!");
    auto changed = w.poll(vfs);
    NF_CHECK(changed.size() == 1u && changed[0] == "content://a.txt");
    NF_CHECK(w.poll(vfs).empty()); // re-baselined: quiet again

    // Deletion reports exactly once, then goes quiet.
    std::filesystem::remove(tmp / "Content" / "a.txt");
    NF_CHECK(w.poll(vfs).size() == 1u);
    NF_CHECK(w.poll(vfs).empty());

    // Our own writes never echo when refreshed.
    write_text(tmp / "Content" / "a.txt", "v3-padded!!");
    w.refresh("content://a.txt", vfs);
    NF_CHECK(w.poll(vfs).empty());

    w.unwatch("content://a.txt");
    NF_CHECK(!w.is_watched("content://a.txt"));
    w.clear();
    NF_CHECK(w.watched_count() == 0u);

    std::filesystem::remove_all(tmp);
}

// Shared GPU fixture: red-textured cube scene with watched material+texture.
struct HotScene {
    VirtualFileSystem vfs;
    std::filesystem::path tmp;
    AssetRegistry reg;
    AssetId mesh_id;
    std::string err;

    bool setup(const std::string& name, float cube_size) {
        tmp = temp_dir_for(name);
        std::filesystem::create_directories(tmp / "Content" / "Scenes");
        std::filesystem::create_directories(tmp / "Content" / "Materials");
        std::filesystem::create_directories(tmp / "Content" / "Textures");
        std::filesystem::create_directories(tmp / "Cache" / "Meshes");
        vfs.mount("content://", tmp / "Content");
        vfs.mount("cache://", tmp / "Cache");

        auto cube = rendering::StaticMesh::create_cube(cube_size);
        mesh_id = AssetId::generate();
        auto asset = MeshAsset::from_static_mesh(*cube, mesh_id, "content://Meshes/cube.nfmesh");
        std::vector<uint8_t> bytes;
        asset->save_to_bytes(bytes);
        // Source (watched) and cooked (loaded) start identical.
        write_bytes(tmp / "Content" / "Meshes" / "cube.nfmesh", bytes);
        if (!vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(bytes)).ok) {
            return false;
        }
        AssetMetadata meta;
        meta.id = mesh_id;
        meta.type = AssetType::Mesh;
        meta.logical_path = "content://Meshes/cube.nfmesh";
        meta.cooked_path = "cache://Meshes/cube.nfmesh";
        if (!reg.add(meta, err)) {
            return false;
        }
        write_bytes(tmp / "Content" / "Textures" / "c.bmp", make_bmp_solid(4, 4, 255, 0, 0));
        const std::string mat_text =
            "# NOVAForge Material v1\nname: Default\nbase_color: 0.8 0.8 0.8 1\nmetallic: 0\n"
            "roughness: 0.4\nao: 1\nemission: 0 0 0\nemission_strength: 0\n"
            "albedo: content://Textures/c.bmp\n";
        write_text(tmp / "Content" / "Materials" / "Default.nfmat", mat_text);

        scene::Scene scene("Hot");
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
        return runtime::save_scene_to_vfs(vfs, "content://Scenes/Hot.nfscene", scene, err);
    }
};

static uint64_t render_red_count(rhi::IGraphicsDevice& device, runtime::Runtime& runtime) {
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
}

static uint32_t render_lit_count(rhi::IGraphicsDevice& device, runtime::Runtime& runtime) {
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
    uint32_t lit = 0;
    for (size_t i = 0; i < 64u * 64u; ++i) {
        if (px[i * 4] > 10 || px[i * 4 + 1] > 10 || px[i * 4 + 2] > 10) {
            ++lit;
        }
    }
    rb->unmap();
    device.wait_idle();
    return lit;
}

NF_TEST(hotreload_texture_goes_live) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    HotScene hs;
    NF_CHECK(hs.setup("nf_ed_hottex", 2.0f));
    AssetManager manager(hs.vfs, hs.reg, &device);
    runtime::Runtime runtime(hs.vfs, hs.reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Hot.nfscene", hs.err));
    auto handle = manager.load_mesh_sync(hs.mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);
    NF_CHECK(render_red_count(device, runtime) > 100); // red albedo live

    // Watch BEFORE the external edit (first poll adopts baselines silently).
    editor::HotReload hot;
    hot.watch_texture("content://Textures/c.bmp");
    NF_CHECK(hot.poll(hs.vfs, hs.reg, runtime).empty());

    // External edit: blue texture on disk + poll => viewport goes blue.
    write_bytes(hs.tmp / "Content" / "Textures" / "c.bmp", make_bmp_solid(4, 4, 0, 0, 255));
    auto results = hot.poll(hs.vfs, hs.reg, runtime);
    NF_CHECK(results.size() == 1u && results[0].ok);
    NF_CHECK(render_red_count(device, runtime) < 50); // red essentially gone
    NF_CHECK(hot.poll(hs.vfs, hs.reg, runtime).empty()); // quiet after

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(hs.tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(hotreload_material_goes_live_and_dirty_wins) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    HotScene hs;
    NF_CHECK(hs.setup("nf_ed_hotmat", 2.0f));
    AssetManager manager(hs.vfs, hs.reg, &device);
    runtime::Runtime runtime(hs.vfs, hs.reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Hot.nfscene", hs.err));
    auto handle = manager.load_mesh_sync(hs.mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);

    editor::ConsoleBuffer console;
    editor::EditorApp app(hs.vfs, hs.reg, manager, console);
    app.attach_runtime(&runtime);
    NF_CHECK(app.open_scene("content://Scenes/Hot.nfscene", hs.err));

    editor::HotReload hot;
    hot.watch_material("content://Materials/Default.nfmat");
    NF_CHECK(hot.poll(hs.vfs, hs.reg, runtime).empty());

    // External edit turns the file blue-ish (base color, no albedo change).
    write_text(hs.tmp / "Content" / "Materials" / "Default.nfmat",
               "# NOVAForge Material v1\nname: Default\nbase_color: 0.1 0.2 0.9 1\nmetallic: 0\n"
               "roughness: 0.4\nao: 1\nemission: 0 0 0\nemission_strength: 0\n"
               "albedo: content://Textures/c.bmp\n");
    auto results = hot.poll(hs.vfs, hs.reg, runtime);
    NF_CHECK(results.size() == 1u && results[0].ok);
    // Albedo texture (red) still multiplies: red dominance survives a base
    // change, so assert structurally instead — params match the file.
    rendering::PBRMaterialParams p{};
    NF_CHECK(runtime.material_params("content://Materials/Default", p));
    NF_CHECK_NEAR(p.base_color[0], 0.1f, 1e-5f);
    NF_CHECK_NEAR(p.base_color[2], 0.9f, 1e-5f);

    // Unsaved editor edits win over the disk: edit, rewrite file, poll skips.
    editor::MaterialEdit edit;
    edit.base_color[0] = 0.0f;
    edit.base_color[1] = 1.0f;
    edit.base_color[2] = 0.0f;
    edit.base_color[3] = 1.0f;
    NF_CHECK(app.set_material_params("content://Materials/Default", edit, hs.err));
    write_text(hs.tmp / "Content" / "Materials" / "Default.nfmat",
               "# NOVAForge Material v1\nname: Default\nbase_color: 1 1 0 1\nmetallic: 0\n"
               "roughness: 0.4\nao: 1\nemission: 0 0 0\nemission_strength: 0\n");
    auto results2 = hot.poll(hs.vfs, hs.reg, runtime);
    NF_CHECK(results2.size() == 1u && !results2[0].ok); // skipped, guarded
    rendering::PBRMaterialParams kept{};
    NF_CHECK(runtime.material_params("content://Materials/Default", kept));
    NF_CHECK_NEAR(kept.base_color[1], 1.0f, 1e-5f); // editor green, not file yellow

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(hs.tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(hotreload_mesh_rebuilds_live) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    HotScene hs;
    NF_CHECK(hs.setup("nf_ed_hotmesh", 2.0f));
    AssetManager manager(hs.vfs, hs.reg, &device);
    runtime::Runtime runtime(hs.vfs, hs.reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Hot.nfscene", hs.err));
    auto handle = manager.load_mesh_sync(hs.mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);
    const uint32_t lit_before = render_lit_count(device, runtime);
    NF_CHECK(lit_before > 100);

    editor::HotReload hot;
    hot.watch_mesh(hs.mesh_id, "content://Meshes/cube.nfmesh");
    NF_CHECK(hot.poll(hs.vfs, hs.reg, runtime).empty());

    // External edit: shrink the source cube; poll rebuilds the live copy.
    auto small = rendering::StaticMesh::create_cube(0.5f);
    auto small_asset = MeshAsset::from_static_mesh(*small, hs.mesh_id, "content://Meshes/cube.nfmesh");
    std::vector<uint8_t> small_bytes;
    small_asset->save_to_bytes(small_bytes);
    write_bytes(hs.tmp / "Content" / "Meshes" / "cube.nfmesh", small_bytes);

    auto results = hot.poll(hs.vfs, hs.reg, runtime);
    NF_CHECK(results.size() == 1u && results[0].ok);
    runtime.update(0.016f);
    const uint32_t lit_after = render_lit_count(device, runtime);
    NF_CHECK(lit_after > 0 && lit_after * 4 < lit_before); // visibly smaller cube

    // Corrupt source: validation rejects, last good copy stays live.
    write_bytes(hs.tmp / "Content" / "Meshes" / "cube.nfmesh", std::vector<uint8_t>{9, 9, 9});
    auto results2 = hot.poll(hs.vfs, hs.reg, runtime);
    NF_CHECK(results2.size() == 1u && !results2[0].ok);
    runtime.update(0.016f);
    NF_CHECK(render_lit_count(device, runtime) > 0);

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(hs.tmp);
    rhi::reset_validation_error_count();
}
