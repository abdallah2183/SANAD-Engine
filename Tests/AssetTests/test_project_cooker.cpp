// Tests/AssetTests/test_project_cooker.cpp — the cook pipeline
//
// The cooker is the first step of packaging, so these tests pin the properties
// packaging depends on: stable identity across re-cooks, idempotence (a second
// cook does no work), content-addressed skipping, and a failure message that
// names the asset rather than only counting it.

#include <NF/Test/TestFramework.hpp>
#include <NF/Project/ProjectCooker.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <filesystem>
#include <fstream>

using namespace nf;
using namespace nf::assets;
using namespace nf::project;

namespace {

struct CookSandbox {
    std::filesystem::path root;
    VirtualFileSystem vfs;
    AssetRegistry registry;

    explicit CookSandbox(const std::string& name) {
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "Content");
        std::filesystem::create_directories(root / "Cache");
        vfs.mount("content://", root / "Content");
        vfs.mount("cache://", root / "Cache");
    }
    ~CookSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void write(const std::string& rel, const std::string& text) const {
        auto p = root / "Content" / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << text;
    }

    // A real Assets-format .nfmesh. Note the cooker validates with
    // assets::MeshAsset, whose magic is 'NFME' — the rendering-side
    // save_mesh_asset writes 'NFM1' and would be rejected. Using the right
    // writer here is the difference between testing the cooker and testing a
    // format mismatch.
    bool write_valid_mesh(const std::string& rel) const {
        auto cube = rendering::StaticMesh::create_cube(1.0f);
        if (!cube) return false;
        auto asset = assets::MeshAsset::from_static_mesh(*cube, AssetId::generate(),
                                                         "content://" + rel);
        if (!asset) return false;
        std::string err;
        auto p = root / "Content" / rel;
        std::filesystem::create_directories(p.parent_path());
        return asset->save_to_file(p.string(), err);
    }

    bool cached(const std::string& rel) const {
        return std::filesystem::exists(root / "Cache" / rel);
    }
};

} // namespace

NF_TEST(cooker_fingerprint_is_content_addressed) {
    const char* a = "hello";
    const char* b = "hellp"; // one byte differs
    const auto fa = fingerprint_bytes(a, 5);
    const auto fb = fingerprint_bytes(b, 5);
    NF_CHECK_EQ(fa.size(), size_t{16});
    NF_CHECK(fa != fb);
    // Stable across calls, or the skip cache would never hit.
    NF_CHECK_EQ(fingerprint_bytes(a, 5), fa);
    // Length matters: a prefix must not hash the same as the whole.
    NF_CHECK(fingerprint_bytes(a, 5) != fingerprint_bytes(a, 4));
}

NF_TEST(cooker_cooked_path_maps_content_into_cache) {
    NF_CHECK_EQ(cooked_path_for("content://Meshes/cube.nfmesh"),
                std::string("cache://Meshes/cube.nfmesh"));
    NF_CHECK_EQ(cooked_path_for("content://A/B/C.png"), std::string("cache://A/B/C.png"));
    // Something already outside content:// is left alone rather than guessed at.
    NF_CHECK_EQ(cooked_path_for("cache://x.png"), std::string("cache://x.png"));
    // A custom cache mount is honoured.
    NF_CHECK_EQ(cooked_path_for("content://M/x.nfmesh", "cache://Packed"),
                std::string("cache://Packed/M/x.nfmesh"));
}

NF_TEST(cooker_is_cookable_excludes_glsl) {
    NF_CHECK(is_cookable(".nfmesh"));
    NF_CHECK(is_cookable(".nfmat"));
    NF_CHECK(is_cookable(".nfscene"));
    NF_CHECK(is_cookable(".PNG")); // case-insensitive
    // GLSL is compiled to SPIR-V by the build, not copied by the cooker.
    // Cooking it would create a registry entry pointing at a file the cooker
    // never writes — which is exactly the stale entry this phase had to remove.
    NF_CHECK(!is_cookable(".frag"));
    NF_CHECK(!is_cookable(".vert"));
    NF_CHECK(!is_cookable(".spv"));
    NF_CHECK(!is_cookable(".nfreg"));
    NF_CHECK(!is_cookable(".txt"));
}

