// Tests/RuntimeTests/test_run_config.cpp — project-aware run configuration
//
// resolve_run_config holds the precedence rules that used to be buried in
// Application::run() next to the window and device setup: a project supplies the
// mounts, title, window size and startup scene; an explicit scene still wins;
// and without a project the engine-tree walk-up behaves exactly as before.
//
// These are asserted here rather than by running a full Application, which would
// need a real window and device to test a decision that is pure policy.

#include <NF/Test/TestFramework.hpp>
#include <NF/Runtime/RunConfigResolver.hpp>
#include <NF/Runtime/Application.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <filesystem>
#include <fstream>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;

namespace {

std::filesystem::path cfg_temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

} // namespace

NF_TEST(run_config_without_project_falls_back_to_the_engine_tree) {
    ApplicationConfig cfg; // no project_path
    VirtualFileSystem vfs;
    ResolvedRunConfig out;
    std::string err;

    NF_CHECK(resolve_run_config(cfg, vfs, out, err));
    // No project means no project identity, and the historical default scene.
    NF_CHECK(out.project_name.empty());
    NF_CHECK_EQ(out.scene_path, std::string(kDefaultScenePath));
    // The legacy path mounts content:// even when it cannot find a Content dir.
    NF_CHECK(vfs.resolve("content://anything.txt").ok);
}

NF_TEST(run_config_project_supplies_mounts) {
    const auto root = cfg_temp_dir("nf_run_cfg_mounts");
    std::filesystem::create_directories(root / "Assets");
    write_file(root / "Demo.nfproj",
               "version: 1\nname: Demo\nmount: content:// -> Assets\n");

    ApplicationConfig cfg;
    cfg.project_path = (root / "Demo.nfproj").string();

    VirtualFileSystem vfs;
    ResolvedRunConfig out;
    std::string err;
    NF_CHECK(resolve_run_config(cfg, vfs, out, err));

    NF_CHECK_EQ(out.project_name, std::string("Demo"));
    // The project's own mount wins over the default Content/.
    auto resolved = vfs.resolve("content://x.txt");
    NF_CHECK(resolved.ok);
    NF_CHECK(resolved.value.string().find("Assets") != std::string::npos);

    std::filesystem::remove_all(root);
}

NF_TEST(run_config_project_supplies_title_window_and_startup_scene) {
    const auto root = cfg_temp_dir("nf_run_cfg_present");
    write_file(root / "Demo.nfproj",
               "version: 1\nname: Demo\ntitle: Demo Game\n"
               "window_width: 1600\nwindow_height: 900\n"
               "startup_scene: content://Scenes/Boot.nfscene\n");

    ApplicationConfig cfg;
    cfg.project_path = (root / "Demo.nfproj").string();

    VirtualFileSystem vfs;
    ResolvedRunConfig out;
    std::string err;
    NF_CHECK(resolve_run_config(cfg, vfs, out, err));

    NF_CHECK_EQ(out.title, std::string("Demo Game"));
    NF_CHECK_EQ(out.width, 1600u);
    NF_CHECK_EQ(out.height, 900u);
    NF_CHECK_EQ(out.scene_path, std::string("content://Scenes/Boot.nfscene"));

    std::filesystem::remove_all(root);
}

NF_TEST(run_config_explicit_scene_wins_over_the_project) {
    const auto root = cfg_temp_dir("nf_run_cfg_explicit");
    write_file(root / "Demo.nfproj",
               "version: 1\nname: Demo\nstartup_scene: content://Scenes/Boot.nfscene\n");

    ApplicationConfig cfg;
    cfg.project_path = (root / "Demo.nfproj").string();
    cfg.scene_path = "content://Scenes/Override.nfscene";

    VirtualFileSystem vfs;
    ResolvedRunConfig out;
    std::string err;
    NF_CHECK(resolve_run_config(cfg, vfs, out, err));

    // --scene must keep working against a project, otherwise a project would be
    // impossible to inspect or test one scene at a time.
    NF_CHECK_EQ(out.scene_path, std::string("content://Scenes/Override.nfscene"));

    std::filesystem::remove_all(root);
}

NF_TEST(run_config_reports_a_missing_project_file) {
    ApplicationConfig cfg;
    cfg.project_path = (std::filesystem::temp_directory_path() / "nf_nope.nfproj").string();

    VirtualFileSystem vfs;
    ResolvedRunConfig out;
    std::string err;
    NF_CHECK(!resolve_run_config(cfg, vfs, out, err));
    // The error must name the file: "project load failed" alone gives a caller
    // nothing to act on.
    NF_CHECK(err.find("nf_nope.nfproj") != std::string::npos);

    // A failed load must not leave a half-mounted VFS behind.
    NF_CHECK(!vfs.resolve("content://x.txt").ok);
}

NF_TEST(run_config_rejects_a_malformed_project) {
    const auto root = cfg_temp_dir("nf_run_cfg_malformed");
    write_file(root / "Bad.nfproj", "name: Bad\n"); // no version

    ApplicationConfig cfg;
    cfg.project_path = (root / "Bad.nfproj").string();

    VirtualFileSystem vfs;
    ResolvedRunConfig out;
    std::string err;
    NF_CHECK(!resolve_run_config(cfg, vfs, out, err));
    NF_CHECK(err.find("version") != std::string::npos);

    std::filesystem::remove_all(root);
}
