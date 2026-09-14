// Tools/AssetCooker/main.cpp — NFAssetCooker CLI
//
// Thin front end. All cooking logic lives in NFProjectTool so it can be unit
// tested; this file only parses arguments and reports.
//
// Modes:
//   --input/--output/--registry   cook a single asset (unchanged, CI uses this)
//   --all                         cook everything under the content mount
//   --verify                      check every registry entry's cooked file
//
// VFS setup: --project <file> uses that project's declared mounts. Without it,
// the tool walks up looking for the engine source tree, which is how it has
// always worked inside this repository.

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Project/ProjectCooker.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

using namespace nf;
using namespace nf::assets;
using namespace nf::project;

namespace {

void print_help() {
    std::cout << "NFAssetCooker — NOVAForge Asset Cooker\n"
              << "Usage:\n"
              << "  NFAssetCooker --input <logical> --output <logical> --registry <logical>\n"
              << "  NFAssetCooker --all --registry <logical> [--content <logical>] [--cache <logical>]\n"
              << "  NFAssetCooker --verify --registry <logical>\n"
              << "  NFAssetCooker --help\n"
              << "\n"
              << "Options:\n"
              << "  --project <file>   Use a .nfproj's declared mounts instead of walking up\n"
              << "                     for the engine source tree.\n"
              << "\n"
              << "Examples:\n"
              << "  NFAssetCooker --input content://Meshes/cube.nfmesh \\\n"
              << "                --output cache://Meshes/cube.nfmesh \\\n"
              << "                --registry content://AssetRegistry.nfreg\n"
              << "  NFAssetCooker --all --registry content://AssetRegistry.nfreg\n"
              << "  NFAssetCooker --verify --registry content://AssetRegistry.nfreg\n"
              << "\n"
              << "Cookable: .nfmesh .nfmat .nfscene .png .jpg .jpeg .bmp .tga\n"
              << "Not cooked: GLSL (.frag/.vert) — the build compiles those to SPIR-V,\n"
              << "and packaging ships the result.\n"
              << "\n"
              << "VFS mounts (auto-detected without --project):\n"
              << "  engine://  -> Engine/\n"
              << "  project:// -> <project root>\n"
              << "  content:// -> Content/ (or project/Content)\n"
              << "  cache://   -> Cache/ or build/cache\n"
              << "\n";
}

// Walk up for the engine source tree. Kept verbatim so an existing in-repo
// invocation behaves exactly as before.
bool setup_vfs_engine_tree(VirtualFileSystem& vfs, std::string& out_error) {
    std::filesystem::path project_root = std::filesystem::current_path();
    for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(project_root / "CMakeLists.txt") &&
            std::filesystem::exists(project_root / "Engine")) {
            break;
        }
        auto parent = project_root.parent_path();
        if (parent == project_root) break;
        project_root = parent;
    }

    std::filesystem::path content_path = project_root / "Content";
    if (!std::filesystem::exists(content_path)) {
        content_path = std::filesystem::current_path() / "Content";
    }
    std::filesystem::path cache_path = project_root / "Cache";
    if (!std::filesystem::exists(cache_path)) {
        cache_path = project_root / "build" / "cache";
    }

    auto mount_or_create = [&](const char* logical, const std::filesystem::path& physical) {
        std::error_code ec;
        std::filesystem::create_directories(physical, ec);
        auto r = vfs.mount(logical, physical);
        if (!r.ok) {
            out_error = std::string("failed to mount ") + logical + ": " + r.error;
            return false;
        }
        return true;
    };

    if (!mount_or_create("engine://", project_root / "Engine")) return false;
    if (!mount_or_create("project://", project_root)) return false;
    if (!mount_or_create("content://", content_path)) return false;
    if (!mount_or_create("cache://", cache_path)) return false;
    return true;
}

bool setup_vfs(VirtualFileSystem& vfs, const std::string& project_path, std::string& out_error) {
    if (project_path.empty()) {
        return setup_vfs_engine_tree(vfs, out_error);
    }
    auto desc = ProjectDescriptor::load_from_file(project_path, out_error);
    if (!desc) return false;
    if (!desc->apply_mounts(vfs, out_error)) return false;
    std::cout << "Project: " << desc->name() << " (" << desc->mounts().size() << " mounts)\n";
    return true;
}

