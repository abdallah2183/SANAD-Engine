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
    std::string scene_path = "content://Scenes/Example.nfscene"; // logical path
    uint32_t max_frames = 0; // 0 = run until close
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
