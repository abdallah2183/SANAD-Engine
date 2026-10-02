// Tests/EditorTests/test_ide_open.cpp — "Open in Visual Studio" (Unity-style)
//
// The chain (VS via vswhere, VS Code, shell default) decides purely from
// explicit search inputs plus filesystem existence: no process spawns during
// selection, so every branch is pinnable with temp dirs. Only the final
// launch touches the OS, and these tests never take it — they prove the
// validation that runs before any launch instead.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/ExternalIde.hpp>

#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::editor;

namespace {

std::string g_canned_vswhere;
bool g_vswhere_fails = false;

const char* canned_runner(std::string& out_error) {
    if (g_vswhere_fails) {
        out_error = "injected failure";
        return nullptr;
    }
    return g_canned_vswhere.c_str();
}

std::filesystem::path ide_temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void touch(const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    FILE* f = nullptr;
#if defined(_MSC_VER)
    fopen_s(&f, file.string().c_str(), "wb");
#else
    f = std::fopen(file.string().c_str(), "wb");
#endif
    if (f != nullptr) {
        std::fclose(f);
    }
}

} // namespace

NF_TEST(vswhere_output_parses_to_first_line) {
    NF_CHECK_EQ(parse_vswhere_installation_path("C:\\VS2022\r\n"), std::string("C:\\VS2022"));
    NF_CHECK_EQ(parse_vswhere_installation_path(""), std::string(""));
    NF_CHECK_EQ(parse_vswhere_installation_path("\r\n  \r\n"), std::string(""));
    NF_CHECK_EQ(parse_vswhere_installation_path("\r\nC:\\Second\r\nC:\\Third\r\n"),
                std::string("C:\\Second"));
    NF_CHECK(devenv_under("").empty());
    NF_CHECK_EQ(devenv_under("C:\\VS").string(),
                (std::filesystem::path("C:\\VS") / "Common7" / "IDE" / "devenv.exe").string());
}

NF_TEST(choose_ide_prefers_live_vswhere_over_stale_fallback) {
    const auto tmp = ide_temp_dir("nf_ide_live");
    const auto live_vs = tmp / "LiveVS";
    touch(live_vs / "Common7" / "IDE" / "devenv.exe");
    const auto stale_vs = tmp / "StaleVS";
    touch(stale_vs / "Common7" / "IDE" / "devenv.exe");
    const auto vswhere = tmp / "vswhere.exe";
    touch(vswhere);

    g_vswhere_fails = false;
    g_canned_vswhere = live_vs.string();
    IdeSearchPaths search;
    search.vswhere_candidates.push_back(vswhere);
    search.devenv_fallbacks.push_back(stale_vs / "Common7" / "IDE" / "devenv.exe");
    search.vswhere_runner = &canned_runner;

    const IdeChoice choice = choose_ide(search);
    NF_CHECK(choice.kind == IdeKind::VisualStudio);
    NF_CHECK_EQ(choice.executable.string(), (live_vs / "Common7" / "IDE" / "devenv.exe").string());

    g_canned_vswhere.clear();
    std::filesystem::remove_all(tmp);
}

NF_TEST(choose_ide_falls_back_when_vswhere_yields_nothing) {
    const auto tmp = ide_temp_dir("nf_ide_fallback");
    const auto fallback_vs = tmp / "FallbackVS";
    touch(fallback_vs / "Common7" / "IDE" / "devenv.exe");
    const auto vswhere = tmp / "vswhere.exe";
    touch(vswhere);

    // vswhere exists but reports no installation: the static fallback still wins.
    g_vswhere_fails = true;
    IdeSearchPaths search;
    search.vswhere_candidates.push_back(vswhere);
    search.devenv_fallbacks.push_back(fallback_vs / "Common7" / "IDE" / "devenv.exe");
    search.vswhere_runner = &canned_runner;

    const IdeChoice choice = choose_ide(search);
    NF_CHECK(choice.kind == IdeKind::VisualStudio);
    NF_CHECK_EQ(choice.executable.string(),
                (fallback_vs / "Common7" / "IDE" / "devenv.exe").string());

    g_vswhere_fails = false;
    std::filesystem::remove_all(tmp);
}

NF_TEST(choose_ide_finds_vscode_by_install_then_path) {
    const auto tmp = ide_temp_dir("nf_ide_code");
    const auto code_exe = tmp / "VSCode" / "Code.exe";
    touch(code_exe);

    IdeSearchPaths search;
    search.code_candidates.push_back(code_exe);
    NF_CHECK(choose_ide(search).kind == IdeKind::VsCode);

    IdeSearchPaths by_path;
    by_path.path_entries.push_back(tmp / "bin");
    touch(tmp / "bin" / "code.cmd");
    const IdeChoice via_path = choose_ide(by_path);
    NF_CHECK(via_path.kind == IdeKind::VsCode);
    NF_CHECK_EQ(via_path.executable.string(), (tmp / "bin" / "code.cmd").string());

    std::filesystem::remove_all(tmp);
}

NF_TEST(choose_ide_degrades_to_shell_default_not_nothing) {
    const IdeSearchPaths empty;
    const IdeChoice choice = choose_ide(empty);
    NF_CHECK(choice.kind == IdeKind::ShellDefault);
    NF_CHECK(choice.executable.empty());
}

NF_TEST(open_in_ide_refuses_missing_files_before_any_launch) {
    const auto tmp = ide_temp_dir("nf_ide_missing");
    std::string err;
    NF_CHECK(!open_in_ide(tmp / "ghost.lua", IdeChoice{IdeKind::ShellDefault, {}}, err));
    NF_CHECK(!err.empty());
    NF_CHECK(!open_in_ide(tmp / "ghost.lua",
                           IdeChoice{IdeKind::VisualStudio, tmp / "devenv.exe"}, err));
    NF_CHECK(!err.empty());
    std::filesystem::remove_all(tmp);
}
