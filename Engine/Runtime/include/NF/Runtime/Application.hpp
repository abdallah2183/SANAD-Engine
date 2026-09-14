#pragma once

#include <NF/Core/Types.hpp>

#include <string>

namespace nf::runtime {

struct ApplicationConfig {
    std::string title = "NOVAForge Runtime";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    bool validation = false;
    bool headless = false;
    // Empty means "not specified": a project's startup_scene is used when one is
    // given, otherwise content://Scenes/Example.nfscene. Empty rather than a
    // default path so a caller can distinguish "I did not choose" from "I chose
    // the same path the default would have picked".
    std::string scene_path;
    uint32_t max_frames = 0; // 0 = run until close
    // Path to a .nfproj. When set, the project supplies the VFS mounts, window
    // title and size, and startup scene. When empty the runtime falls back to
    // walking up the filesystem for a directory containing both Engine/ and
    // Content/ — the engine source tree layout, which is what every in-repo
    // sample and test relies on. A shipped game has neither directory, so it
    // must pass a project.
    std::string project_path;
};

class Application {
public:
    explicit Application(const ApplicationConfig& config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Returns exit code (0 on success)
    int run();

private:
    ApplicationConfig m_config;
};

} // namespace nf::runtime
