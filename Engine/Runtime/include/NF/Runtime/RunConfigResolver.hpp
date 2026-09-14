#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Types.hpp>

#include <string>

namespace nf::runtime {

struct ApplicationConfig;

// The effective run settings after a project (if any) has been applied.
struct ResolvedRunConfig {
    std::string scene_path;   // never empty unless the caller explicitly asked for no scene
    std::string title;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string project_name; // empty when running without a project
};

// Applies a project to the VFS and derives the effective run settings.
//
// Extracted from Application::run() so the precedence rules are assertable
// rather than buried in a 400-line function that also opens a window and a
// device:
//
//   - With a project: the project declares the mounts, and supplies title,
//     window size and startup scene.
//   - An explicitly supplied scene_path always wins over the project's startup
//     scene, so `--scene` keeps working against a project.
//   - Without a project: the legacy walk-up for a directory containing both
//     Engine/ and Content/ still applies, and an unspecified scene falls back
//     to content://Scenes/Example.nfscene. This is what every in-repo sample and
//     test relies on, so it must not change.
//
// `vfs` receives the mounts. Returns false and fills out_error when a project
// was configured but could not be loaded or mounted — never a partial setup.
bool resolve_run_config(const ApplicationConfig& config,
                        assets::VirtualFileSystem& vfs,
                        ResolvedRunConfig& out,
                        std::string& out_error);

// The scene used when nothing specifies one and no project is in play.
inline constexpr const char* kDefaultScenePath = "content://Scenes/Example.nfscene";

} // namespace nf::runtime
