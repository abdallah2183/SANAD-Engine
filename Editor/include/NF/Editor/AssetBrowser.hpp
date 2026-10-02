#pragma once

// NF/Editor/AssetBrowser.hpp — content browser model over VFS + AssetRegistry.
//
// Entries come from two sources: registry metadata (type, AssetId, cooked
// state) and a filesystem scan of content:// for scene files (*.nfscene),
// which are not registry assets. Filtering is pure (name substring,
// case-insensitive, plus type). Double-click dispatch and mesh drag & drop
// produce plain data/commands — no UI toolkit types here.

#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetTypes.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/ECS/ECS.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nf::editor {

class ICommand;

struct AssetEntry {
    std::string logical_path;
    assets::AssetType type = assets::AssetType::Unknown;
    assets::AssetId id;
    bool has_id = false;
    bool has_cooked = false;
    std::string cooked_path;
    bool is_scene_file = false;
};

struct AssetBrowserState {
    std::string filter_text;
    // -1 means "all types", otherwise a valid assets::AssetType value.
    int filter_type = -1;
    std::string selected_path;
    // Which tree the panel shows: 0 = engine Content (content://), 1 = the
    // open project's files (project://), Unity's Assets-vs-Packages split.
    int browser_root = 0;
    // Folder the panel is navigated into ("" = the root itself). Folders are
    // parent paths of the listing, so an empty folder appears once it holds
    // a file; the current folder persists even so, and files land in it.
    std::string current_folder;
    // --- FileSystem dock (Godot-style) ------------------------------------
    // Favorites bar above the folder tree. Entries are folder logical paths
    // ("content://Meshes", "project://Scenes"). Pure strings so tests can
    // pin add/remove/toggle without a panel.
    std::vector<std::string> favorites;
    // Navigation history for Back/Forward. navigate_to() pushes the previous
    // folder onto back_stack and clears forward_stack; go_back/forward move
    // current_folder between the stacks. All pure over the state struct.
    std::vector<std::string> back_stack;
    std::vector<std::string> forward_stack;
    // 0 = detailed list, 1 = icon grid (thumbnails). Persisted per session.
    int view_mode = 0;
    // Top tabs of the dock: 0 = FileSystem, 1 = History (visited folders).
    int file_tab = 0;
};

// --- Unity-style folder navigation (pure over a listing) -------------------
//
// The panel used to be one flat filtered list, which answers "find this file"
// but never "what is in my game". These helpers turn the same listing into a
// navigable tree: folders first, files of the current folder below, a flat
// search only while the filter box is non-empty.

/// "content://" or "project://" prefix of a logical path, "" when neither.
std::string mount_of(const std::string& logical_path);
/// Parent folder of a file ("content://Meshes/cube.nfmesh" -> "content://Meshes").
/// A root-level file folds to its mount ("content://x" -> "content://").
std::string asset_folder_of(const std::string& logical_path);
/// Sorted unique folders present in the listing.
std::vector<std::string> asset_folders(const std::vector<AssetEntry>& entries);
/// Direct file children of `folder` ("" = mount root), sorted by path.
std::vector<AssetEntry> assets_in_folder(const std::vector<AssetEntry>& entries,
                                         const std::string& folder);
/// Direct subfolders of `folder`, sorted.
std::vector<std::string> subfolders_in_folder(const std::vector<std::string>& folders,
                                              const std::string& folder);
/// Parent of a folder ("content://Meshes/Box" -> "content://Meshes").
/// The mount root is its own parent, so Up never escapes the tree.
std::string parent_folder_of(const std::string& folder);
/// Last segment for display ("content://Meshes" -> "Meshes").
std::string folder_display_name(const std::string& folder);
/// True when `folder` is a mount root or empty (the navigation top).
bool is_root_folder(const std::string& folder);
/// Path validation shared by folder creation: mount prefix, no spaces (script
/// paths stop at the first space, so a spaced folder would strand them).
bool valid_asset_folder(const std::string& logical_dir, std::string& out_err);

// --- FileSystem dock model (Godot-style favorites / history / icons) --------
//
// All pure over strings/vectors so the panel stays thin and the rules stay
// testable without ImGui. The panel calls navigate_to() on every folder
// change so Back/Forward always work; direct assignment of current_folder
// (root switch, tests) intentionally bypasses history.

