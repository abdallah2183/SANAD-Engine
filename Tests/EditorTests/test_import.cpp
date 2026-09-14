// Editor import queue: external files become content assets (or clean errors).

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/ImportQueue.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshUpload.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace nf;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

static void write_file(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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

static std::vector<uint8_t> make_cube_nfmesh(const AssetId& id, const std::string& logical) {
    auto cube = rendering::StaticMesh::create_cube(1.0f);
    auto asset = rendering::make_mesh_asset(*cube, id, logical);
    std::vector<uint8_t> bytes;
    asset->save_to_bytes(bytes);
    return bytes;
}

NF_TEST(import_mesh_file) {
    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_import_mesh");
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    AssetRegistry reg;

    const auto src = tmp / "External" / "cube.nfmesh";
    write_file(src, make_cube_nfmesh(AssetId::generate(), "content://Meshes/cube.nfmesh"));

    editor::ImportQueue q;
    std::string err;
    const size_t job = q.submit(src.string(), "content://Meshes", false, err);
    NF_CHECK(job != 0);
    NF_CHECK(q.has_pending());
    q.process_all(vfs, reg);
    NF_CHECK(!q.has_pending());
    NF_CHECK(q.jobs().size() == 1u);
    NF_CHECK(q.jobs()[0].state == editor::ImportJob::State::Done);
    NF_CHECK(q.jobs()[0].assigned_id.valid());

    // Content copy + cooked copy + registry entry, all consistent.
    NF_CHECK(vfs.exists("content://Meshes/cube.nfmesh").value);
    NF_CHECK(vfs.exists("cache://Meshes/cube.nfmesh").value);
    const AssetMetadata* meta = reg.find_by_path("content://Meshes/cube.nfmesh");
    NF_CHECK(meta != nullptr);
    NF_CHECK(meta->type == AssetType::Mesh);
    NF_CHECK(meta->id == q.jobs()[0].assigned_id);
    NF_CHECK(!meta->fingerprint.empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(import_texture_file) {
    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_import_tex");
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    AssetRegistry reg;

    const auto src = tmp / "External" / "red.bmp";
    write_file(src, make_bmp_solid(4, 4, 255, 0, 0));

    editor::ImportQueue q;
    std::string err;
    NF_CHECK(q.submit(src.string(), "content://Textures", false, err) != 0);
    q.process_all(vfs, reg);
    NF_CHECK(q.jobs()[0].state == editor::ImportJob::State::Done);
    const AssetMetadata* meta = reg.find_by_path("content://Textures/red.bmp");
    NF_CHECK(meta != nullptr);
    NF_CHECK(meta->type == AssetType::Texture);
    NF_CHECK(vfs.exists("cache://Textures/red.bmp").value);

    std::filesystem::remove_all(tmp);
}

NF_TEST(import_rejects_garbage) {
    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_import_bad");
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    AssetRegistry reg;
    editor::ImportQueue q;
    std::string err;

    // Unknown extension: rejected at submit, never queued.
    const auto txt = tmp / "External" / "note.txt";
    write_file(txt, std::vector<uint8_t>{'h', 'i'});
    NF_CHECK(q.submit(txt.string(), "content://Meshes", false, err) == 0);
    NF_CHECK(!err.empty());
    NF_CHECK(q.jobs().empty());

    // Missing source: rejected at submit.
    NF_CHECK(q.submit((tmp / "External" / "ghost.nfmesh").string(), "content://Meshes", false, err) == 0);

    // Corrupt mesh bytes: queued, then Failed with an error (nothing written).
    const auto bad = tmp / "External" / "bad.nfmesh";
    write_file(bad, std::vector<uint8_t>{1, 2, 3, 4, 5});
    NF_CHECK(q.submit(bad.string(), "content://Meshes", false, err) != 0);
    q.process_all(vfs, reg);
    NF_CHECK(q.jobs().back().state == editor::ImportJob::State::Failed);
    NF_CHECK(!q.jobs().back().error.empty());
    NF_CHECK(reg.size() == 0u);

    // Duplicate destination without overwrite: Failed.
    const auto src = tmp / "External" / "cube.nfmesh";
    write_file(src, make_cube_nfmesh(AssetId::generate(), "content://Meshes/cube.nfmesh"));
    NF_CHECK(q.submit(src.string(), "content://Meshes", false, err) != 0);
    q.process_all(vfs, reg);
    NF_CHECK(q.jobs().back().state == editor::ImportJob::State::Done);
    NF_CHECK(q.submit(src.string(), "content://Meshes", false, err) != 0);
    q.process_all(vfs, reg);
    NF_CHECK(q.jobs().back().state == editor::ImportJob::State::Failed);

    // ...but overwrite=true replaces cleanly.
    NF_CHECK(q.submit(src.string(), "content://Meshes", true, err) != 0);
    q.process_all(vfs, reg);
    NF_CHECK(q.jobs().back().state == editor::ImportJob::State::Done);
    NF_CHECK(reg.size() == 1u);

    q.clear_finished();
    NF_CHECK(q.jobs().empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(import_material_scene_noregistry) {
    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_import_misc");
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    AssetRegistry reg;
    editor::ImportQueue q;
    std::string err;

    const auto mat = tmp / "External" / "Blue.nfmat";
    const std::string mat_text = "# NOVAForge Material v1\nname: Blue\nbase_color: 0.1 0.2 0.9 1\n";
    write_file(mat, std::vector<uint8_t>(mat_text.begin(), mat_text.end()));
    NF_CHECK(q.submit(mat.string(), "content://Materials", false, err) != 0);

    const auto scn = tmp / "External" / "Mini.nfscene";
    const std::string scn_text = "# NOVAForge Scene v1\nversion: 1\nname: Mini\nentity_count: 0\n";
    write_file(scn, std::vector<uint8_t>(scn_text.begin(), scn_text.end()));
    NF_CHECK(q.submit(scn.string(), "content://Scenes", false, err) != 0);

    q.process_all(vfs, reg);
    NF_CHECK(q.jobs()[0].state == editor::ImportJob::State::Done);
    NF_CHECK(q.jobs()[1].state == editor::ImportJob::State::Done);
    NF_CHECK(vfs.exists("content://Materials/Blue.nfmat").value);
    NF_CHECK(vfs.exists("content://Scenes/Mini.nfscene").value);
    // Text assets bypass the registry (like scenes always have).
    NF_CHECK(reg.size() == 0u);

    std::filesystem::remove_all(tmp);
}

// Pump until nothing is pending (bounded, generous for loaded CI).
static bool pump_until_settled(editor::ImportQueue& q, assets::VirtualFileSystem& vfs,
                               assets::AssetRegistry& reg, int timeout_ms = 15000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (q.has_pending()) {
        q.pump_async(vfs, reg);
        if (!q.has_pending()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
    }
    return true;
}

NF_TEST(import_async_completes_on_workers) {
    // Real worker threads when possible; the queue falls back to inline
    // execution otherwise, so assertions hold on both paths.
    const bool owned = !nf::JobSystem::instance().is_initialized();
    if (owned) {
        nf::JobSystem::instance().init(2);
    }

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_import_async");
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    AssetRegistry reg;
    editor::ImportQueue q;
    std::string err;

    const auto mesh_src = tmp / "External" / "cube.nfmesh";
    write_file(mesh_src, make_cube_nfmesh(AssetId::generate(), "content://Meshes/cube.nfmesh"));
    const auto tex_src = tmp / "External" / "red.bmp";
    write_file(tex_src, make_bmp_solid(4, 4, 255, 0, 0));
    const auto mat_src = tmp / "External" / "Blue.nfmat";
    const std::string mat_text = "# NOVAForge Material v1\nname: Blue\nbase_color: 0.1 0.2 0.9 1\n";
    write_file(mat_src, std::vector<uint8_t>(mat_text.begin(), mat_text.end()));

    const size_t j_mesh = q.submit(mesh_src.string(), "content://Meshes", false, err);
    const size_t j_tex = q.submit(tex_src.string(), "content://Textures", false, err);
    const size_t j_mat = q.submit(mat_src.string(), "content://Materials", false, err);
    NF_CHECK(j_mesh != 0 && j_tex != 0 && j_mat != 0);

    NF_CHECK(pump_until_settled(q, vfs, reg));
    NF_CHECK(q.jobs().size() == 3u);
    // Submit order preserved in the queue; every job terminal with progress 1.
    NF_CHECK(q.jobs()[0].id == j_mesh && q.jobs()[1].id == j_tex && q.jobs()[2].id == j_mat);
    for (const auto& j : q.jobs()) {
        NF_CHECK(j.state == editor::ImportJob::State::Done);
        NF_CHECK(j.progress > 0.99f);
    }
    // Same end state as the synchronous path would produce.
    NF_CHECK(reg.find_by_path("content://Meshes/cube.nfmesh") != nullptr);
    NF_CHECK(reg.find_by_path("content://Textures/red.bmp") != nullptr);
    NF_CHECK(reg.size() == 2u);
    NF_CHECK(vfs.exists("content://Materials/Blue.nfmat").value);
    NF_CHECK(q.jobs()[0].assigned_id.valid() && q.jobs()[1].assigned_id.valid());
    NF_CHECK(q.jobs()[0].assigned_id != q.jobs()[1].assigned_id);

    if (owned) {
        nf::JobSystem::instance().shutdown();
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(import_async_failure_does_not_block) {
    const bool owned = !nf::JobSystem::instance().is_initialized();
    if (owned) {
        nf::JobSystem::instance().init(2);
    }

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_import_async_fail");
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    AssetRegistry reg;
    editor::ImportQueue q;
    std::string err;

    const auto good1 = tmp / "External" / "a.nfmesh";
    write_file(good1, make_cube_nfmesh(AssetId::generate(), "content://Meshes/a.nfmesh"));
    const auto bad = tmp / "External" / "bad.nfmesh";
    write_file(bad, std::vector<uint8_t>{1, 2, 3, 4, 5});
    const auto good2 = tmp / "External" / "b.nfmesh";
    write_file(good2, make_cube_nfmesh(AssetId::generate(), "content://Meshes/b.nfmesh"));

    NF_CHECK(q.submit(good1.string(), "content://Meshes", false, err) != 0);
    NF_CHECK(q.submit(bad.string(), "content://Meshes", false, err) != 0);
    NF_CHECK(q.submit(good2.string(), "content://Meshes", false, err) != 0);

    NF_CHECK(pump_until_settled(q, vfs, reg));
    NF_CHECK(q.jobs().size() == 3u);
    NF_CHECK(q.jobs()[0].state == editor::ImportJob::State::Done);
    NF_CHECK(q.jobs()[1].state == editor::ImportJob::State::Failed);
    NF_CHECK(!q.jobs()[1].error.empty());
    NF_CHECK(q.jobs()[2].state == editor::ImportJob::State::Done);
    NF_CHECK(reg.size() == 2u);

    if (owned) {
        nf::JobSystem::instance().shutdown();
    }
    std::filesystem::remove_all(tmp);
}
