#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace nf::project::embedded_game {

inline constexpr wchar_t kTemplate[] = L"SANAD_EXPORT_TEMPLATE";
inline constexpr wchar_t kIndex[] = L"SANAD_GAME_INDEX";
inline constexpr wchar_t kVersion[] = L"SANAD_GAME_1";
inline constexpr WORD kLanguage = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);

inline std::wstring file_resource_name(std::size_t index) {
    return L"SANAD_GAME_FILE_" + std::to_wstring(index);
}

inline std::wstring unique_id() {
    GUID id{};
    wchar_t text[39]{};
    if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, text, 39) == 0) return {};
    return std::wstring(text + 1, 36);
}

// The resource index contains relative file names, never Windows device
// names, alternate streams or paths that can escape the game data directory.
inline bool valid_file_path(const std::filesystem::path& path) {
    if (path.empty() || path.has_root_path()) return false;
    for (const auto& part : path) {
        const auto& name = part.native();
        if (name.empty() || name.back() == L'.' || name.back() == L' ' ||
            name.find_first_of(L"<>:\"|?*") != std::wstring::npos) return false;
        for (const auto ch : name) {
            if (ch < L' ') return false;
        }
        std::wstring stem = name.substr(0, name.find(L'.'));
        stem.erase(stem.find_last_not_of(L' ') + 1);
        for (auto& ch : stem) {
            if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        }
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            stem == L"CONIN$" || stem == L"CONOUT$") return false;
        if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
            std::wstring_view(L"123456789\u00b9\u00b2\u00b3").find(stem.back()) !=
                std::wstring_view::npos) return false;
    }
    return true;
}

// A staging directory is private to one export or one game startup.
struct StagingDirectory {
    std::filesystem::path path;
    ~StagingDirectory() {
        if (path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

} // namespace nf::project::embedded_game
#endif