/// File name with extension ("content://Meshes/cube.nfmesh" -> "cube.nfmesh").
std::string file_name_of(const std::string& logical_path);
/// Lowercased extension with dot (".NFMESH" -> ".nfmesh", no ext -> "").
std::string file_extension_lower(const std::string& logical_path);
/// Stem without extension ("cube.nfmesh" -> "cube").
std::string file_stem_of(const std::string& logical_path);

/// Icon kind the FileSystem dock draws (drawn with ImDrawList, never emoji).
enum class AssetIconKind {
    Folder,
    Mesh,
    Scene,
    Prefab,
    Material,
    Texture,
    Script,
    Shader,
    Audio,
    Unknown,
};

/// Icon for a browser entry (prefab detected by path, like the listing).
AssetIconKind asset_icon_of(const AssetEntry& entry);
/// Icon for a folder path (always Folder) — keeps call sites uniform.
AssetIconKind asset_icon_for_folder();

/// Favorites helpers (idempotent add, silent-no-op remove).
bool is_favorite(const AssetBrowserState& state, const std::string& folder);
void add_favorite(AssetBrowserState& state, const std::string& folder);
void remove_favorite(AssetBrowserState& state, const std::string& folder);
void toggle_favorite(AssetBrowserState& state, const std::string& folder);

/// Pushes current onto back history and navigates. No-op when same.
void browser_navigate_to(AssetBrowserState& state, const std::string& folder);
bool browser_can_go_back(const AssetBrowserState& state);
bool browser_can_go_forward(const AssetBrowserState& state);
bool browser_go_back(AssetBrowserState& state);
bool browser_go_forward(AssetBrowserState& state);

/// Breadcrumb segments for display, e.g. "content://Meshes/Box" ->
/// {"content://", "Meshes", "Box"}. Root alone -> {"content://"}.
std::vector<std::string> breadcrumb_parts(const std::string& folder);
/// Logical path of breadcrumb index i (clicking segment i navigates there).
std::string breadcrumb_path_at(const std::string& folder, size_t index);

// Lists content:// assets (registry + on-disk scene files).
std::vector<AssetEntry> list_content_assets(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg);

// Pure filter over a listing.
std::vector<AssetEntry> filter_assets(const std::vector<AssetEntry>& entries, const std::string& text,
                                      int type_filter);

// Double-click target classification for the shell to dispatch.
enum class AssetOpenAction {
    None,
    OpenScene,       // logical scene path to open
    ShowMeshInfo,    // mesh entry: display metadata
    ShowMaterialInfo, // material entry: logical path to inspect
    ShowTextureInfo, // texture entry: logical path to inspect
    OpenInIde        // script entry: logical path to open in the external IDE
};
AssetOpenAction classify_double_click(const AssetEntry& entry, std::string& out_info);

// Builds the "drag mesh into viewport" command (new entity + Transform +
// Name + MeshComponent). Returns nullptr with err when the entry is not a mesh.
std::unique_ptr<ICommand> make_drop_mesh_command(const AssetEntry& entry, const std::string& entity_name,
                                                 ecs::Entity parent, std::string& out_err);

// --- UX2 item E3: the browser shows the OPEN PROJECT, not the engine tree -----

/// The asset type a file extension maps to inside a project, or Unknown when
/// the extension is not a project asset. Pure, so the mapping is testable.
/// `.nfscene` is a Scene; a prefab is also a `.nfscene` (prefabs are scenes under
/// `Prefabs/`), so the caller distinguishes them by path, not by extension.
assets::AssetType project_asset_type_for(const std::string& extension);

/// True when `logical_path` sits under a Prefabs/ directory. Prefabs are ordinary
/// .nfscene files, so this is the only thing that tells them apart in a listing.
bool is_prefab_path(const std::string& logical_path);

/// Lists the OPEN PROJECT's own files under `project://` — meshes, materials,
/// scenes and prefabs — instead of the engine tree's `content://`. This is what
/// the bottom panel shows once a project is open, so the browser answers "what is
/// in MY game" rather than "what shipped with the engine".
///
/// Sorted by logical path so the listing is deterministic (the project rule:
/// never let iteration order leak into output).
std::vector<AssetEntry> list_project_assets(assets::VirtualFileSystem& vfs);

} // namespace nf::editor
