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
};

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
    ShowTextureInfo  // texture entry: logical path to inspect
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
