// Tests/ToolTests/test_build_tool.cpp — the `nf` developer's two launch paths.
//
// The selector cases are deliberately below the CLI: they prove the allow-list
// itself, including its failure text, without replacing an integration case
// that runs the real `nf` executable. The process cases then pin the direct
// Windows command line and the build-then-play workflow that exposed cmd.exe's
// outer-quote parsing failure.

#include <NF/Test/TestFramework.hpp>

#include <NF/Project/ProjectPackager.hpp>
#include <NF/Project/ProjectScaffold.hpp>
#include <NF/Project/WindowsProcess.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace nf;
using namespace nf::project;

#ifndef NF_TEMPLATE_DIR
#define NF_TEMPLATE_DIR ""
#endif
#ifndef NF_TEMPLATE_THIRDPERSON_DIR
#define NF_TEMPLATE_THIRDPERSON_DIR ""
#endif
#ifndef NF_TEMPLATE_FPSSTARTER_DIR
#define NF_TEMPLATE_FPSSTARTER_DIR ""
#endif
#ifndef NF_TEMPLATE_PLATFORMER2D_DIR
#define NF_TEMPLATE_PLATFORMER2D_DIR ""
#endif
#ifndef NF_BASIC3D_SHADER_DIR
#define NF_BASIC3D_SHADER_DIR ""
#endif
#ifndef NF_PLAYER_EXE
#define NF_PLAYER_EXE ""
#endif
#ifndef NF_CLI_EXE
#define NF_CLI_EXE ""
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TempDir {
public:
    explicit TempDir(const std::string& name) : root_(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
        require(!ec, "failed to create test directory '" + root_.string() + "': " + ec.message());
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return root_; }

private:
    std::filesystem::path root_;
};

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "failed to read '" + path.string() + "'");
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
}

struct TemplateSpec {
    std::string_view name;
    const char* directory;
};

const std::array<TemplateSpec, 4> kTemplateSpecs{{
    {"Default", NF_TEMPLATE_DIR},
    {"ThirdPerson", NF_TEMPLATE_THIRDPERSON_DIR},
    {"FPSStarter", NF_TEMPLATE_FPSSTARTER_DIR},
    {"Platformer2D", NF_TEMPLATE_PLATFORMER2D_DIR},
}};

std::filesystem::path templates_root() {
    return std::filesystem::path(NF_TEMPLATE_DIR).parent_path();
}

bool contains(const std::string& text, std::string_view value) {
    return text.find(value) != std::string::npos;
}

} // namespace

NF_TEST(project_template_selector_copies_the_selected_manifest_and_marker) {
    for (const TemplateSpec& spec : kTemplateSpecs) {
        if (std::string(spec.directory).empty()) {
            NF_SKIP((std::string(spec.name) + " template directory is not configured").c_str());
        }

        TempDir scratch("nf_template_select_" + std::string(spec.name));
        std::string error;
        const auto selected = resolve_project_template(
            spec.name, templates_root(), std::filesystem::path(NF_TEMPLATE_DIR), error);
        require(!selected.empty(),
                std::string(spec.name) + ": template selection failed: " + error);
        require(selected == std::filesystem::path(spec.directory),
                std::string(spec.name) + ": selection resolved to the wrong directory");

        ScaffoldOptions options;
        options.root = scratch.path();
        options.name = "Selected";
        options.template_dir = selected;
        require(scaffold_project(options, error),
                std::string(spec.name) + ": scaffold failed: " + error);

        // Main.nfscene is the genre-specific marker; the registry manifest must
        // travel with it. Byte equality proves this source was copied rather
        // than reconstructed or replaced by Default.
        const auto source_root = std::filesystem::path(spec.directory) / "Content";
        const auto copied_root = scratch.path() / "Content";
        require(read_bytes(source_root / "Scenes" / "Main.nfscene") ==
                    read_bytes(copied_root / "Scenes" / "Main.nfscene"),
                std::string(spec.name) + ": scene marker was not copied byte-for-byte");
        require(read_bytes(source_root / "AssetRegistry.nfreg") ==
                    read_bytes(copied_root / "AssetRegistry.nfreg"),
                std::string(spec.name) + ": asset manifest was not copied byte-for-byte");
    }
}

