#include <NF/Editor/ExternalIde.hpp>

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace nf::editor {

namespace {

std::string narrow_env(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec) && !ec;
}

std::vector<std::filesystem::path> split_path_env(const std::string& path_env) {
    std::vector<std::filesystem::path> out;
    size_t start = 0;
    while (start <= path_env.size()) {
        const size_t end = path_env.find(';', start);
        const std::string piece = path_env.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

#if defined(_WIN32)
std::string run_vswhere_capture(const std::filesystem::path& vswhere, std::string& out_error) {
    // vswhere prints the install path and nothing else with these flags.
    const std::string command =
        "\"" + vswhere.string() + "\" -latest -products * -property installationPath 2>nul";
    std::string output;
    FILE* pipe = nullptr;
#if defined(_MSC_VER)
    pipe = _popen(command.c_str(), "r");
#else
    pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        out_error = "failed to run vswhere";
        return {};
    }
    char chunk[512];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) {
        output += chunk;
    }
#if defined(_MSC_VER)
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    if (status != 0) {
        out_error = "vswhere reported no installation";
        return {};
    }
    return output;
}
#endif

} // namespace

IdeSearchPaths default_search_paths() {
    IdeSearchPaths search;
#if defined(_WIN32)
    const std::string program_files_x86 = narrow_env("ProgramFiles(x86)");
    const std::string program_files = narrow_env("ProgramFiles");
    const std::string local_app_data = narrow_env("LOCALAPPDATA");
    if (!program_files_x86.empty()) {
        search.vswhere_candidates.emplace_back(
            std::filesystem::path(program_files_x86) / "Microsoft Visual Studio" / "Installer" /
            "vswhere.exe");
    }
    if (!program_files.empty()) {
        search.vswhere_candidates.emplace_back(
            std::filesystem::path(program_files) / "Microsoft Visual Studio" / "Installer" /
            "vswhere.exe");
    }
    // Static devenv.exe fallbacks when vswhere is missing or reports nothing.
    // VS installs 64-bit under ProgramFiles since 2022 (this machine runs
    // VS 18 under "C:\Program Files\..."), and the version directory is no
    // longer always "2022" — cover 18/2026, 17/2022 and 16/2019 in both
    // Program Files roots, newest first.
    const char* roots[2] = {program_files.c_str(), program_files_x86.c_str()};
    for (const char* root : roots) {
        if (root == nullptr || *root == '\0') {
            continue;
        }
        for (const char* version : {"18", "2026", "17", "2022", "16", "2019"}) {
            for (const char* edition : {"Enterprise", "Professional", "Community"}) {
                search.devenv_fallbacks.emplace_back(std::filesystem::path(root) /
                                                     "Microsoft Visual Studio" / version / edition /
                                                     "Common7" / "IDE" / "devenv.exe");
            }
        }
    }
    if (!program_files.empty()) {
        search.code_candidates.emplace_back(
            std::filesystem::path(program_files) / "Microsoft VS Code" / "Code.exe");
    }
    if (!local_app_data.empty()) {
        search.code_candidates.emplace_back(
            std::filesystem::path(local_app_data) / "Programs" / "Microsoft VS Code" / "Code.exe");
    }
    search.path_entries = split_path_env(narrow_env("PATH"));
#else
    (void)search;
#endif
    return search;
}

std::string parse_vswhere_installation_path(const std::string& stdout_text) {
    // First non-empty line, trailing \r stripped: vswhere ends its line with
    // CRLF, and a path carrying \r would never resolve.
    size_t pos = 0;
    while (pos < stdout_text.size()) {
        size_t end = stdout_text.find('\n', pos);
        std::string line = stdout_text.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            return line.substr(first);
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return {};
}

std::filesystem::path devenv_under(const std::string& installation_path) {
    if (installation_path.empty()) {
        return {};
    }
    return std::filesystem::path(installation_path) / "Common7" / "IDE" / "devenv.exe";
}

IdeChoice choose_ide(const IdeSearchPaths& search) {
    // 1. Visual Studio via vswhere (live query first: an installed-but-moved
    //    VS still resolves, while a stale fallback path would 404).
    for (const auto& vswhere : search.vswhere_candidates) {
        if (!file_exists(vswhere)) {
            continue;
        }
        std::string runner_error;
        std::string output;
        if (search.vswhere_runner != nullptr) {
            const char* injected = search.vswhere_runner(runner_error);
            if (injected != nullptr) {
                output = injected;
            }
        }
#if defined(_WIN32)
        else {
            output = run_vswhere_capture(vswhere, runner_error);
        }
#endif
        const std::filesystem::path devenv = devenv_under(parse_vswhere_installation_path(output));
        if (!devenv.empty() && file_exists(devenv)) {
            return IdeChoice{IdeKind::VisualStudio, devenv};
        }
        // This vswhere yielded nothing usable: try the next candidate before
        // falling through to the static fallbacks, then to Code.
        continue;
    }
    for (const auto& devenv : search.devenv_fallbacks) {
        if (file_exists(devenv)) {
            return IdeChoice{IdeKind::VisualStudio, devenv};
        }
    }
    // 2. VS Code: install locations, then PATH (code.cmd shim or Code.exe).
    for (const auto& code : search.code_candidates) {
        if (file_exists(code)) {
            return IdeChoice{IdeKind::VsCode, code};
        }
    }
    for (const auto& dir : search.path_entries) {
        for (const char* name : {"code.cmd", "Code.exe", "code"}) {
            const std::filesystem::path candidate = dir / name;
            if (file_exists(candidate)) {
                return IdeChoice{IdeKind::VsCode, candidate};
            }
        }
    }
    // 3. Whatever the shell opens .lua with. Not nothing: erroring here
    //    would strand a user whose only editor is Notepad++.
    return IdeChoice{IdeKind::ShellDefault, {}};
}

bool open_in_ide(const std::filesystem::path& file, const IdeChoice& ide, std::string& out_error) {
    if (!file_exists(file)) {
        out_error = "File does not exist: " + file.string();
        return false;
    }
#if !defined(_WIN32)
    (void)ide;
    out_error = "external IDE launching is Windows-only in this build";
    return false;
#else
    if (ide.kind == IdeKind::ShellDefault || ide.executable.empty()) {
        const HINSTANCE rc = ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(rc) <= 32) {
            out_error = "Shell could not open '" + file.string() + "'";
            return false;
        }
        return true;
    }
    // Detached: no handle inheritance, no wait — the IDE outlives the click.
    std::wstring command_line = L"\"" + ide.executable.wstring() + L"\" \"" + file.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started =
        CreateProcessW(ide.executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                       CREATE_DEFAULT_ERROR_MODE | DETACHED_PROCESS, nullptr, nullptr, &startup,
                       &process);
    if (started == 0) {
        out_error = "failed to start '" + ide.executable.string() + "' (Windows error " +
                    std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#endif
}

bool open_file_in_ide(const std::filesystem::path& file, const IdeSearchPaths& search,
                       IdeKind& out_kind, std::string& out_error) {
    const IdeChoice choice = choose_ide(search);
    out_kind = choice.kind;
    return open_in_ide(file, choice, out_error);
}

} // namespace nf::editor
