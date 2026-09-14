// Editor asset browser: listing, name/type filtering, double-click dispatch.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/AssetBrowser.hpp>

using namespace nf;

static void mount_tmp(assets::VirtualFileSystem& vfs, const std::filesystem::path& tmp) {
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
}

NF_TEST(editor_asset_list_and_filter) {
    assets::VirtualFileSystem vfs;
    const auto tmp = std::filesystem::temp_directory_path() / "nf_ed_assets";
    mount_tmp(vfs, tmp);

    // One cooked mesh in the registry + one scene file on disk.
    const assets::AssetId mesh_id = assets::AssetId::generate();
    assets::AssetMetadata meta;
    meta.id = mesh_id;
    meta.type = assets::AssetType::Mesh;
    meta.logical_path = "content://Meshes/cube.nfmesh";
    meta.cooked_path = "cache://Meshes/cube.nfmesh";
    const std::vector<uint8_t> blob{1, 2, 3, 4};
    NF_CHECK(vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(blob)).ok);
    assets::AssetRegistry reg;
    std::string err;
    NF_CHECK(reg.add(meta, err));
    NF_CHECK(vfs.write_text("content://Scenes/Level.nfscene", "# NOVAForge Scene v1\n").ok);

    auto all = editor::list_content_assets(vfs, reg);
    NF_CHECK(all.size() >= 2u);

    auto cubes = editor::filter_assets(all, "cube", -1);
    NF_CHECK_EQ(cubes.size(), 1u);
    NF_CHECK(cubes[0].type == assets::AssetType::Mesh);
    NF_CHECK(cubes[0].has_cooked);

    auto scenes = editor::filter_assets(all, "", static_cast<int>(assets::AssetType::Scene));
    NF_CHECK_EQ(scenes.size(), 1u);
    NF_CHECK(scenes[0].logical_path == "content://Scenes/Level.nfscene");

    // Case-insensitive name match + type mismatch yields nothing.
    NF_CHECK(editor::filter_assets(all, "CUBE", -1).size() == 1u);
    NF_CHECK(editor::filter_assets(all, "cube", static_cast<int>(assets::AssetType::Scene)).empty());
    NF_CHECK(editor::filter_assets(all, "no-such-asset", -1).empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_asset_double_click) {
    editor::AssetEntry scene;
    scene.logical_path = "content://Scenes/Level.nfscene";
    scene.type = assets::AssetType::Scene;
    scene.is_scene_file = true;
    std::string info;
    NF_CHECK(editor::classify_double_click(scene, info) == editor::AssetOpenAction::OpenScene);
    NF_CHECK(info == "content://Scenes/Level.nfscene");

    editor::AssetEntry mesh;
    mesh.logical_path = "content://Meshes/cube.nfmesh";
    mesh.type = assets::AssetType::Mesh;
    mesh.id = assets::AssetId::generate();
    mesh.has_id = true;
    mesh.has_cooked = true;
    mesh.cooked_path = "cache://Meshes/cube.nfmesh";
    NF_CHECK(editor::classify_double_click(mesh, info) == editor::AssetOpenAction::ShowMeshInfo);
    NF_CHECK(info.find("cube.nfmesh") != std::string::npos);
}