bool load_registry(VirtualFileSystem& vfs, AssetRegistry& reg, const std::string& registry,
                   std::string& out_error) {
    auto exists = vfs.exists(registry);
    if (exists.ok && exists.value) {
        return reg.load(vfs, registry, out_error);
    }
    reg.set_version(AssetRegistry::kCurrentVersion);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Logger::instance().add_sink(Logger::make_console_sink());
    Logger::instance().set_min_level(LogLevel::Info);

    if (argc == 1) { print_help(); return 0; }

    std::string input, output, registry, project_path;
    std::string content_mount = "content://";
    std::string cache_mount = "cache://";
    bool verify = false;
    bool all = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 < argc) dst = argv[++i];
        };
        auto value_of = [&](std::string_view prefix) {
            return std::string(arg.substr(prefix.size()));
        };

        if (arg == "--input") next(input);
        else if (arg.rfind("--input=", 0) == 0) input = value_of("--input=");
        else if (arg == "--output") next(output);
        else if (arg.rfind("--output=", 0) == 0) output = value_of("--output=");
        else if (arg == "--registry") next(registry);
        else if (arg.rfind("--registry=", 0) == 0) registry = value_of("--registry=");
        else if (arg == "--project") next(project_path);
        else if (arg.rfind("--project=", 0) == 0) project_path = value_of("--project=");
        else if (arg == "--content") next(content_mount);
        else if (arg == "--cache") next(cache_mount);
        else if (arg == "--verify") verify = true;
        else if (arg == "--all") all = true;
        else if (arg == "--help" || arg == "-h") { print_help(); return 0; }
        else if (arg == "--version") { std::cout << "NFAssetCooker v0.2\n"; return 0; }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_help();
            return 1;
        }
    }

    VirtualFileSystem vfs;
    std::string vfs_err;
    if (!setup_vfs(vfs, project_path, vfs_err)) {
        std::cerr << "VFS setup failed: " << vfs_err << "\n";
        return 1;
    }

    if (registry.empty()) {
        std::cerr << "--registry is required\n";
        return 1;
    }

    AssetRegistry reg;
    std::string reg_err;
    if (!load_registry(vfs, reg, registry, reg_err)) {
        std::cerr << "Failed to load registry '" << registry << "': " << reg_err << "\n";
        return 1;
    }

    // --- verify -------------------------------------------------------------
    if (verify) {
        VerifyReport report;
        std::string err;
        verify_registry(vfs, reg, report, err);
        if (!err.empty()) std::cerr << err << "\n";
        std::cout << "Verify: total " << report.total << ", ok " << report.ok_count
                  << ", missing " << report.missing << ", bad_format " << report.bad_format << "\n";
        return report.ok() ? 0 : 1;
    }

    // --- cook all -----------------------------------------------------------
    if (all) {
        CookReport report;
        std::string err;
        const bool enumerated = cook_all(vfs, reg, content_mount, cache_mount, report, err);
        if (!err.empty()) std::cerr << err << "\n";
        if (report.cooked > 0 || report.pruned > 0) {
            std::string save_err;
            if (!reg.save(vfs, registry, save_err)) {
                std::cerr << "Failed to save registry '" << registry << "': " << save_err << "\n";
                return 1;
            }
        }
        std::cout << "Cook report: cooked " << report.cooked << ", skipped " << report.skipped
                  << ", failed " << report.failed << ", total " << report.total() << "\n";
        return (enumerated && report.ok()) ? 0 : 1;
    }

    // --- cook one -----------------------------------------------------------
    if (input.empty() || output.empty()) {
        std::cerr << "--input and --output are required (or use --all)\n";
        print_help();
        return 1;
    }

    std::string cook_err;
    bool skipped = false;
    const bool ok = cook_one(vfs, reg, input, output, cook_err, skipped);
    if (!ok) {
        std::cerr << "Cook failed: " << cook_err << "\n";
        std::cout << "Cook report: cooked 0, skipped 0, failed 1, total 1\n";
        return 1;
    }
    if (skipped) {
        std::cout << "Skipped (unchanged): " << input << " -> " << output << "\n";
    } else {
        std::string save_err;
        if (!reg.save(vfs, registry, save_err)) {
            std::cerr << "Failed to save registry '" << registry << "': " << save_err << "\n";
            return 1;
        }
    }
    std::cout << "Cook report: cooked " << (skipped ? 0 : 1) << ", skipped " << (skipped ? 1 : 0)
              << ", failed 0, total 1\n";
    return 0;
}