NF_TEST(project_template_selector_preserves_default_and_rejects_unknown_names) {
    std::string error;
    const std::filesystem::path configured_default =
        std::filesystem::path("configured-default");
    const auto no_flag = resolve_project_template(
        "", templates_root(), configured_default, error);
    require(no_flag == configured_default, "no-flag template selection changed the default path");
    require(error.empty(), "no-flag template selection reported an error");

    constexpr std::array<std::string_view, 5> kRejectedNames{
        "Unknown", "thirdperson", "../Templates/ThirdPerson",
        "Templates/ThirdPerson", "ThirdPerson\\..\\Default"};

    for (const std::string_view name : kRejectedNames) {
        error.clear();
        const auto selected = resolve_project_template(
            name, templates_root(), configured_default, error);
        require(selected.empty(), "unknown/traversal template name unexpectedly resolved");
        for (const TemplateSpec& spec : kTemplateSpecs) {
            const std::string valid_name(spec.name);
            require(contains(error, valid_name),
                    "template rejection did not list valid name '" + valid_name + "'");
        }
    }
}

NF_TEST(project_packager_rejects_output_overlap_without_deleting_sources) {
    TempDir scratch("nf_packager_overlap");
    ScaffoldOptions scaffold;
    scaffold.root = scratch.path();
    scaffold.name = "Overlap";
    scaffold.template_dir = NF_TEMPLATE_DIR;
    std::string error;
    require(scaffold_project(scaffold, error), "overlap fixture scaffold failed: " + error);

    const auto sentinel = scratch.path() / "Content" / "sentinel.txt";
    {
        std::ofstream output(sentinel, std::ios::binary);
        require(static_cast<bool>(output), "failed to create overlap sentinel");
        output << "must survive rejected packaging\n";
    }

    BuildOptions options;
    options.project_file = scratch.path() / "Overlap.nfproj";
    options.output_dir = scratch.path(); // the dangerous --out . case
    BuildReport report;
    require(!build_project(options, report, error),
            "packager accepted an output directory equal to the project root");
    require(contains(error, "overlaps project root"),
            "root-overlap error was not explicit: " + error);
    require(std::filesystem::exists(sentinel),
            "rejected root-overlap build deleted a source file");

    options.output_dir = scratch.path() / "Content" / "dist";
    error.clear();
    require(!build_project(options, report, error),
            "packager accepted an output directory inside the content mount");
    require(contains(error, "overlaps project mount 'content://'"),
            "content-overlap error was not explicit: " + error);
    require(std::filesystem::exists(sentinel),
            "rejected content-overlap build deleted a source file");
}

NF_TEST(nf_new_cli_forwards_template_selection_and_never_falls_back) {
#if !defined(_WIN32)
    NF_SKIP("nf is a Windows-only workflow");
#else
    if (std::string(NF_CLI_EXE).empty() || std::string(NF_TEMPLATE_THIRDPERSON_DIR).empty()) {
        NF_SKIP("nf or template directories not built (NF_BUILD_TOOLS=OFF)");
    }

    TempDir selected_root("nf_cli_template_selected");
    std::string error;
    const int selected_rc = run_windows_process_and_wait(
        NF_CLI_EXE, selected_root.path(),
        {"new", selected_root.path().string(), "--name", "CliGame", "--template", "ThirdPerson"},
        error);
    require(error.empty(), "nf new ThirdPerson launch failed: " + error);
    require(selected_rc == 0, "nf new --template ThirdPerson exited " +
                                  std::to_string(selected_rc));
    require(read_bytes(std::filesystem::path(NF_TEMPLATE_THIRDPERSON_DIR) /
                       "Content" / "Scenes" / "Main.nfscene") ==
                read_bytes(selected_root.path() / "Content" / "Scenes" / "Main.nfscene"),
            "nf new did not copy the ThirdPerson scene marker");

    TempDir rejected_root("nf_cli_template_rejected");
    error.clear();
    const int rejected_rc = run_windows_process_and_wait(
        NF_CLI_EXE, rejected_root.path(),
        {"new", rejected_root.path().string(), "--name", "NoFallback", "--template",
         "Templates/ThirdPerson"},
        error);
    require(error.empty(), "unknown-template CLI launch failed: " + error);
    require(rejected_rc != 0, "nf new accepted a template traversal path");
    require(!std::filesystem::exists(rejected_root.path() / "NoFallback.nfproj"),
            "nf new silently fell back to Default after an unknown template");
#endif
}

