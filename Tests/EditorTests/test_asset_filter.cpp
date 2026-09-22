// Editor asset browser: listing, name/type filtering, double-click dispatch.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/AssetBrowser.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// --- E3: the browser shows the OPEN PROJECT's files ---------------------------
//
// The bottom panel used to list content:// unconditionally, so an author opening
// their own game saw the engine's sample content and none of their work. These
// pin the two pure rules behind the new listing (extension -> type, and how a
// prefab is told from a plain scene) plus the listing itself against a real
// project:// mount.

NF_TEST(editor_project_asset_type_mapping) {
    using assets::AssetType;
    NF_CHECK(editor::project_asset_type_for(".nfmesh") == AssetType::Mesh);
    NF_CHECK(editor::project_asset_type_for(".nfmat") == AssetType::Material);
    NF_CHECK(editor::project_asset_type_for(".nfscene") == AssetType::Scene);
    NF_CHECK(editor::project_asset_type_for(".png") == AssetType::Texture);
    NF_CHECK(editor::project_asset_type_for(".jpg") == AssetType::Texture);
    // Case-insensitive: a Windows filesystem hands back whatever the author typed.
    NF_CHECK(editor::project_asset_type_for(".NFMESH") == AssetType::Mesh);
    NF_CHECK(editor::project_asset_type_for(".NfScene") == AssetType::Scene);
    // Everything else is not a project asset and must not appear in the panel.
    NF_CHECK(editor::project_asset_type_for(".txt") == AssetType::Unknown);
    NF_CHECK(editor::project_asset_type_for("") == AssetType::Unknown);
    NF_CHECK(editor::project_asset_type_for(".spv") == AssetType::Unknown);
}

NF_TEST(editor_project_prefab_path_detection) {
    // Prefabs are ordinary .nfscene files under a Prefabs/ directory, so the path
    // is the only thing that tells them apart.
    NF_CHECK(editor::is_prefab_path("project://Prefabs/Tower.nfscene"));
    NF_CHECK(editor::is_prefab_path("project://Content/Prefabs/Tower.nfscene"));
    NF_CHECK(editor::is_prefab_path("project://Prefabs\\Tower.nfscene"));
    NF_CHECK(!editor::is_prefab_path("project://Scenes/Level1.nfscene"));
    NF_CHECK(!editor::is_prefab_path("project://Meshes/cube.nfmesh"));
}

NF_TEST(editor_project_listing_shows_only_the_projects_assets) {
    assets::VirtualFileSystem vfs;
    const auto tmp = std::filesystem::temp_directory_path() / "nf_ed_project_browser";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp / "Scenes", ec);
    std::filesystem::create_directories(tmp / "Meshes", ec);
    std::filesystem::create_directories(tmp / "Prefabs", ec);
    std::filesystem::create_directories(tmp / "Notes", ec);
    vfs.mount("project://", tmp);

    const auto touch = [](const std::filesystem::path& p) { std::ofstream(p) << "x"; };
    touch(tmp / "Scenes" / "Level1.nfscene");
    touch(tmp / "Meshes" / "cube.nfmesh");
    touch(tmp / "Prefabs" / "Tower.nfscene");
    touch(tmp / "Notes" / "readme.txt"); // not a project asset
    touch(tmp / "Scenes" / "notes.md");  // not a project asset

    const std::vector<editor::AssetEntry> got = editor::list_project_assets(vfs);

    // Only the recognised extensions, and only under project://.
    NF_CHECK(got.size() == 3u);
    for (const editor::AssetEntry& e : got) {
        NF_CHECK(e.logical_path.rfind("project://", 0) == 0);
        NF_CHECK(e.logical_path.find("readme.txt") == std::string::npos);
        NF_CHECK(e.logical_path.find("notes.md") == std::string::npos);
    }
    // Sorted, so the panel order does not depend on directory iteration order.
    for (std::size_t i = 1; i < got.size(); ++i) {
        NF_CHECK(got[i - 1].logical_path < got[i].logical_path);
    }
    // The scene entry is flagged so double-click can open it.
    bool saw_scene = false;
    for (const editor::AssetEntry& e : got) {
        if (e.logical_path.find("Level1.nfscene") != std::string::npos) {
            saw_scene = true;
            NF_CHECK(e.is_scene_file);
            NF_CHECK(e.type == assets::AssetType::Scene);
        }
    }
    NF_CHECK(saw_scene);
}

NF_TEST(editor_project_listing_is_empty_without_a_project_mount) {
    // No project:// mounted (engine-tree mode): an empty listing, never a crash
    // and never a fallback that silently shows the engine's content under a
    // project-shaped header.
    assets::VirtualFileSystem vfs;
    NF_CHECK(editor::list_project_assets(vfs).empty());
}