NF_TEST(cooker_asset_type_and_format_agree) {
    NF_CHECK(asset_type_for(".nfmesh") == AssetType::Mesh);
    NF_CHECK(asset_type_for(".nfscene") == AssetType::Scene);
    NF_CHECK(asset_type_for(".nfmat") == AssetType::Material);
    NF_CHECK(asset_type_for(".png") == AssetType::Texture);
    NF_CHECK(asset_type_for(".tga") == AssetType::Texture);
    NF_CHECK(asset_type_for(".qqq") == AssetType::Unknown);

    // The format strings must match what the editor's import queue already
    // writes, or a cooked entry and an imported entry look like two different
    // formats for the same bytes.
    NF_CHECK_EQ(format_for(AssetType::Mesh), std::string("nfmesh-v1"));
    NF_CHECK_EQ(format_for(AssetType::Texture), std::string("img-v1"));
    NF_CHECK_EQ(format_for(AssetType::Shader), std::string("spv-v1"));
    NF_CHECK_EQ(format_for(AssetType::Scene), std::string("nfscene-v1"));
    NF_CHECK_EQ(format_for(AssetType::Material), std::string("nfmat-v1"));
}

NF_TEST(cooker_cook_one_writes_the_cooked_file_and_registers_it) {
    CookSandbox sb("nf_cook_one");
    sb.write("Materials/M.nfmat", "albedo: 1 1 1\n");

    std::string err;
    bool skipped = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Materials/M.nfmat",
                      "cache://Materials/M.nfmat", err, skipped));
    NF_CHECK(!skipped);
    NF_CHECK(sb.cached("Materials/M.nfmat"));

    const auto* meta = sb.registry.find_by_path("content://Materials/M.nfmat");
    NF_CHECK(meta != nullptr);
    if (meta) {
        NF_CHECK(meta->id.valid());
        NF_CHECK(meta->type == AssetType::Material);
        NF_CHECK_EQ(meta->format, std::string("nfmat-v1"));
        NF_CHECK(!meta->fingerprint.empty());
    }
}

NF_TEST(cooker_cook_one_skips_an_unchanged_asset) {
    CookSandbox sb("nf_cook_skip");
    sb.write("Scenes/S.nfscene", "version: 1\n");

    std::string err;
    bool skipped = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Scenes/S.nfscene",
                      "cache://Scenes/S.nfscene", err, skipped));
    NF_CHECK(!skipped);

    // Second cook of identical bytes must do no work: this is what makes
    // `nf build` usable in a loop.
    bool skipped2 = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Scenes/S.nfscene",
                      "cache://Scenes/S.nfscene", err, skipped2));
    NF_CHECK(skipped2);

    // Changing the source invalidates the skip.
    sb.write("Scenes/S.nfscene", "version: 2\n");
    bool skipped3 = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Scenes/S.nfscene",
                      "cache://Scenes/S.nfscene", err, skipped3));
    NF_CHECK(!skipped3);
}

NF_TEST(cooker_re_cook_preserves_the_asset_id) {
    CookSandbox sb("nf_cook_identity");
    sb.write("Scenes/S.nfscene", "version: 1\n");

    std::string err;
    bool skipped = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Scenes/S.nfscene",
                      "cache://Scenes/S.nfscene", err, skipped));
    const auto* first = sb.registry.find_by_path("content://Scenes/S.nfscene");
    NF_CHECK(first != nullptr);
    if (!first) return;
    const AssetId original = first->id;

    sb.write("Scenes/S.nfscene", "version: 2\n");
    bool skipped2 = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Scenes/S.nfscene",
                      "cache://Scenes/S.nfscene", err, skipped2));

    const auto* second = sb.registry.find_by_path("content://Scenes/S.nfscene");
    NF_CHECK(second != nullptr);
    if (second) {
        // Scenes reference assets by AssetId. Regenerating it on every cook would
        // silently break every reference in the project.
        NF_CHECK(second->id == original);
        NF_CHECK_EQ(sb.registry.size(), size_t{1});
    }
}

