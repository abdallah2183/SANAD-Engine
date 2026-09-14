// Tools/BuildTool/main.cpp — the `nf` command line.
//
// Thin front end over NFProjectTool. Every command delegates; this file parses
// arguments, locates the project, and reports. The end-to-end chain the design
// document asks for (§262) is:
//
//   nf new Demo        create a project
//   nf build           cook it and package it
//   <player>           run it outside the editor
//
// Shader and template locations are compile definitions supplied by CMake, the
// same mechanism NFRuntime uses for NF_BASIC3D_SHADER_DIR.

#include <NF/Project/ProjectCooker.hpp>
#include <NF/Project/ProjectPackager.hpp>
#include <NF/Project/ProjectScaffold.hpp>

#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Logger.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef NF_TEMPLATE_DIR
    #define NF_TEMPLATE_DIR ""
#endif
#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

using namespace nf;
using namespace nf::project;

namespace {

constexpr const char* kVersion = "nf 0.1 (NOVAForge Phase 7)";

void print_help() {
    std::cout << "nf — NOVAForge project tool\n"
              << "Usage:\n"
              << "  nf new <dir> [--name <Name>]     create a project\n"
              << "  nf cook  [--project <file>]      cook every asset\n"
              << "  nf build [--project <file>] [--out <dir>]\n"
              << "                                   cook and package into dist/\n"
              << "  nf run   [--project <file>] [--frames N] [--headless] [--validation]\n"
              << "                                   build, then launch the player\n"
              << "  nf verify [--project <file>]     check every cooked asset is present\n"
              << "  nf --help | --version\n"
              << "\n"
              << "Without --project, the current directory (and its parents) is searched\n"
              << "for a single .nfproj.\n";
}

// The directory the running executable lives in. The player is built beside
// `nf`, so packaging can find it without a path argument.
std::filesystem::path executable_dir(const char* argv0) {
    std::error_code ec;
    auto p = std::filesystem::absolute(argv0, ec);
    if (ec || p.empty()) return std::filesystem::current_path();
    return p.parent_path();
}

// Search the current directory and up to four parents for a .nfproj. Returns an
// empty path and fills out_error when there is none or more than one.
std::filesystem::path find_project_file(std::string& out_error) {
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    for (int i = 0; i < 5; ++i) {
        std::vector<std::filesystem::path> found;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && !ec &&
                it->path().extension() == assets::ProjectDescriptor::kExtension) {
                found.push_back(it->path());
            }
        }
        if (found.size() == 1) return found.front();
        if (found.size() > 1) {
            out_error = "more than one .nfproj in '" + dir.string() + "'; pass --project";
            return {};
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    out_error = "no .nfproj found in the current directory or its parents; pass --project";
    return {};
}

std::filesystem::path resolve_project(const std::string& explicit_path, std::string& out_error) {
    if (!explicit_path.empty()) {
        if (!std::filesystem::exists(explicit_path)) {
            out_error = "project file does not exist: '" + explicit_path + "'";
            return {};
        }
        return explicit_path;
    }
    return find_project_file(out_error);
}

// Mount a project so the cooker and verifier can use logical paths.
bool open_project(const std::filesystem::path& project_file,
                  assets::VirtualFileSystem& vfs,
                  assets::AssetRegistry& registry,
                  std::string& out_error) {
    auto desc = assets::ProjectDescriptor::load_from_file(project_file, out_error);
    if (!desc) return false;
    if (!desc->apply_mounts(vfs, out_error)) return false;

    auto exists = vfs.exists("content://AssetRegistry.nfreg");
    if (exists.ok && exists.value) {
        return registry.load(vfs, "content://AssetRegistry.nfreg", out_error);
    }
    registry.set_version(assets::AssetRegistry::kCurrentVersion);
    return true;
}

int cmd_new(const std::string& dir, const std::string& name_in) {
    const std::filesystem::path root = dir.empty() ? std::filesystem::current_path()
                                                   : std::filesystem::path(dir);
    std::string name = name_in;
    if (name.empty()) name = root.filename().string();
    if (name.empty()) {
        std::cerr << "nf new: cannot infer a name; pass --name\n";
        return 1;
    }

    ScaffoldOptions opts;
    opts.root = root;
    opts.name = name;
    // An empty NF_TEMPLATE_DIR means the engine was configured without the
    // templates directory; a bare project is still valid, just empty.
    opts.template_dir = std::string(NF_TEMPLATE_DIR);

    std::string err;
    if (!scaffold_project(opts, err)) {
        std::cerr << "nf new: " << err << "\n";
        return 1;
    }
    std::cout << "Created project '" << name << "' at " << root.string() << "\n";
    std::cout << "  " << name << assets::ProjectDescriptor::kExtension << "\n";
    if (!opts.template_dir.empty()) {
        std::cout << "  Content/  (from template)\n";
    }
    std::cout << "\nNext: nf build --project " << (root / (name + ".nfproj")).string() << "\n";
    return 0;
}

int cmd_cook(const std::filesystem::path& project_file) {
    assets::VirtualFileSystem vfs;
    assets::AssetRegistry registry;
    std::string err;
    if (!open_project(project_file, vfs, registry, err)) {
        std::cerr << "nf cook: " << err << "\n";
        return 1;
    }

    CookReport report;
    const bool enumerated = cook_all(vfs, registry, "content://", "cache://", report, err);
    if (!err.empty()) std::cerr << err << "\n";
    if (report.cooked > 0 || report.pruned > 0) {
        std::string save_err;
        if (!registry.save(vfs, "content://AssetRegistry.nfreg", save_err)) {
            std::cerr << "nf cook: failed to save registry: " << save_err << "\n";
            return 1;
        }
    }
    std::cout << "Cook report: cooked " << report.cooked << ", skipped " << report.skipped
              << ", failed " << report.failed << ", pruned " << report.pruned
              << ", total " << report.total() << "\n";
    return (enumerated && report.ok()) ? 0 : 1;
}

int cmd_verify(const std::filesystem::path& project_file) {
    assets::VirtualFileSystem vfs;
    assets::AssetRegistry registry;
    std::string err;
    if (!open_project(project_file, vfs, registry, err)) {
        std::cerr << "nf verify: " << err << "\n";
        return 1;
    }
    VerifyReport report;
    verify_registry(vfs, registry, report, err);
    if (!err.empty()) std::cerr << err << "\n";
    std::cout << "Verify: total " << report.total << ", ok " << report.ok_count
              << ", missing " << report.missing << ", bad_format " << report.bad_format << "\n";
    return report.ok() ? 0 : 1;
}

BuildOptions make_build_options(const std::filesystem::path& project_file,
                                const std::string& out_dir,
                                const char* argv0) {
    BuildOptions opts;
    opts.project_file = project_file;
    if (!out_dir.empty()) opts.output_dir = out_dir;
    const auto exe_dir = executable_dir(argv0);
    opts.player_exe = exe_dir / "NFPlayer.exe";
    if (!std::filesystem::exists(opts.player_exe)) {
        // Non-Windows or a non-standard layout: try the bare name.
        opts.player_exe = exe_dir / "NFPlayer";
    }
    opts.shader_dir = std::string(NF_BASIC3D_SHADER_DIR);
    return opts;
}

int cmd_build(const std::filesystem::path& project_file,
              const std::string& out_dir,
              const char* argv0) {
    BuildReport report;
    std::string err;
    if (!build_project(make_build_options(project_file, out_dir, argv0), report, err)) {
        std::cerr << "nf build: " << err << "\n";
        return 1;
    }
    std::cout << "Cook report: cooked " << report.cook.cooked << ", skipped " << report.cook.skipped
              << ", failed " << report.cook.failed << ", pruned " << report.cook.pruned
              << ", total " << report.cook.total() << "\n";
    std::cout << "Packaged " << report.files_packaged << " file(s) (" << report.manifest_entries
              << " manifest entries), " << report.shaders_copied << " shader(s)\n";
    std::cout << "Output: " << report.output_dir.string() << "\n";
    std::cout << "Run:    " << report.output_dir.string() << "/NFPlayer --project "
              << report.packaged_project.string() << "\n";
    return 0;
}

int cmd_run(const std::filesystem::path& project_file,
            const std::string& out_dir,
            const std::vector<std::string>& passthrough,
            const char* argv0) {
    // Build first: running a stale package is the most confusing possible
    // failure, because the game looks wrong rather than broken.
    if (const int rc = cmd_build(project_file, out_dir, argv0); rc != 0) {
        return rc;
    }

    BuildReport report;
    std::string err;
    const auto opts = make_build_options(project_file, out_dir, argv0);
    // Re-derive the output path without rebuilding: cheap and avoids duplicating
    // the default-directory rule.
    auto desc = assets::ProjectDescriptor::load_from_file(project_file, err);
    if (!desc) {
        std::cerr << "nf run: " << err << "\n";
        return 1;
    }
    const auto resolved_out = opts.output_dir.empty() ? (desc->root() / "dist") : opts.output_dir;
    const auto player = resolved_out / opts.player_exe.filename();
    const auto packaged = resolved_out / project_file.filename();

    if (!std::filesystem::exists(player)) {
        std::cerr << "nf run: player not found at '" << player.string()
                  << "' (build the engine with NF_BUILD_TOOLS=ON)\n";
        return 1;
    }

    std::string cmd = "\"" + player.string() + "\" --project \"" + packaged.string() + "\"";
    for (const auto& a : passthrough) {
        cmd += " \"";
        cmd += a;
        cmd += "\"";
    }
    std::cout << "Launching: " << cmd << "\n";
    return std::system(cmd.c_str());
}

} // namespace

