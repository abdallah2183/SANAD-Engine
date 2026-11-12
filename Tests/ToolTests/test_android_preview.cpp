#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Project/AndroidPreviewExport.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
NF_TEST(android_preview_process_preserves_paths_and_reports_failure) {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = nf::test::temp_root() / ("sanad android preview " + std::to_string(stamp));
    const fs::path repo = root / "Engine With Spaces";
    const fs::path script = repo / "Tools" / "AndroidPlayer" / "build_project_preview.ps1";
    const fs::path project = root / "Game With Spaces" / "My Game.nfproj";
    fs::create_directories(project.parent_path());
    fs::create_directories(script.parent_path());
    { std::ofstream out(project); out << "test project"; }

    auto missing = nf::project::export_android_preview(project, repo);
    NF_CHECK(!missing.error.empty());

    {
        std::ofstream out(script);
        out << "param([string]$Project,[string]$OutputApk)\n"
               "[IO.File]::WriteAllText($OutputApk, $Project)\n"
               "Write-Output 'android preview process reached'\n";
    }
    auto built = nf::project::export_android_preview(project, repo);
    NF_CHECK(built.error.empty());
    NF_CHECK(fs::is_regular_file(built.apk));
    NF_CHECK(fs::is_regular_file(built.log));
    std::ifstream apk(built.apk);
    const std::string payload((std::istreambuf_iterator<char>(apk)), std::istreambuf_iterator<char>());
    NF_CHECK(payload == project.string());
    std::ifstream log(built.log);
    const std::string output((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    NF_CHECK(output.find("android preview process reached") != std::string::npos);

    { std::ofstream out(script); out << "Write-Error 'expected failure'\nexit 7\n"; }
    auto failed = nf::project::export_android_preview(project, repo);
    NF_CHECK(!failed.error.empty());
    NF_CHECK(fs::is_regular_file(failed.log));
    NF_CHECK(!fs::exists(failed.apk));
}

NF_TEST(android_preview_builds_real_project_when_configured) {
    char* project_path = nullptr;
    size_t length = 0;
    if (_dupenv_s(&project_path, &length, "SANAD_ANDROID_TEST_PROJECT") != 0 ||
        !project_path || !*project_path) {
        std::free(project_path);
        NF_SKIP("Set SANAD_ANDROID_TEST_PROJECT to opt in to the Android SDK integration test");
    }
    const std::filesystem::path project(project_path);
    std::free(project_path);
    const auto repo = std::filesystem::path(NF_TEMPLATE_DIR).parent_path().parent_path();
    const auto built = nf::project::export_android_preview(project, repo);
    NF_CHECK(built.error.empty());
    NF_CHECK(std::filesystem::is_regular_file(built.apk));
    if (std::filesystem::is_regular_file(built.apk)) NF_CHECK(std::filesystem::file_size(built.apk) > 0);
    NF_CHECK(std::filesystem::is_regular_file(built.log));
}
#endif
