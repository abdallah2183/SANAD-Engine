// Editor asset browser: listing, name/type filtering, double-click dispatch.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Editor/AssetBrowser.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>

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
    NF_CHECK(vfs.write_text("content://Scenes/Level.nfscene", "# SANAD Scene v1\n").ok);

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
    std::filesystem::create_directories(tmp / "Cache" / "Meshes", ec);
    vfs.mount("project://", tmp);

    const auto touch = [](const std::filesystem::path& p) { std::ofstream(p) << "x"; };
    touch(tmp / "Scenes" / "Level1.nfscene");
    touch(tmp / "Meshes" / "cube.nfmesh");
    touch(tmp / "Prefabs" / "Tower.nfscene");
    touch(tmp / "Notes" / "readme.txt"); // not a project asset
    touch(tmp / "Scenes" / "notes.md");  // not a project asset
    touch(tmp / "Cache" / "Meshes" / "cube.nfmesh"); // cooked copy: never listed

    const std::vector<editor::AssetEntry> got = editor::list_project_assets(vfs);

    // Only the recognised extensions, and only under project://.
    NF_CHECK(got.size() == 3u);
    for (const editor::AssetEntry& e : got) {
        NF_CHECK(e.logical_path.rfind("project://", 0) == 0);
        NF_CHECK(e.logical_path.find("readme.txt") == std::string::npos);
        NF_CHECK(e.logical_path.find("notes.md") == std::string::npos);
        NF_CHECK(e.logical_path.find("Cache") == std::string::npos);
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

NF_TEST(editor_asset_folder_navigation_is_pure) {
    NF_CHECK_EQ(editor::mount_of("content://Meshes/cube.nfmesh"), std::string("content://"));
    NF_CHECK_EQ(editor::mount_of("project://Scripts/a.lua"), std::string("project://"));
    NF_CHECK(editor::mount_of("other://x").empty());

    NF_CHECK_EQ(editor::asset_folder_of("content://Meshes/cube.nfmesh"),
                std::string("content://Meshes"));
    NF_CHECK_EQ(editor::asset_folder_of("content://cube.nfmesh"), std::string("content://"));
    NF_CHECK_EQ(editor::asset_folder_of("project://A/B/c.lua"), std::string("project://A/B"));
    NF_CHECK(editor::asset_folder_of("garbage").empty());

    editor::AssetEntry a, b, c;
    a.logical_path = "content://Meshes/cube.nfmesh";
    b.logical_path = "content://Meshes/Box/crate.nfmesh";
    c.logical_path = "content://Scenes/Level.nfscene";
    const std::vector<editor::AssetEntry> entries{a, b, c};

    const auto folders = editor::asset_folders(entries);
    NF_CHECK_EQ(folders.size(), 3u);
    if (folders.size() == 3) {
        NF_CHECK_EQ(folders[0], std::string("content://Meshes"));
        NF_CHECK_EQ(folders[1], std::string("content://Meshes/Box"));
        NF_CHECK_EQ(folders[2], std::string("content://Scenes"));
    }

    NF_CHECK_EQ(editor::assets_in_folder(entries, "content://Meshes").size(), 1u);
    NF_CHECK_EQ(editor::assets_in_folder(entries, "content://Meshes/Box").size(), 1u);
    NF_CHECK(editor::assets_in_folder(entries, "content://Nowhere").empty());

    const auto subs = editor::subfolders_in_folder(folders, "content://Meshes");
    NF_CHECK_EQ(subs.size(), 1u);
    if (!subs.empty()) {
        NF_CHECK_EQ(subs[0], std::string("content://Meshes/Box"));
    }
    NF_CHECK(editor::subfolders_in_folder(folders, "content://Meshes/Box").empty());

    NF_CHECK_EQ(editor::parent_folder_of("content://Meshes/Box"), std::string("content://Meshes"));
    NF_CHECK_EQ(editor::parent_folder_of("content://Meshes"), std::string("content://"));
    NF_CHECK_EQ(editor::parent_folder_of("content://"), std::string("content://"));
    NF_CHECK_EQ(editor::folder_display_name("content://Meshes/Box"), std::string("Box"));
    NF_CHECK(editor::is_root_folder(""));
    NF_CHECK(editor::is_root_folder("project://"));
    NF_CHECK(!editor::is_root_folder("project://Scripts"));

    std::string err;
    NF_CHECK(editor::valid_asset_folder("content://Scripts", err));
    NF_CHECK(editor::valid_asset_folder("project://A/B", err));
    NF_CHECK(!editor::valid_asset_folder("content://My Stuff", err));
    NF_CHECK(!editor::valid_asset_folder("content://Scripts/", err));
    NF_CHECK(!editor::valid_asset_folder("other://Scripts", err));
}

// --- FileSystem dock model: names, icons, favorites, history, crumbs --------
// Pure over strings/vectors (no ImGui), so the Godot-style dock stays thin.

NF_TEST(editor_filesystem_names_and_icons_are_pure) {
    NF_CHECK_EQ(editor::file_name_of("content://Meshes/cube.nfmesh"),
                std::string("cube.nfmesh"));
    NF_CHECK_EQ(editor::file_name_of("project://Scenes/Level1.nfscene"),
                std::string("Level1.nfscene"));
    NF_CHECK_EQ(editor::file_extension_lower("content://Meshes/CUBE.NFMESH"),
                std::string(".nfmesh"));
    NF_CHECK_EQ(editor::file_stem_of("content://Meshes/cube.nfmesh"), std::string("cube"));
    NF_CHECK(editor::file_extension_lower("content://Meshes/noext").empty());

    editor::AssetEntry mesh, scene, prefab, tex, mat, script;
    mesh.logical_path = "content://Meshes/cube.nfmesh";
    mesh.type = assets::AssetType::Mesh;
    scene.logical_path = "project://Scenes/Level1.nfscene";
    scene.type = assets::AssetType::Scene;
    prefab.logical_path = "project://Prefabs/Tower.nfscene";
    prefab.type = assets::AssetType::Scene;
    tex.logical_path = "content://Textures/brick.png";
    tex.type = assets::AssetType::Texture;
    mat.logical_path = "content://Materials/Default.nfmat";
    mat.type = assets::AssetType::Material;
    script.logical_path = "content://Scripts/player.gd";
    script.type = assets::AssetType::Unknown; // on-disk-only: extension decides
    NF_CHECK(editor::asset_icon_of(mesh) == editor::AssetIconKind::Mesh);
    NF_CHECK(editor::asset_icon_of(scene) == editor::AssetIconKind::Scene);
    NF_CHECK(editor::asset_icon_of(prefab) == editor::AssetIconKind::Prefab);
    NF_CHECK(editor::asset_icon_of(tex) == editor::AssetIconKind::Texture);
    NF_CHECK(editor::asset_icon_of(mat) == editor::AssetIconKind::Material);
    NF_CHECK(editor::asset_icon_of(script) == editor::AssetIconKind::Script);
    NF_CHECK(editor::asset_icon_for_folder() == editor::AssetIconKind::Folder);
}

NF_TEST(editor_filesystem_favorites_are_idempotent) {
    editor::AssetBrowserState st;
    NF_CHECK(!editor::is_favorite(st, "content://Meshes"));
    editor::add_favorite(st, "content://Meshes");
    NF_CHECK(editor::is_favorite(st, "content://Meshes"));
    editor::add_favorite(st, "content://Meshes"); // no duplicate
    NF_CHECK_EQ(st.favorites.size(), 1u);
    editor::toggle_favorite(st, "content://Meshes");
    NF_CHECK(!editor::is_favorite(st, "content://Meshes"));
    editor::toggle_favorite(st, "project://Scenes");
    NF_CHECK(editor::is_favorite(st, "project://Scenes"));
    editor::remove_favorite(st, "project://Scenes");
    NF_CHECK(st.favorites.empty());
    editor::remove_favorite(st, "content://Nowhere"); // silent no-op
}

NF_TEST(editor_filesystem_history_navigates_back_and_forward) {
    editor::AssetBrowserState st;
    NF_CHECK(!editor::browser_can_go_back(st));
    NF_CHECK(!editor::browser_can_go_forward(st));
    editor::browser_navigate_to(st, "content://Meshes");
    NF_CHECK_EQ(st.current_folder, std::string("content://Meshes"));
    NF_CHECK(editor::browser_can_go_back(st)); // "" pushed
    editor::browser_navigate_to(st, "content://Meshes"); // same: no-op
    NF_CHECK_EQ(st.back_stack.size(), 1u);
    editor::browser_navigate_to(st, "content://Scenes");
    NF_CHECK(editor::browser_can_go_back(st));
    NF_CHECK(editor::browser_go_back(st));
    NF_CHECK_EQ(st.current_folder, std::string("content://Meshes"));
    NF_CHECK(editor::browser_can_go_forward(st));
    NF_CHECK(editor::browser_go_forward(st));
    NF_CHECK_EQ(st.current_folder, std::string("content://Scenes"));
    NF_CHECK(!editor::browser_can_go_forward(st));
}

NF_TEST(editor_filesystem_breadcrumbs_split_and_resolve) {
    const auto parts = editor::breadcrumb_parts("content://Meshes/Box");
    NF_CHECK_EQ(parts.size(), 3u);
    if (parts.size() == 3) {
        NF_CHECK_EQ(parts[0], std::string("content://"));
        NF_CHECK_EQ(parts[1], std::string("Meshes"));
        NF_CHECK_EQ(parts[2], std::string("Box"));
    }
    NF_CHECK_EQ(editor::breadcrumb_path_at("content://Meshes/Box", 0),
                std::string("content://"));
    NF_CHECK_EQ(editor::breadcrumb_path_at("content://Meshes/Box", 1),
                std::string("content://Meshes"));
    NF_CHECK_EQ(editor::breadcrumb_path_at("content://Meshes/Box", 2),
                std::string("content://Meshes/Box"));
}

// --- Delete / rename at the app layer ---------------------------------------
//
// The VFS tests cover the filesystem refusals; these cover the layer the
// FileSystem dock actually calls, where the interesting behaviour is SESSION
// bookkeeping: a rename has to retarget the open scene, the dock selection, the
// favourites and the navigation history, or the panel navigates to a path that
// no longer exists and Save writes to a file that is not the one on screen.

namespace {

struct FileOpsFixture {
    assets::VirtualFileSystem vfs;
    assets::AssetRegistry reg;
    assets::AssetManager manager;
    editor::ConsoleBuffer console;
    editor::EditorApp app;
    std::filesystem::path tmp;

    explicit FileOpsFixture(const char* name)
        : manager(vfs, reg), app(vfs, reg, manager, console) {
        tmp = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp / "Content" / "Scenes");
        std::filesystem::create_directories(tmp / "Content" / "Meshes" / "Props");
        vfs.mount("content://", tmp / "Content");
        (void)vfs.write_text("content://Meshes/cube.nfmesh", "mesh");
        (void)vfs.write_text("content://Meshes/Props/rock.nfmesh", "rock");
        (void)vfs.write_text("content://Scenes/Level.nfscene", "# scene\n");
    }
    ~FileOpsFixture() { std::filesystem::remove_all(tmp); }
};

} // namespace