NF_TEST(cooker_cook_one_rejects_a_corrupt_mesh) {
    CookSandbox sb("nf_cook_corrupt");
    sb.write("Meshes/bad.nfmesh", "this is not a mesh");

    std::string err;
    bool skipped = false;
    NF_CHECK(!cook_one(sb.vfs, sb.registry, "content://Meshes/bad.nfmesh",
                       "cache://Meshes/bad.nfmesh", err, skipped));
    NF_CHECK(!err.empty());
    // A corrupt asset must not be published: it would fail at load time in the
    // game, which is worse than failing now.
    NF_CHECK(!sb.cached("Meshes/bad.nfmesh"));
    NF_CHECK_EQ(sb.registry.size(), size_t{0});
}

NF_TEST(cooker_cook_one_reports_a_missing_input) {
    CookSandbox sb("nf_cook_missing");
    std::string err;
    bool skipped = false;
    NF_CHECK(!cook_one(sb.vfs, sb.registry, "content://Meshes/nope.nfmesh",
                       "cache://Meshes/nope.nfmesh", err, skipped));
    NF_CHECK(err.find("does not exist") != std::string::npos);
}

NF_TEST(cooker_cook_all_cooks_every_supported_file) {
    CookSandbox sb("nf_cook_all");
    NF_CHECK(sb.write_valid_mesh("Meshes/cube.nfmesh"));
    sb.write("Materials/M.nfmat", "albedo: 1 1 1\n");
    sb.write("Scenes/S.nfscene", "version: 1\n");
    sb.write("Textures/T.png", "not-really-a-png");
    // Not cookable: must be left alone rather than half-handled.
    sb.write("Shaders/thing.frag", "void main(){}");
    sb.write("AssetRegistry.nfreg", "# NOVAForge Asset Registry\nversion: 1\ncount: 0\n");

    CookReport report;
    std::string err;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", report, err));
    NF_CHECK_EQ(report.cooked, size_t{4});
    NF_CHECK_EQ(report.failed, size_t{0});
    NF_CHECK_EQ(report.total(), size_t{4});

    NF_CHECK(sb.cached("Meshes/cube.nfmesh"));
    NF_CHECK(sb.cached("Materials/M.nfmat"));
    NF_CHECK(sb.cached("Textures/T.png"));
    NF_CHECK(!sb.cached("Shaders/thing.frag"));
    NF_CHECK_EQ(sb.registry.size(), size_t{4});
}

NF_TEST(cooker_cook_all_is_idempotent) {
    CookSandbox sb("nf_cook_idem");
    NF_CHECK(sb.write_valid_mesh("Meshes/cube.nfmesh"));
    sb.write("Materials/M.nfmat", "albedo: 1 1 1\n");

    CookReport first;
    std::string err;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", first, err));
    NF_CHECK_EQ(first.cooked, size_t{2});

    CookReport second;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", second, err));
    NF_CHECK_EQ(second.cooked, size_t{0});
    NF_CHECK_EQ(second.skipped, size_t{2});
    NF_CHECK_EQ(second.failed, size_t{0});
}

NF_TEST(cooker_cook_all_names_each_failure) {
    CookSandbox sb("nf_cook_names");
    NF_CHECK(sb.write_valid_mesh("Meshes/good.nfmesh"));
    sb.write("Meshes/bad.nfmesh", "junk");

    CookReport report;
    std::string err;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", report, err));
    NF_CHECK_EQ(report.failed, size_t{1});
    NF_CHECK_EQ(report.cooked, size_t{1});
    NF_CHECK(!report.ok());
    // A bare count tells the user nothing about which asset is broken.
    NF_CHECK(err.find("bad.nfmesh") != std::string::npos);
}

NF_TEST(cooker_cook_all_fails_on_a_missing_content_root) {
    VirtualFileSystem vfs;
    vfs.mount("cache://", std::filesystem::temp_directory_path() / "nf_cook_noroot_cache");
    AssetRegistry registry;
    CookReport report;
    std::string err;
    NF_CHECK(!cook_all(vfs, registry, "content://", "cache://", report, err));
    NF_CHECK(!err.empty());
}

