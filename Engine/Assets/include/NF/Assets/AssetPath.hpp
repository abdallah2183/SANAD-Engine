#pragma once

// NF/Assets/AssetPath.hpp — the canonical spelling of a logical asset path.
//
// The asset registry keys assets by an EXACT string match on the logical path,
// but the filesystem behind it is NTFS on Windows, where
// "content://Meshes/cube.nfmesh" and "content://meshes/cube.nfmesh" are the
// SAME file. Two spellings of one file therefore register as two assets with
// two AssetIds: the cooker's "unchanged, skip" check never fires, the file is
// cooked twice, and a scene that references one spelling silently stops
// resolving to the other.
//
// The canonical spelling is the lowercased path on a case-insensitive
// filesystem, and the path exactly as typed everywhere else (a case-SENSITIVE
// filesystem treats the two spellings as two different files, and folding them
// would merge assets that genuinely differ).

#include <cctype>
#include <string>
#include <string_view>

namespace nf::assets {

// The canonical spelling of a logical path.
//
// The mount ("content://") is a keyword and is left alone; the PATH is
// lowercased on Windows, where the filesystem is case-insensitive.
inline std::string canonical_asset_path(std::string_view logical_path) {
#if defined(_WIN32)
    const std::size_t sep = logical_path.find("://");
    if (sep == std::string_view::npos) {
        return std::string(logical_path);
    }
    std::string out(logical_path.substr(0, sep + 3));
    for (const char c : logical_path.substr(sep + 3)) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
#else
    return std::string(logical_path);
#endif
}

// True when two logical paths name the same file on this filesystem.
inline bool same_asset_path(std::string_view a, std::string_view b) {
    if (a == b) {
        return true;
    }
#if defined(_WIN32)
    return canonical_asset_path(a) == canonical_asset_path(b);
#else
    return false;
#endif
}

} // namespace nf::assets