NF_TEST(editor_delete_asset_removes_the_file) {
    FileOpsFixture f("nf_ed_del_asset");
    std::string err;
    NF_CHECK(f.app.delete_asset("content://Meshes/cube.nfmesh", false, err));
    NF_CHECK(!f.vfs.exists("content://Meshes/cube.nfmesh").value);
    // A single-file delete must not touch a sibling.
    NF_CHECK(f.vfs.exists("content://Meshes/Props/rock.nfmesh").ok);
}

NF_TEST(editor_delete_asset_refuses_a_root) {
    // The refusal has to happen at BOTH layers; this is the one a user reaches
    // by right-clicking the res:// row itself.
    FileOpsFixture f("nf_ed_del_root");
    std::string err;
    NF_CHECK(!f.app.delete_asset("content://", true, err));
    NF_CHECK(!err.empty()); // and it says why, rather than failing silently
    NF_CHECK(f.vfs.exists("content://Scenes/Level.nfscene").ok);
}

NF_TEST(editor_delete_asset_refuses_a_folder_without_recursive) {
    FileOpsFixture f("nf_ed_del_dir_nonrec");
    std::string err;
    NF_CHECK(!f.app.delete_asset("content://Meshes", false, err));
    NF_CHECK(f.vfs.exists("content://Meshes/cube.nfmesh").ok);
    // ...and takes the tree when asked to, which is the folder menu item.
    NF_CHECK(f.app.delete_asset("content://Meshes", true, err));
    NF_CHECK(!f.vfs.exists("content://Meshes").value);
    NF_CHECK(f.vfs.exists("content://Scenes/Level.nfscene").ok);
}

