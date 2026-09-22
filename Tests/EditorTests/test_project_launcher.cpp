// EditorTests — project launcher core (UX2 item E2, option B).
//
// The launcher is a native Win32 window, but every decision it makes is a pure
// inline function in NF/Editor/ProjectLauncher.hpp: the recent-project list's
// ordering and de-duplication, project-name validation, the Documents default,
// and the .nfproj path. Those are what these tests pin.
//
// Pure CPU: no window, no COM, no filesystem dialog. The Win32 shell around it
// (run_project_launcher, pick_folder_dialog) is deliberately not covered here —
// it is a thin wrapper whose only policy would be a bug worth fixing by moving
// it into the header.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/ProjectLauncher.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::editor;

namespace {

std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

} // namespace

// --- recent list -------------------------------------------------------------

NF_TEST(launcher_recent_is_most_recently_used_first) {
    std::vector<std::string> recent;
    remember_project(recent, "C:/proj/A.nfproj");
    remember_project(recent, "C:/proj/B.nfproj");
    remember_project(recent, "C:/proj/C.nfproj");
    NF_CHECK(recent.size() == 3u);
    NF_CHECK(recent[0] == "C:/proj/C.nfproj"); // newest first
    NF_CHECK(recent[1] == "C:/proj/B.nfproj");
    NF_CHECK(recent[2] == "C:/proj/A.nfproj");
}

NF_TEST(launcher_reopening_promotes_instead_of_duplicating) {
    std::vector<std::string> recent;
    remember_project(recent, "A");
    remember_project(recent, "B");
    remember_project(recent, "C");
    remember_project(recent, "A"); // reopen the oldest
    NF_CHECK(recent.size() == 3u); // not four
    NF_CHECK(recent[0] == "A");
    NF_CHECK(recent[1] == "C");
    NF_CHECK(recent[2] == "B");
}

NF_TEST(launcher_recent_dedupes_case_insensitively) {
    // Windows paths differ only by case, so the same project reached through two
    // spellings must not occupy two rows.
    std::vector<std::string> recent;
    remember_project(recent, "C:/Proj/MyGame.nfproj");
    remember_project(recent, "c:/proj/mygame.NFPROJ");
    NF_CHECK(recent.size() == 1u);
    NF_CHECK(recent[0] == "c:/proj/mygame.NFPROJ"); // the spelling just used wins
}

NF_TEST(launcher_recent_is_capped) {
    std::vector<std::string> recent;
    for (int i = 0; i < static_cast<int>(kMaxRecentProjects) + 5; ++i) {
        remember_project(recent, "P" + std::to_string(i));
    }
    NF_CHECK(recent.size() == kMaxRecentProjects);
    // The cap keeps the NEWEST entries, and the newest is first.
    NF_CHECK(recent.front() == "P" + std::to_string(kMaxRecentProjects + 4));
}

NF_TEST(launcher_recent_ignores_an_empty_path) {
    std::vector<std::string> recent;
    remember_project(recent, "");
    NF_CHECK(recent.empty());
}

NF_TEST(launcher_prunes_projects_that_no_longer_exist) {
    const auto tmp = temp_dir_for("nf_launcher_prune");
    const auto real = tmp / "Real.nfproj";
    {
        std::ofstream(real) << "stub";
    }
    std::vector<std::string> recent = {real.string(), (tmp / "Gone.nfproj").string()};
    prune_missing_projects(recent);
    NF_CHECK(recent.size() == 1u);
    NF_CHECK(recent[0] == real.string());
}

// --- name validation ---------------------------------------------------------

NF_TEST(launcher_accepts_ordinary_project_names) {
    NF_CHECK(project_name_error("MyGame").empty());
    NF_CHECK(project_name_error("SANAD Engine 2").empty());
    NF_CHECK(project_name_error("محرك سند").empty()); // Arabic is a valid name
    NF_CHECK(project_name_error("a").empty());
}

NF_TEST(launcher_rejects_names_the_filesystem_cannot_take) {
    NF_CHECK(!project_name_error("").empty());          // empty
    NF_CHECK(!project_name_error(".").empty());         // path fragment
    NF_CHECK(!project_name_error("..").empty());
    NF_CHECK(!project_name_error(" leading").empty());  // leading space
    NF_CHECK(!project_name_error("trailing ").empty()); // trailing space
    NF_CHECK(!project_name_error("trailing.").empty()); // trailing dot
    NF_CHECK(!project_name_error("a/b").empty());       // separator
    NF_CHECK(!project_name_error("a\\b").empty());
    NF_CHECK(!project_name_error("a:b").empty()); // reserved char
    NF_CHECK(!project_name_error("a*b").empty());
    NF_CHECK(!project_name_error("a?b").empty());
    NF_CHECK(!project_name_error("a\"b").empty());
    NF_CHECK(!project_name_error("a<b").empty());
    NF_CHECK(!project_name_error("a>b").empty());
    NF_CHECK(!project_name_error("a|b").empty());
    NF_CHECK(std::string(65, 'x').size() == 65u);
    NF_CHECK(!project_name_error(std::string(65, 'x')).empty()); // too long
}

