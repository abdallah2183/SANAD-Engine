#pragma once

// NF/Editor/ProjectLauncher.hpp — the pre-editor project launcher (UX2 item E2,
// option B).
//
// A native Win32 window shown BEFORE the VFS mounts are applied, when the user
// gave no --project. It is deliberately a separate launcher rather than an ImGui
// window inside the editor, because of how main.cpp boots:
//
//   mounts (401-466, derived from the project)  ->  window  ->  device
//   ->  swapchain  ->  Runtime (519, NEEDS the mounts)  ->  UiRenderer/ui_pass
//   ->  ImGui context
//
// The project choice determines the mounts, the mounts must exist before the
// Runtime, and no UI exists until ~90 lines after it. A picker that runs where
// its answer is needed therefore cannot be the editor's own UI without
// reordering startup. This launcher runs before all of it.
//
// Its ONLY output is a .nfproj path, which is written into cfg.project_path.
// Everything downstream is the existing, already-proven --project path,
// untouched — which is what makes this unable to regress the editor.
//
// The list, validation and path rules below are pure and header-inline so
// EditorTests can pin them without a window or a filesystem dialog.
// ProjectLauncher.cpp is a thin Win32 shell over them.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace nf::editor {

/// How many recent projects the launcher remembers.
inline constexpr std::size_t kMaxRecentProjects = 8;

/// Case-insensitive path compare. Windows paths differ only by case, and the
/// same project reached through two spellings must not occupy two rows.
inline bool project_path_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

/// Moves `path` to the FRONT of `recent`, de-duplicating case-insensitively and
/// capping the list. "Most recently used", not "insertion ordered" — reopening
/// an old project must promote it, which is the whole point of the list.
inline void remember_project(std::vector<std::string>& recent, const std::string& path) {
    if (path.empty()) {
        return;
    }
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&](const std::string& p) {
                                    return project_path_equal(p, path);
                                }),
                 recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecentProjects) {
        recent.resize(kMaxRecentProjects);
    }
}

/// Drops entries whose .nfproj no longer exists. A project the user moved or
/// deleted since last session is the normal case here, not an error — offering
/// it would just produce a failed open.
inline void prune_missing_projects(std::vector<std::string>& recent) {
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const std::string& p) {
                                    std::error_code ec;
                                    return !std::filesystem::exists(p, ec);
                                }),
                 recent.end());
}

/// A project name becomes a DIRECTORY name, so reject what the filesystem
/// cannot take or what would be ambiguous. Returns an empty string when the name
/// is acceptable, otherwise a LOCALIZATION KEY (err_name_*) for the reason —
/// never an English sentence. The shell translates it with nf::ui::tr() at
/// display time, so the Arabic UI never shows a hardcoded English reason.
/// (Tests pin emptiness only, so the key form needs no consumer change there.)
inline std::string project_name_error(const std::string& name) {
    if (name.empty()) {
        return "err_name_empty";
    }
    if (name.size() > 64) {
        return "err_name_long";
    }
    if (name == "." || name == "..") {
        return "err_name_fragment";
    }
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') {
        return "err_name_edges";
    }
    for (char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
            c == '?' || c == '*') {
            return "err_name_forbidden";
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return "err_name_control";
        }
    }
    // CON, PRN, AUX, NUL, COM1-9, LPT1-9 — reserved whatever the extension, and
    // they fail at CreateDirectory with a confusing error rather than here.
    std::string upper;
    upper.reserve(name.size());
    for (char c : name) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    const std::size_t dot = upper.find('.');
    const std::string stem = (dot == std::string::npos) ? upper : upper.substr(0, dot);
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
        return "err_name_reserved";
    }
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
        stem[3] >= '1' && stem[3] <= '9') {
        return "err_name_reserved";
    }
    return {};
}

/// `Documents` under a profile root. Pure and parameterised so the rule is
/// testable: the launcher passes the real known-folder path (which OneDrive can
/// redirect, so it is NOT simply USERPROFILE), tests pass a literal.
inline std::string documents_under(const std::string& profile_root) {
    if (profile_root.empty()) {
        return {};
    }
    std::filesystem::path p(profile_root);
    p /= "Documents";
    return p.string();
}

/// The `.nfproj` a project directory is expected to contain for `name`.
inline std::string project_file_for(const std::string& dir, const std::string& name) {
    std::filesystem::path p(dir);
    p /= (name + ".nfproj");
    return p.string();
}

