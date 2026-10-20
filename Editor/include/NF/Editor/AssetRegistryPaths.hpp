#pragma once

// NF/Editor/AssetRegistryPaths.hpp — the canonical spelling of a logical
// asset path, and a registry lookup that tolerates any spelling.
//
// The registry keys assets by an EXACT string match on the logical path, but
// the filesystem behind it is NTFS, where "content://Meshes/cube.nfmesh" and
// "content://Meshes/Cube.nfmesh" are the SAME file. Two spellings of one
// file therefore register as two assets with two AssetIds: the cooker's
// "unchanged, skip" check never fires, the file is cooked twice, and a scene
// that references one spelling silently stops resolving to the other.
//
// The fix lives in the registry itself (NF/Assets/AssetRegistry.hpp): the
// lookup tolerates any spelling and the registry refuses to hold two
// spellings of one file. This header is the editor's view of that rule — the
// canonical spelling it registers with, and a lookup that finds an entry
// whichever writer stored it.

#include <NF/Assets/AssetPath.hpp>
#include <NF/Assets/AssetRegistry.hpp>

#include <string>
#include <string_view>

namespace nf::editor {

// The canonical spelling of a logical path.
//
// The mount ("content://") is a keyword and is left alone; the PATH is
// lowercased on Windows, where the filesystem is case-insensitive, and left
// exactly as typed everywhere else (a case-SENSITIVE filesystem would treat
// the two spellings as two different files, and folding them would merge
// assets that are genuinely different).
inline std::string canonical_registry_path(std::string_view logical_path) {
    return assets::canonical_asset_path(logical_path);
}

// True when two logical paths name the same file on this filesystem.
inline bool same_asset_path(std::string_view a, std::string_view b) {
    return assets::same_asset_path(a, b);
}

// The registry entry for `logical_path`, whichever spelling it is stored
// under. Returns nullptr when there is none.
inline const assets::AssetMetadata* find_asset(const assets::AssetRegistry& reg,
                                               const std::string& logical_path) {
    return reg.find_by_path(logical_path);
}

// The mutable twin of find_asset(). The registry exposes its entries through
// a const accessor only, but the object is not actually const — the values
// are the very metadata the mutable find_by_path() hands out.
inline assets::AssetMetadata* find_asset(assets::AssetRegistry& reg,
                                         const std::string& logical_path) {
    return reg.find_by_path(logical_path);
}

} // namespace nf::editor
