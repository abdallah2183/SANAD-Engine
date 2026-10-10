// Editor import queue: external files become content assets (or clean errors).

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/MeshExport.hpp>
#include <NF/Assets/MeshImport.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/ImportQueue.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Rendering/MaterialAsset.hpp>

#include <chrono>
#include <iterator>
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

/// A cube exported to `<dir>/<stem>.<ext>`, so the import tests exercise the
/// engine's real writer on one side and its real reader on the other.
static bool write_cube_model(const std::filesystem::path& dir, const std::string& stem,
                             assets::MeshFormat format) {
    auto cube = rendering::StaticMesh::create_cube(1.0f);
    auto asset = rendering::make_mesh_asset(*cube, AssetId::generate(),
                                            "content://Meshes/" + stem + ".nfmesh");
    const std::filesystem::path path =
        dir / (stem + "." + std::string(assets::mesh_format_extension(format)));
    std::string err;
    return assets::export_mesh_to_file(*asset, path.string(), err);
}

/// A VFS + registry pair rooted in a fresh scratch directory, which is what
/// every import test needs before it can submit anything.
struct ImportFixture {
    assets::VirtualFileSystem vfs;
    assets::AssetRegistry reg;
    editor::ImportQueue queue;
    std::filesystem::path tmp;

    explicit ImportFixture(const std::string& name) {
        tmp = temp_dir_for(name);
        std::filesystem::create_directories(tmp / "Content");
        std::filesystem::create_directories(tmp / "Cache");
        std::filesystem::create_directories(tmp / "External");
        vfs.mount("content://", tmp / "Content");
        vfs.mount("cache://", tmp / "Cache");
    }
    ~ImportFixture() { std::filesystem::remove_all(tmp); }
    ImportFixture(const ImportFixture&) = delete;
    ImportFixture& operator=(const ImportFixture&) = delete;

    std::filesystem::path external(const std::string& name) const { return tmp / "External" / name; }
};