NF_TEST(windows_process_command_line_preserves_player_paths_and_flags) {
    const std::vector<std::wstring> arguments{
        L"--project", L"C:\\Users\\Game Developer\\My Game.nfproj",
        L"--frames", L"30", L"--headless", L"--validation"};

    const std::wstring expected =
        L"\"--project\" \"C:\\Users\\Game Developer\\My Game.nfproj\" "
        L"\"--frames\" \"30\" \"--headless\" \"--validation\"";
    require(build_windows_command_line(arguments) == expected,
            "Windows player command line was not preserved verbatim");

    // A trailing backslash is the case a naive quote helper corrupts, so pin
    // it alongside the normal path/flag shape above.
    require(quote_windows_argument(L"C:\\quoted name\\") == L"\"C:\\quoted name\\\\\"",
            "trailing backslashes were not protected before the closing quote");
}

NF_TEST(windows_executable_directory_uses_module_path_not_argv0) {
#if !defined(_WIN32)
    NF_SKIP("Windows module paths are only meaningful on Windows");
#else
    if (std::string(NF_CLI_EXE).empty()) {
        NF_SKIP("nf was not built (NF_BUILD_TOOLS=OFF)");
    }
    const auto module_dir = executable_directory();
    const auto cli_dir = std::filesystem::path(NF_CLI_EXE).parent_path();
    std::error_code ec;
    require(std::filesystem::is_directory(module_dir, ec) && !ec,
            "executable_directory did not return a directory: " + module_dir.string());
    require(std::filesystem::equivalent(module_dir, cli_dir, ec) && !ec,
            "executable_directory did not resolve beside the built CLI");
#endif
}

NF_TEST(windows_process_launcher_returns_the_child_exit_status) {
#if !defined(_WIN32)
    NF_SKIP("CreateProcessW is Windows-only");
#else
    if (std::string(NF_PLAYER_EXE).empty()) {
        NF_SKIP("NFPlayer not built (NF_BUILD_TOOLS=OFF)");
    }
    const std::filesystem::path player(NF_PLAYER_EXE);
    require(std::filesystem::is_regular_file(player),
            "NFPlayer test executable does not exist: '" + player.string() + "'");

    TempDir scratch("nf_process_exit_status");
    std::string error;
    // NFPlayer rejects this missing descriptor before initialising Vulkan and
    // returns 1. An empty launcher error distinguishes that real child status
    // from a spawn failure.
    const std::filesystem::path missing_project =
        scratch.path() / "missing" / "Missing.nfproj";
    const int exit_code = run_windows_process_and_wait(
        player, scratch.path(), {"--project", missing_project.string()}, error);
    require(error.empty(), "controlled child launch failed: " + error);
    require(exit_code == 1, "child exit status 1 was not returned (got " +
                                std::to_string(exit_code) + ")");
#endif
}

NF_TEST(nf_run_launches_player_and_forwards_validation_flags) {
#if !defined(_WIN32)
    NF_SKIP("nf is a Windows-only workflow");
#else
    if (std::string(NF_CLI_EXE).empty() || std::string(NF_PLAYER_EXE).empty() ||
        std::string(NF_TEMPLATE_DIR).empty() || std::string(NF_BASIC3D_SHADER_DIR).empty()) {
        NF_SKIP("nf, NFPlayer, template, or shader directory not built");
    }

    TempDir scratch("nf_cli_run");
    ScaffoldOptions options;
    options.root = scratch.path();
    options.name = "RunGame";
    options.template_dir = NF_TEMPLATE_DIR;
    std::string error;
    require(scaffold_project(options, error), "nf run fixture scaffold failed: " + error);

    const int exit_code = run_windows_process_and_wait(
        NF_CLI_EXE, scratch.path(),
        {"run", "--project", (scratch.path() / "RunGame.nfproj").string(),
         "--frames", "2", "--headless", "--validation"},
        error);
    require(error.empty(), "nf run process launch failed: " + error);
    require(exit_code == 0, "nf run did not return the player's successful exit status");
    require(std::filesystem::exists(scratch.path() / "dist" / "manifest.txt"),
            "nf run did not build the package before launching the player");
    require(std::filesystem::exists(scratch.path() / "dist" / "NFPlayer.exe"),
            "nf run package is missing NFPlayer.exe");
#endif
}