NF_TEST(launcher_rejects_reserved_windows_device_names) {
    // These fail at CreateDirectory with a confusing error, so they are caught
    // here instead. The check is case-insensitive and ignores any extension.
    for (const char* n : {"CON", "con", "PRN", "AUX", "NUL", "COM1", "LPT9", "con.txt", "Com3"}) {
        NF_CHECK(!project_name_error(n).empty());
    }
    // Near-misses must still be allowed: these are not reserved.
    for (const char* n : {"CONSOLE", "COM0", "COM10", "LPT", "NULL"}) {
        NF_CHECK(project_name_error(n).empty());
    }
}

// --- paths -------------------------------------------------------------------

NF_TEST(launcher_documents_default_is_documents_under_the_profile) {
    const std::string got = documents_under("C:/Users/someone");
    NF_CHECK(got.find("Documents") != std::string::npos);
    NF_CHECK(got.rfind("C:/Users/someone", 0) == 0);
    // An empty root yields an empty path rather than "Documents" relative to
    // whatever the working directory happens to be.
    NF_CHECK(documents_under("").empty());
}

NF_TEST(launcher_project_file_is_named_after_the_project) {
    const std::string got = project_file_for("C:/work", "MyGame");
    NF_CHECK(got.find("MyGame.nfproj") != std::string::npos);
    NF_CHECK(got.rfind("C:/work", 0) == 0);
}

// --- shell settings JSON -------------------------------------------------------
// Pure inline round trip (no %APPDATA% touch): language + window state survive
// serialisation, garbage keeps safe defaults, absurd sizes are clamped out.

NF_TEST(launcher_settings_json_round_trip) {
    LauncherSettings s;
    s.arabic = true;
    s.maximized = false;
    s.width = 1600;
    s.height = 900;
    const LauncherSettings back = settings_from_json(settings_to_json(s));
    NF_CHECK(back.arabic);
    NF_CHECK(!back.maximized);
    NF_CHECK(back.width == 1600);
    NF_CHECK(back.height == 900);
}

NF_TEST(launcher_settings_json_defaults_are_english_maximized) {
    const LauncherSettings s = settings_from_json("");
    NF_CHECK(!s.arabic);
    NF_CHECK(s.maximized);
    const LauncherSettings garbage = settings_from_json("{oops");
    NF_CHECK(!garbage.arabic);
    NF_CHECK(garbage.maximized);
}

NF_TEST(launcher_settings_json_rejects_absurd_sizes) {
    const LauncherSettings tiny = settings_from_json(
        "{\"language\":\"ar\",\"maximized\":false,\"width\":50,\"height\":20}");
    NF_CHECK(tiny.arabic); // language still reads — only sizes are guarded
    NF_CHECK(tiny.width == 1180);
    NF_CHECK(tiny.height == 720);
    const LauncherSettings huge = settings_from_json(
        "{\"language\":\"en\",\"maximized\":false,\"width\":99999,\"height\":99999}");
    NF_CHECK(huge.width == 1180);
    NF_CHECK(huge.height == 720);
}

// --- name-error keys -----------------------------------------------------------
// project_name_error returns LOCALIZATION keys (err_name_*), not English
// sentences: the shell translates them at display time. Emptiness is pinned
// above; here the key form itself is pinned.

NF_TEST(launcher_name_errors_are_localization_keys) {
    // Note: the 65-char case uses a named string — a temporary's c_str()
    // inside the list below would dangle before the loop body runs.
    const std::string too_long(65, 'x');
    const char* bad[] = {"", ".", "..", " leading", "a/b",
                         "a:b", "CON", "com1", too_long.c_str()};
    for (const char* name : bad) {
        const std::string err = project_name_error(name);
        NF_CHECK(!err.empty());
        NF_CHECK(err.rfind("err_name_", 0) == 0);
    }
    NF_CHECK(project_name_error("MyGame").empty());
}

// --- shell search filter -------------------------------------------------------

NF_TEST(launcher_search_empty_matches_everything) {
    NF_CHECK(project_matches_search("C:/games/MyGame/MyGame.nfproj", ""));
    NF_CHECK(project_matches_search("", ""));
}

NF_TEST(launcher_search_is_case_insensitive_substring) {
    NF_CHECK(project_matches_search("C:/games/SandRunner/SandRunner.nfproj", "sand"));
    NF_CHECK(project_matches_search("C:/games/SandRunner/SandRunner.nfproj", "SAND"));
    NF_CHECK(project_matches_search("C:/games/SandRunner/SandRunner.nfproj", "Runner"));
    NF_CHECK(!project_matches_search("C:/games/SandRunner/SandRunner.nfproj", "oasis"));
    NF_CHECK(!project_matches_search("C:/games/SandRunner/SandRunner.nfproj", "sandrunnerx"));
}
