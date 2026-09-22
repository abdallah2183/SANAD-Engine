// Tools/Player/main.cpp — NFPlayer, the standalone game runtime.
//
// This is the "Run outside editor" step of the design document's phase-1
// success chain. It reads a project descriptor and runs it — no engine source
// tree, no editor, no build directories. A package produced by `nf build` is
// self-contained: the descriptor's mounts are relative to the descriptor, so the
// player resolves Content/, Cache/ and Shaders/ from wherever the package sits.

#include <NF/Runtime/Application.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Core/CrashHandler.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// Stamped into every crash report the player writes, so a dump that arrives
// from a player can be tied back to the exact build that produced it.
constexpr const char* kPlayerVersion = "NFPlayer 0.1 (NOVAForge Phase 7)";

void print_help() {
    std::cout << "NFPlayer — NOVAForge standalone runtime\n"
              << "Usage:\n"
              << "  NFPlayer --project <file.nfproj> [options]\n"
              << "Options:\n"
              << "  --scene <logical>   Override the project's startup scene\n"
              << "  --frames N          Run N frames then exit (0 = until the window closes)\n"
              << "  --validation        Enable Vulkan validation layers\n"
              << "  --headless          No window (offscreen only)\n"
              << "  --help\n"
              << "\n"
              << "Without --project, a single .nfproj beside the executable is used.\n"
              << "\n"
              << "Crash dumps and human-readable reports are written to Crashes/ next\n"
              << "to this executable — inside the game folder, so uninstalling the\n"
              << "game removes them too.\n"
              << "\n"
              << "Test hook: --crash-test [dir] arms the crash handler, raises a fatal\n"
              << "exception, and exits — used to verify the dump + report path. With\n"
              << "no dir, it writes to Crashes/ beside the executable.\n";
}

// Look for exactly one .nfproj next to the executable. A package contains one,
// so a shipped game can be launched with no arguments at all.
std::string find_project_beside(const char* argv0) {
    std::error_code ec;
    auto exe = std::filesystem::absolute(argv0, ec);
    if (ec || exe.empty()) return {};
    const auto dir = exe.parent_path();

    std::vector<std::filesystem::path> found;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec &&
            it->path().extension() == nf::assets::ProjectDescriptor::kExtension) {
            found.push_back(it->path());
        }
    }
    return found.size() == 1 ? found.front().string() : std::string{};
}

} // namespace

