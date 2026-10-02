#pragma once

// NF/Editor/ExternalIde.hpp — "Open in Visual Studio", Unity-style.
//
// Selecting an object and writing its code should not mean hunting for the
// file on disk: the inspector's Script section offers one button that opens
// the script in an IDE. The chain is deliberate and loud about what it
// picked (returned in IdeChoice::kind, shown in the console):
//   1. Visual Studio (devenv.exe) located through vswhere.exe, falling back
//      to the well-known Community/Professional/Enterprise paths;
//   2. VS Code (Code.exe) via install paths + PATH;
//   3. the shell default ("open" verb) — whatever opens .lua on this machine.
//
// Launching is detached: the editor never waits for the IDE. Everything that
// decides *what* to launch is a pure function of explicit search inputs, so
// the whole chain is unit-testable without spawning a process; only the last
// call touches the OS.

#include <filesystem>
#include <string>
#include <vector>

namespace nf::editor {

enum class IdeKind : unsigned char {
    VisualStudio = 0,
    VsCode = 1,
    ShellDefault = 2,
};

struct IdeChoice {
    IdeKind kind = IdeKind::ShellDefault;
    // Empty for ShellDefault (the shell resolves the handler).
    std::filesystem::path executable;
};

/// Explicit search inputs. Production fills them from the environment (see
/// default_search_paths); tests fill them with temp dirs.
struct IdeSearchPaths {
    // vswhere.exe candidates, in order. Empty = none installed.
    std::vector<std::filesystem::path> vswhere_candidates;
    // devenv.exe fallback locations (edition install paths), in order.
    std::vector<std::filesystem::path> devenv_fallbacks;
    // Code.exe install locations, in order.
    std::vector<std::filesystem::path> code_candidates;
    // Directories searched for code.cmd/Code.exe (usually PATH entries).
    std::vector<std::filesystem::path> path_entries;
    // Runs `vswhere -latest -property installationPath`, returning stdout.
    // Null = really execute it; tests inject canned output (or failure).
    const char* (*vswhere_runner)(std::string& out_error) = nullptr;
};

/// Production search inputs from the environment. Never throws.
IdeSearchPaths default_search_paths();

/// First line of vswhere stdout (the install path), empty when blank.
std::string parse_vswhere_installation_path(const std::string& stdout_text);

/// devenv.exe under an installation path. Empty when blank.
std::filesystem::path devenv_under(const std::string& installation_path);

/// Picks the IDE without launching anything. Pure given its inputs (only
/// filesystem existence checks, no processes).
IdeChoice choose_ide(const IdeSearchPaths& search);

/// Launches `ide` detached on `file`. False + err when the file is missing;
/// a launch failure is also an error (never silent). Never blocks.
bool open_in_ide(const std::filesystem::path& file, const IdeChoice& ide, std::string& out_error);

/// One call for panels: locate, choose, launch, and describe the pick.
bool open_file_in_ide(const std::filesystem::path& file, const IdeSearchPaths& search,
                       IdeKind& out_kind, std::string& out_error);

} // namespace nf::editor
