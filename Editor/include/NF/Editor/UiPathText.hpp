#pragma once

#include <string>
#include <string_view>

namespace nf::editor {

inline void remove_last_utf8_character(std::string& text) {
    if (text.empty()) return;
    size_t start = text.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
    text.resize(start);
}

// Only the filename extension changes. Dots in parent directories, Windows
// separators, and a leading dot in a hidden filename are not extensions.
inline std::string export_path_for_extension(std::string path, std::string_view extension) {
    const auto slash = path.find_last_of("/\\");
    const size_t filename = slash == std::string::npos ? 0 : slash + 1;
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos && dot > filename) path.resize(dot);
    path += '.';
    path += extension;
    return path;
}

} // namespace nf::editor