int main(int argc, char** argv) {
    nf::runtime::ApplicationConfig config;

    std::string project_path;
    // Test-only escape hatch: arms the real crash handler, raises a real fatal
    // exception, and lets the SEH filter do its job. This is how the shipping
    // pipeline proves the minidump + crash-report path end to end — the same
    // binary a customer runs, not a special test harness.
    // `--crash-test` with no directory uses the default Crashes/ beside the
    // executable, which is what proves a shipped package keeps its artifacts
    // inside its own folder (see UNINSTALL.txt).
    bool crash_test = false;
    std::string crash_test_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };

        if (arg == "--project") project_path = next();
        else if (arg.rfind("--project=", 0) == 0) project_path = arg.substr(10);
        else if (arg == "--scene") config.scene_path = next();
        else if (arg.rfind("--scene=", 0) == 0) config.scene_path = arg.substr(8);
        else if (arg == "--frames") config.max_frames = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (arg.rfind("--frames=", 0) == 0) config.max_frames = static_cast<uint32_t>(std::atoi(arg.substr(9).c_str()));
        else if (arg == "--validation") config.validation = true;
        else if (arg == "--headless") config.headless = true;
        else if (arg == "--crash-test") crash_test = true;
        else if (arg.rfind("--crash-test=", 0) == 0) {
            crash_test = true;
            crash_test_dir = arg.substr(13);
        }
        else if (arg == "--help" || arg == "-h") { print_help(); return 0; }
        else {
            std::cerr << "NFPlayer: unknown argument '" << arg << "'\n";
            print_help();
            return 1;
        }
    }

    // The crash handler is armed before anything else so even a failure during
    // startup leaves a readable artifact behind. Dumps land beside the
    // executable (never %APPDATA%, never the registry): the package folder is
    // the whole install, and deleting it is the whole uninstall.
    std::error_code exe_ec;
    const auto exe_dir = std::filesystem::absolute(argv[0], exe_ec).parent_path();
    {
        nf::CrashHandlerConfig crash_cfg;
        crash_cfg.dump_directory = (exe_dir / "Crashes").string();
        crash_cfg.dump_prefix = "NFPlayer";
        crash_cfg.app_version = kPlayerVersion;
        nf::install_crash_handler(crash_cfg);
    }

    if (crash_test) {
#ifdef _WIN32
        // Re-arm with the requested directory, then raise an unhandled
        // exception. The filter writes the minidump + .txt report and the
        // process terminates — nothing after this line runs.
        nf::CrashHandlerConfig cfg;
        cfg.dump_directory =
            crash_test_dir.empty() ? (exe_dir / "Crashes").string() : crash_test_dir;
        cfg.dump_prefix = "NFPlayer";
        cfg.app_version = kPlayerVersion;
        nf::install_crash_handler(cfg);
        RaiseException(0xE0000046, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        std::cerr << "NFPlayer: --crash-test did not crash (filter not armed?)\n";
        return 1;
#else
        (void)crash_test_dir;
        std::cerr << "NFPlayer: --crash-test is Windows-only\n";
        return 1;
#endif
    }

    // Environment overrides, so CI can drive the player the same way it drives
    // the samples.
#ifdef _MSC_VER
    char* buf = nullptr; size_t sz = 0;
    if (_dupenv_s(&buf, &sz, "NF_PLAYER_PROJECT") == 0 && buf) {
        if (project_path.empty()) project_path = buf;
        std::free(buf);
    }
    char* b2 = nullptr; size_t s2 = 0;
    if (_dupenv_s(&b2, &s2, "NF_PLAYER_FRAMES") == 0 && b2) {
        config.max_frames = static_cast<uint32_t>(std::atoi(b2));
        std::free(b2);
    }
    char* b3 = nullptr; size_t s3 = 0;
    if (_dupenv_s(&b3, &s3, "NF_PLAYER_VALIDATION") == 0 && b3) {
        config.validation = true;
        std::free(b3);
    }
    char* b4 = nullptr; size_t s4 = 0;
    if (_dupenv_s(&b4, &s4, "NF_PLAYER_HEADLESS") == 0 && b4) {
        config.headless = true;
        std::free(b4);
    }
#else
    if (auto e = std::getenv("NF_PLAYER_PROJECT"); e && project_path.empty()) project_path = e;
    if (auto e = std::getenv("NF_PLAYER_FRAMES")) config.max_frames = static_cast<uint32_t>(std::atoi(e));
    if (std::getenv("NF_PLAYER_VALIDATION")) config.validation = true;
    if (std::getenv("NF_PLAYER_HEADLESS")) config.headless = true;
#endif

    if (project_path.empty()) {
        project_path = find_project_beside(argv[0]);
        if (project_path.empty()) {
            std::cerr << "NFPlayer: no --project given and no single .nfproj beside the executable\n";
            print_help();
            return 1;
        }
    }

    // Fail before touching the GPU: a missing or malformed project is a much
    // clearer error here than a blank window.
    {
        std::string err;
        auto desc = nf::assets::ProjectDescriptor::load_from_file(project_path, err);
        if (!desc) {
            std::cerr << "NFPlayer: " << err << "\n";
            return 1;
        }
        std::cout << "NFPlayer: project '" << desc->name() << "' (" << project_path << ")\n";
    }

    config.project_path = project_path;
    nf::runtime::Application app(config);
    return app.run();
}
