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

} // namespace nf::editor
