#pragma once

#include <string_view>

namespace nf::editor {

// The rename dialog edits a single file or folder name, never a path. Keep
// UTF-8 bytes intact while rejecting Windows separators and invalid leaf names.
inline bool valid_asset_rename_leaf(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
        return false;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 ||
            std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos)
            return false;
    }
    return true;
}

} // namespace nf::editor