NF_TEST(import_mesh_file_case) {
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
    const std::string mat_text = "# SANAD Material v1\nname: Blue\nbase_color: 0.1 0.2 0.9 1\n";
    write_file(mat, std::vector<uint8_t>(mat_text.begin(), mat_text.end()));
    NF_CHECK(q.submit(mat.string(), "content://Materials", false, err) != 0);

    const auto scn = tmp / "External" / "Mini.nfscene";
    const std::string scn_text = "# SANAD Scene v1\nversion: 1\nname: Mini\nentity_count: 0\n";
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
    const std::string mat_text = "# SANAD Material v1\nname: Blue\nbase_color: 0.1 0.2 0.9 1\n";
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

// ---------------------------------------------------------------------------
// Model sources: every format the dialog offers actually imports
//
// These are the regression pins for the bug this work exists to fix. The Import
// dialog's file filter offered "*.gltf;*.glb" while ImportQueue::submit()
// answered "Unsupported extension" for both — the two lists were maintained by
// hand and drifted apart. `import_accepts_every_format_the_dialog_offers` walks
// the reader's own format table, so the day a format is added to one list and
// not the other, this file goes red instead of the dialog going quiet.
// ---------------------------------------------------------------------------

NF_TEST(import_accepts_every_format_the_dialog_offers) {
    ImportFixture fx("nf_ed_import_all_formats");

    // The dialog's glob and the queue's accepted set are the same set.
    const std::string glob = editor::ImportQueue::accepted_extension_glob();
    for (const assets::MeshImportFormat format : assets::mesh_import_formats()) {
        const std::string ext = assets::mesh_import_format_extension(format);
        NF_CHECK(glob.find("*." + ext) != std::string::npos);
    }

    for (const assets::MeshFormat format : assets::mesh_formats()) {
        const std::string stem = std::string("Cube_") + assets::mesh_format_extension(format);
        if (!write_cube_model(fx.external(""), stem, format)) {
            NF_SKIP("cannot write the model fixture");
        }
    }

    for (const assets::MeshFormat format : assets::mesh_formats()) {
        const std::string ext = assets::mesh_format_extension(format);
        const std::string stem = std::string("Cube_") + ext;
        const auto src = fx.external(stem + "." + ext);
        std::string err;
        const size_t job = fx.queue.submit(src.string(), "content://Meshes", false, err);
        // A zero here IS the bug: the dialog offered a file the queue refuses.
        NF_CHECK(job != 0);
        if (job == 0) {
            continue;
        }
        NF_CHECK(err.empty());
    }

    fx.queue.process_all(fx.vfs, fx.reg);
    NF_CHECK(fx.queue.jobs().size() == assets::mesh_formats().size());
    for (const editor::ImportJob& job : fx.queue.jobs()) {
        NF_CHECK(job.state == editor::ImportJob::State::Done);
        NF_CHECK(job.type == assets::AssetType::Mesh);
        NF_CHECK(job.assigned_id.valid());
        // Every model lands as a cooked .nfmesh, never under its source name.
        NF_CHECK(job.dst_logical.size() > 7);
        NF_CHECK(job.dst_logical.substr(job.dst_logical.size() - 7) == ".nfmesh");
        NF_CHECK(fx.vfs.exists(job.dst_logical).value);
        NF_CHECK(fx.reg.find_by_path(job.dst_logical) != nullptr);
    }
}

NF_TEST(import_glb_source_cooks_to_a_registered_nfmesh) {
    ImportFixture fx("nf_ed_import_glb_source");
    // The exact case a developer hits first: pick a .glb in the Import dialog.
    if (!write_cube_model(fx.external(""), "Hero", assets::MeshFormat::Glb)) {
        NF_SKIP("cannot write the model fixture");
    }
    const auto src = fx.external("Hero.glb");

    std::string err;
    const size_t job = fx.queue.submit(src.string(), "content://Meshes", false, err);
    NF_CHECK(job != 0);
    NF_CHECK(fx.queue.jobs()[0].source_format == assets::MeshImportFormat::Glb);

    fx.queue.process_all(fx.vfs, fx.reg);
    const editor::ImportJob& done = fx.queue.jobs()[0];
    NF_CHECK(done.state == editor::ImportJob::State::Done);
    NF_CHECK(done.dst_logical == "content://Meshes/Hero.nfmesh");
    // The mesh is written first; the GLB also declares a material, which lands
    // beside it as an .nfmat — .nfmesh cannot carry one.
    NF_CHECK(!done.written.empty());
    NF_CHECK(done.written[0].logical_path == "content://Meshes/Hero.nfmesh");
    NF_CHECK(done.written[0].type == assets::AssetType::Mesh);
    NF_CHECK(fx.vfs.exists("content://Meshes/Hero.nfmesh").value);
    NF_CHECK(fx.vfs.exists("cache://Meshes/Hero.nfmesh").value);

    const assets::AssetMetadata* meta = fx.reg.find_by_path("content://Meshes/Hero.nfmesh");
    NF_CHECK(meta != nullptr);
    NF_CHECK(meta->type == assets::AssetType::Mesh);
    NF_CHECK(meta->id == done.assigned_id);
    NF_CHECK(!meta->fingerprint.empty());

    // And the cooked file really is a mesh the engine can read back.
    std::string load_err;
    const auto cooked_path = fx.vfs.resolve("content://Meshes/Hero.nfmesh");
    NF_CHECK(cooked_path.ok);
    auto cooked = assets::MeshAsset::load_from_file(cooked_path.value.string(), load_err);
    NF_CHECK(cooked != nullptr);
    if (cooked) {
        NF_CHECK(cooked->vertices.size() == 24u);
        NF_CHECK(cooked->indices.size() == 36u);
    }
}

NF_TEST(import_multi_mesh_source_writes_one_nfmesh_per_mesh) {
    ImportFixture fx("nf_ed_import_multi_mesh");
    // Two `g` groups: the reader produces two meshes, and the queue has to keep
    // both. One asset plus a silently dropped second mesh would be exactly the
    // kind of loss this pipeline is supposed to make impossible.
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "g Hull\n"
        "f 1 2 3\n"
        "g Turret\n"
        "f 3 2 1\n";
    const auto src = fx.external("Tank.obj");
    write_file(src, std::vector<uint8_t>(obj.begin(), obj.end()));

    std::string err;
    NF_CHECK(fx.queue.submit(src.string(), "content://Meshes", false, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);

    const editor::ImportJob& done = fx.queue.jobs()[0];
    NF_CHECK(done.state == editor::ImportJob::State::Done);
    NF_CHECK(done.written.size() == 2u);
    NF_CHECK(done.written[0].logical_path == "content://Meshes/Tank.nfmesh");
    NF_CHECK(done.written[1].logical_path == "content://Meshes/Tank_1.nfmesh");
    NF_CHECK(fx.vfs.exists("content://Meshes/Tank.nfmesh").value);
    NF_CHECK(fx.vfs.exists("content://Meshes/Tank_1.nfmesh").value);
    NF_CHECK(fx.vfs.exists("cache://Meshes/Tank_1.nfmesh").value);
    NF_CHECK(fx.reg.size() == 2u);
    NF_CHECK(fx.reg.find_by_path("content://Meshes/Tank.nfmesh") != nullptr);
    NF_CHECK(fx.reg.find_by_path("content://Meshes/Tank_1.nfmesh") != nullptr);
}

NF_TEST(import_surfaces_what_the_reader_could_not_carry) {
    ImportFixture fx("nf_ed_import_warnings");
    // STL has no UVs and no vertex normals. The reader says so, and the job has
    // to hand that on — a quiet import would let a developer ship a mesh with
    // zeroed UVs and never know why the texture is wrong.
    const std::string stl =
        "solid tri\n"
        "  facet normal 0 0 1\n"
        "    outer loop\n"
        "      vertex 0 0 0\n"
        "      vertex 1 0 0\n"
        "      vertex 0 1 0\n"
        "    endloop\n"
        "  endfacet\n"
        "endsolid tri\n";
    const auto src = fx.external("Tri.stl");
    write_file(src, std::vector<uint8_t>(stl.begin(), stl.end()));

    std::string err;
    NF_CHECK(fx.queue.submit(src.string(), "content://Meshes", false, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);

    const editor::ImportJob& done = fx.queue.jobs()[0];
    NF_CHECK(done.state == editor::ImportJob::State::Done);
    NF_CHECK(done.warnings.size() == 1u);
    NF_CHECK(done.warnings[0].find("no UVs") != std::string::npos);
}

NF_TEST(import_model_source_rejects_a_broken_file_without_writing) {
    ImportFixture fx("nf_ed_import_broken_model");
    const auto src = fx.external("Broken.glb");
    write_file(src, std::vector<uint8_t>{'n', 'o', 't', ' ', 'a', ' ', 'm', 'o', 'd', 'e', 'l'});

    std::string err;
    NF_CHECK(fx.queue.submit(src.string(), "content://Meshes", false, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);

    const editor::ImportJob& done = fx.queue.jobs()[0];
    NF_CHECK(done.state == editor::ImportJob::State::Failed);
    NF_CHECK(!done.error.empty());
    NF_CHECK(done.written.empty());
    NF_CHECK(fx.reg.size() == 0u);
    NF_CHECK(!fx.vfs.exists("content://Meshes/Broken.nfmesh").value);
}

// ---------------------------------------------------------------------------
// Materials and textures: importing a model has to bring what makes it look
// like itself. Geometry alone arrives grey, which is not a usable import.
// ---------------------------------------------------------------------------

NF_TEST(import_model_writes_its_textures_and_materials) {
    ImportFixture fx("nf_ed_import_model_materials");

    // An OBJ + .mtl + a base-colour image, all beside each other: the shape a
    // real artist's export has.
    const std::string obj =
        "mtllib Panel.mtl\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "vn 0 0 1\n"
        "usemtl wood\n"
        "f 1/1/1 2/2/1 3/3/1\n";
    write_file(fx.external("Panel.obj"), std::vector<uint8_t>(obj.begin(), obj.end()));
    const std::string mtl =
        "newmtl wood\n"
        "Kd 0.55 0.35 0.15\n"
        "d 0.8\n"
        "Ks 1 1 1\n"
        "Ns 32\n"
        "map_Kd wood.png\n";
    write_file(fx.external("Panel.mtl"), std::vector<uint8_t>(mtl.begin(), mtl.end()));
    const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13,
                                      'I', 'H', 'D', 'R'};
    write_file(fx.external("wood.png"), png);

    std::string err;
    NF_CHECK(fx.queue.submit(fx.external("Panel.obj").string(), "content://Meshes", false, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);

    const editor::ImportJob& done = fx.queue.jobs()[0];
    NF_CHECK(done.state == editor::ImportJob::State::Done);

    // Three files, in a stable order: mesh, texture, material.
    NF_CHECK(done.written.size() == 3u);
    NF_CHECK(done.written[0].logical_path == "content://Meshes/Panel.nfmesh");
    NF_CHECK(done.written[0].type == assets::AssetType::Mesh);
    NF_CHECK(done.written[1].logical_path == "content://Textures/Panel_wood.png");
    NF_CHECK(done.written[1].type == assets::AssetType::Texture);
    NF_CHECK(done.written[2].logical_path == "content://Materials/Panel_wood.nfmat");
    NF_CHECK(done.written[2].type == assets::AssetType::Material);

    // The texture is a real, registered asset with a cache copy.
    NF_CHECK(fx.vfs.exists("content://Textures/Panel_wood.png").value);
    NF_CHECK(fx.vfs.exists("cache://Textures/Panel_wood.png").value);
    const assets::AssetMetadata* texture =
        fx.reg.find_by_path("content://Textures/Panel_wood.png");
    NF_CHECK(texture != nullptr);
    if (texture != nullptr) {
        NF_CHECK(texture->type == assets::AssetType::Texture);
        NF_CHECK(texture->format == "img-v1");
    }

    // The .nfmat parses, carries the imported factors, and its albedo resolves
    // to the texture that was just written — that link is the whole point.
    const auto mat_path = fx.vfs.resolve("content://Materials/Panel_wood.nfmat");
    NF_CHECK(mat_path.ok);
    std::ifstream in(mat_path.value, std::ios::binary);
    NF_CHECK(static_cast<bool>(in));
    const std::string mat_text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    rendering::MaterialAsset material;
    std::string mat_err;
    NF_CHECK(rendering::MaterialAsset::load_from_text(mat_text, material, mat_err));
    NF_CHECK(material.name == "wood");
    NF_CHECK_NEAR(material.params.base_color[0], 0.55f, 1e-5f);
    NF_CHECK_NEAR(material.params.base_color[1], 0.35f, 1e-5f);
    NF_CHECK_NEAR(material.params.base_color[2], 0.15f, 1e-5f);
    NF_CHECK_NEAR(material.params.base_color[3], 0.8f, 1e-5f);
    NF_CHECK(material.albedo == "content://Textures/Panel_wood.png");
    // The texture it points at is loadable through the VFS, not a dangling name.
    NF_CHECK(fx.vfs.exists(material.albedo).value);

    // Ks and Ns have nowhere to go in the engine's material block, and the job
    // says so instead of pretending the material is complete.
    NF_CHECK(!done.warnings.empty());
    std::string joined;
    for (const std::string& warning : done.warnings) joined += warning + "|";
    NF_CHECK(joined.find("Ks") != std::string::npos);
    NF_CHECK(joined.find("Ns") != std::string::npos);
}

NF_TEST(import_model_without_textures_writes_only_geometry) {
    ImportFixture fx("nf_ed_import_model_plain");
    // No mtllib, no usemtl: nothing to carry beyond the mesh, and nothing
    // invented to fill the gap.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    write_file(fx.external("Plain.obj"), std::vector<uint8_t>(obj.begin(), obj.end()));

    std::string err;
    NF_CHECK(fx.queue.submit(fx.external("Plain.obj").string(), "content://Meshes", false, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);

    const editor::ImportJob& done = fx.queue.jobs()[0];
    // Surface the importer's own message on failure: "state != Done" alone
    // sends the next reader hunting through the whole pipeline.
    if (done.state != editor::ImportJob::State::Done) {
        throw std::runtime_error("import failed: " + done.error);
    }
    NF_CHECK(done.written.size() == 1u);
    NF_CHECK(done.written[0].logical_path == "content://Meshes/Plain.nfmesh");
    NF_CHECK(fx.reg.size() == 1u);
    NF_CHECK(fx.queue.jobs()[0].warnings.empty());
}

NF_TEST(import_model_overwrite_replaces_textures_and_materials_too) {
    ImportFixture fx("nf_ed_import_model_overwrite");
    const std::string obj =
        "mtllib Panel.mtl\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vn 0 0 1\n"
        "usemtl wood\n"
        "f 1//1 2//1 3//1\n";
    write_file(fx.external("Panel.obj"), std::vector<uint8_t>(obj.begin(), obj.end()));
    const std::string mtl = "newmtl wood\nKd 0.1 0.2 0.3\nd 1\n";
    write_file(fx.external("Panel.mtl"), std::vector<uint8_t>(mtl.begin(), mtl.end()));

    std::string err;
    NF_CHECK(fx.queue.submit(fx.external("Panel.obj").string(), "content://Meshes", false, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);
    if (fx.queue.jobs()[0].state != editor::ImportJob::State::Done) {
        throw std::runtime_error("import failed: " + fx.queue.jobs()[0].error);
    }
    NF_CHECK(fx.reg.size() == 1u); // the mesh; the .nfmat bypasses the registry

    // Re-importing without overwrite is refused, and refused for the FILE that
    // already exists rather than half-applied across the set.
    NF_CHECK(fx.queue.submit(fx.external("Panel.obj").string(), "content://Meshes", false, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);
    NF_CHECK(fx.queue.jobs()[1].state == editor::ImportJob::State::Failed);
    NF_CHECK(fx.queue.jobs()[1].error.find("already exists") != std::string::npos);

    // With overwrite it replaces cleanly, leaving exactly one registry entry.
    NF_CHECK(fx.queue.submit(fx.external("Panel.obj").string(), "content://Meshes", true, err) != 0);
    fx.queue.process_all(fx.vfs, fx.reg);
    NF_CHECK(fx.queue.jobs()[2].state == editor::ImportJob::State::Done);
    NF_CHECK(fx.reg.size() == 1u);
    NF_CHECK(fx.reg.find_by_path("content://Meshes/Panel.nfmesh") != nullptr);
}