NF_TEST(cooker_verify_flags_a_missing_cooked_file) {
    CookSandbox sb("nf_verify_missing");
    sb.write("Scenes/S.nfscene", "version: 1\n");
    std::string err;
    bool skipped = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Scenes/S.nfscene",
                      "cache://Scenes/S.nfscene", err, skipped));

    VerifyReport ok_report;
    NF_CHECK(verify_registry(sb.vfs, sb.registry, ok_report, err));
    NF_CHECK_EQ(ok_report.ok_count, size_t{1});

    // Delete the cooked output: verify must notice, because packaging would ship
    // a registry entry pointing at nothing.
    std::filesystem::remove(sb.root / "Cache" / "Scenes" / "S.nfscene");
    VerifyReport bad_report;
    NF_CHECK(!verify_registry(sb.vfs, sb.registry, bad_report, err));
    NF_CHECK_EQ(bad_report.missing, size_t{1});
    NF_CHECK(err.find("S.nfscene") != std::string::npos);
}

NF_TEST(cooker_verify_flags_a_corrupt_cooked_mesh) {
    CookSandbox sb("nf_verify_corrupt");
    NF_CHECK(sb.write_valid_mesh("Meshes/cube.nfmesh"));
    std::string err;
    bool skipped = false;
    NF_CHECK(cook_one(sb.vfs, sb.registry, "content://Meshes/cube.nfmesh",
                      "cache://Meshes/cube.nfmesh", err, skipped));

    // Corrupt the cooked copy behind the registry's back.
    {
        std::ofstream f(sb.root / "Cache" / "Meshes" / "cube.nfmesh",
                        std::ios::binary | std::ios::trunc);
        f << "garbage";
    }
    VerifyReport report;
    NF_CHECK(!verify_registry(sb.vfs, sb.registry, report, err));
    NF_CHECK_EQ(report.bad_format, size_t{1});
    NF_CHECK_EQ(report.missing, size_t{0});
}

NF_TEST(cooker_verify_on_an_empty_registry_is_ok) {
    CookSandbox sb("nf_verify_empty");
    VerifyReport report;
    std::string err;
    NF_CHECK(verify_registry(sb.vfs, sb.registry, report, err));
    NF_CHECK_EQ(report.total, size_t{0});
    NF_CHECK(report.ok());
}

NF_TEST(cooker_prunes_entries_whose_source_is_gone) {
    CookSandbox sb("nf_cook_prune");
    sb.write("Materials/Keep.nfmat", "name: Keep\n");
    sb.write("Materials/Gone.nfmat", "name: Gone\n");

    CookReport first;
    std::string err;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", first, err));
    NF_CHECK_EQ(first.cooked, size_t{2});
    NF_CHECK_EQ(first.pruned, size_t{0});
    NF_CHECK(sb.cached("Materials/Gone.nfmat"));

    // Delete a source asset. A rebuild must not keep serving it: the walk only
    // sees files that exist, so without pruning the entry and its cooked output
    // survive and the next package ships data the project no longer contains.
    std::filesystem::remove(sb.root / "Content" / "Materials" / "Gone.nfmat");

    CookReport second;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", second, err));
    NF_CHECK_EQ(second.pruned, size_t{1});
    NF_CHECK_EQ(second.cooked, size_t{0});
    NF_CHECK_EQ(sb.registry.size(), size_t{1});
    NF_CHECK(sb.registry.find_by_path("content://Materials/Gone.nfmat") == nullptr);
    NF_CHECK(!sb.cached("Materials/Gone.nfmat"));
    NF_CHECK(sb.cached("Materials/Keep.nfmat"));
}

NF_TEST(cooker_prune_leaves_foreign_mount_entries_alone) {
    CookSandbox sb("nf_cook_prune_scope");
    sb.write("Materials/M.nfmat", "name: M\n");
    CookReport first;
    std::string err;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", first, err));

    // An entry that is not under the content mount is not ours to prune: the
    // walk cannot see it, so treating its absence as a deletion would be wrong.
    AssetMetadata foreign;
    foreign.id = AssetId::generate();
    foreign.type = AssetType::Texture;
    foreign.logical_path = "project://External/thing.png";
    foreign.cooked_path = "cache://External/thing.png";
    foreign.fingerprint = "deadbeefdeadbeef";
    foreign.format = "img-v1";
    foreign.version = 1;
    NF_CHECK(sb.registry.add(foreign, err));

    CookReport second;
    NF_CHECK(cook_all(sb.vfs, sb.registry, "content://", "cache://", second, err));
    NF_CHECK_EQ(second.pruned, size_t{0});
    NF_CHECK(sb.registry.find_by_path("project://External/thing.png") != nullptr);
}