/// Shell settings persisted across runs (language + window state). Stored as
/// a tiny hand-parsed settings.json under %APPDATA%/NOVAForge (no JSON library
/// in the project); the recent list stays in its own file in LOCALAPPDATA.
/// The launcher reads this at boot so it reopens in the saved language, and
/// the editor applies it too (see main.cpp) so Arabic survives a restart even
/// without --arabic.
struct LauncherSettings {
    bool arabic = false;
    bool maximized = true;
    int width = 1180;
    int height = 720;
};

/// Serialises settings to the exact JSON shape load/save use. Pure and inline
/// so tests can pin the round trip without touching %APPDATA%.
inline std::string settings_to_json(const LauncherSettings& s) {
    // Hand-built: the shape is fixed (four fields) and pulling in a JSON
    // library for it would be a dependency for a string concat.
    std::string out = "{\"language\":\"";
    out += (s.arabic ? "ar" : "en");
    out += "\",\"maximized\":";
    out += (s.maximized ? "true" : "false");
    out += ",\"width\":";
    out += std::to_string(s.width);
    out += ",\"height\":";
    out += std::to_string(s.height);
    out += "}";
    return out;
}

/// Parses what settings_to_json wrote. Unknown/missing fields keep defaults —
/// a hand-edited or older file must never break startup.
inline LauncherSettings settings_from_json(const std::string& json) {
    LauncherSettings s;
    if (json.find("\"ar\"") != std::string::npos) {
        s.arabic = true;
    }
    if (json.find("\"maximized\":false") != std::string::npos) {
        s.maximized = false;
    }
    // Minimal integer scan for "width":<n> / "height":<n> (strtol-free so the
    // header stays dependency-free; malformed numbers keep defaults).
    auto parse_int_field = [&](const char* field, int& target) {
        const std::string key = std::string("\"") + field + "\":";
        const std::size_t pos = json.find(key);
        if (pos == std::string::npos) {
            return;
        }
        std::size_t i = pos + key.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) {
            ++i;
        }
        int value = 0;
        bool any = false;
        while (i < json.size() && json[i] >= '0' && json[i] <= '9' && value < 100000) {
            value = value * 10 + (json[i] - '0');
            any = true;
            ++i;
        }
        if (any && value >= 400 && value <= 7680) {
            target = value;
        }
    };
    parse_int_field("width", s.width);
    parse_int_field("height", s.height);
    return s;
}

LauncherSettings load_settings();
void save_settings(const LauncherSettings& s);
/// Rewrites just the language field, preserving window state. Used by the live
/// toggles (shell Settings page, editor menu/radio) so every flip persists.
void save_language(bool arabic);

/// What the launcher came back with. An empty `project_path` with `quit == true`
/// means the user closed it — the caller then exits cleanly instead of booting a
/// half-initialised editor with engine-tree mounts the user did not ask for.
struct LauncherResult {
    std::string project_path;
    bool quit = false;
    // Shell pages (Settings): start the editor with the Arabic localised UI.
    // main.cpp applies it exactly like --arabic.
    bool arabic = false;
};

/// Case-insensitive substring for the Projects-page search box. Pure so tests
/// can pin it: empty needle matches everything, ASCII folding only (project
/// paths are compared, not display text).
inline bool project_matches_search(const std::string& path, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    auto lower = [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    };
    std::string hay;
    hay.reserve(path.size());
    for (char c : path) {
        hay.push_back(lower(static_cast<unsigned char>(c)));
    }
    std::string ndl;
    ndl.reserve(needle.size());
    for (char c : needle) {
        ndl.push_back(lower(static_cast<unsigned char>(c)));
    }
    return hay.find(ndl) != std::string::npos;
}

/// Shows the native launcher and blocks until the user picks a project, creates
/// one, or quits. On a non-Windows build this returns `quit = true` immediately
/// (the engine is Windows-first; this keeps the translation unit portable).
/// Takes the recent list by MUTABLE reference: the Settings page can clear it,
/// and the caller persists afterwards exactly as before.
LauncherResult run_project_launcher(std::vector<std::string>& recent_projects);

/// Recent-project persistence: a small text file, one absolute path per line,
/// stored beside the editor's own data rather than inside a project, so it
/// survives deleting the project directory.
std::vector<std::string> load_recent_projects();
void save_recent_projects(const std::vector<std::string>& recent);

/// Native folder picker (IFileDialog + FOS_PICKFOLDERS). Shared by the New
/// Project flow here and by the save-location flow in E4. False = cancelled,
/// which is a normal outcome, never an error. `start_dir` seeds the dialog.
bool pick_folder_dialog(const std::string& title, const std::string& start_dir,
                        std::string& out_dir);

/// Native "open file" picker for .nfproj files. False = cancelled.
bool pick_project_file_dialog(const std::string& title, const std::string& start_dir,
                              std::string& out_file);

} // namespace nf::editor