int main(int argc, char** argv) {
    Logger::instance().add_sink(Logger::make_console_sink());
    Logger::instance().set_min_level(LogLevel::Warn);

    if (argc < 2) {
        print_help();
        return 0;
    }

    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        print_help();
        return 0;
    }
    if (command == "--version" || command == "-v") {
        std::cout << kVersion << "\n";
        return 0;
    }

    std::string project_arg, out_dir, name;
    std::vector<std::string> passthrough;
    std::string positional;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--project") project_arg = next();
        else if (arg.rfind("--project=", 0) == 0) project_arg = arg.substr(10);
        else if (arg == "--out") out_dir = next();
        else if (arg.rfind("--out=", 0) == 0) out_dir = arg.substr(6);
        else if (arg == "--name") name = next();
        else if (arg.rfind("--name=", 0) == 0) name = arg.substr(7);
        else if (arg == "--frames" || arg == "--headless" || arg == "--validation") {
            // Forwarded verbatim to the player.
            passthrough.push_back(arg);
            if (arg == "--frames") {
                const auto v = next();
                if (!v.empty()) passthrough.push_back(v);
            }
        } else if (!arg.empty() && arg[0] != '-') {
            positional = arg;
        } else {
            std::cerr << "nf: unknown option '" << arg << "'\n";
            return 1;
        }
    }

    if (command == "new") {
        return cmd_new(positional, name);
    }

    std::string proj_err;
    const auto project_file = resolve_project(project_arg, proj_err);
    if (project_file.empty()) {
        std::cerr << "nf " << command << ": " << proj_err << "\n";
        return 1;
    }

    if (command == "cook") return cmd_cook(project_file);
    if (command == "verify") return cmd_verify(project_file);
    if (command == "build") return cmd_build(project_file, out_dir, argv[0]);
    if (command == "run") return cmd_run(project_file, out_dir, passthrough, argv[0]);

    std::cerr << "nf: unknown command '" << command << "'\n";
    print_help();
    return 1;
}