NF_TEST(editor_delete_asset_drops_a_stale_dock_selection) {
    // Otherwise the dock keeps rendering a row for a file that is gone, and the
    // "open in IDE" item on it resolves to nothing.
    FileOpsFixture f("nf_ed_del_selection");
    f.app.browser().selected_path = "content://Meshes/cube.nfmesh";
    std::string err;
    NF_CHECK(f.app.delete_asset("content://Meshes/cube.nfmesh", false, err));
    NF_CHECK(f.app.browser().selected_path.empty());
}

NF_TEST(editor_rename_asset_retargets_the_session) {
    FileOpsFixture f("nf_ed_rename_session");
    f.app.browser().selected_path = "content://Meshes/cube.nfmesh";
    editor::add_favorite(f.app.browser(), "content://Meshes");
    editor::add_favorite(f.app.browser(), "content://Scenes");
    editor::browser_navigate_to(f.app.browser(), "content://Scenes");
    editor::browser_navigate_to(f.app.browser(), "content://Meshes");

    std::string err;
    NF_CHECK(f.app.rename_asset("content://Meshes", "content://Models", err));
    NF_CHECK(f.vfs.exists("content://Models/Props/rock.nfmesh").ok);

    editor::AssetBrowserState& bs = f.app.browser();
    // Every pointer that named a path inside the renamed tree followed it, so
    // none of them can navigate into a folder that no longer exists.
    NF_CHECK(editor::is_favorite(bs, "content://Models"));
    NF_CHECK(!editor::is_favorite(bs, "content://Meshes"));
    NF_CHECK(editor::is_favorite(bs, "content://Scenes")); // untouched
    NF_CHECK(bs.current_folder == "content://Models");
    for (const std::string& b : bs.back_stack) {
        NF_CHECK(b.find("content://Meshes") == std::string::npos);
    }
    // The dock selection followed too (it pointed at a file in the tree).
    NF_CHECK(bs.selected_path == "content://Models/cube.nfmesh");
}

NF_TEST(editor_rename_asset_rejects_a_name_with_spaces) {
    // Same rule as script creation, for the same reason: a Lua host stops at the
    // first space, so the file is created and then silently never found.
    FileOpsFixture f("nf_ed_rename_space");
    std::string err;
    NF_CHECK(!f.app.rename_asset("content://Meshes/cube.nfmesh",
                                 "content://Meshes/my cube.nfmesh", err));
    NF_CHECK(f.vfs.exists("content://Meshes/cube.nfmesh").ok);
}

NF_TEST(editor_rename_asset_to_the_same_name_is_a_no_op) {
    // Not an error the user needs to read: they typed what was already there.
    FileOpsFixture f("nf_ed_rename_same");
    std::string err;
    NF_CHECK(f.app.rename_asset("content://Meshes/cube.nfmesh",
                                "content://Meshes/cube.nfmesh", err));
    NF_CHECK(err.empty());
    NF_CHECK(f.vfs.exists("content://Meshes/cube.nfmesh").ok);
}
