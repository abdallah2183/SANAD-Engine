#pragma once

#include <NF/Project/ProjectCooker.hpp>

#include <filesystem>
#include <string>

namespace nf::project {

struct BuildOptions {
    std::filesystem::path project_file; // the .nfproj to build
    std::filesystem::path player_exe;   // copied into the package when non-empty
    std::filesystem::path shader_dir;   // compiled SPIR-V, copied to <project>/Shaders/Basic3D
    std::filesystem::path output_dir;   // empty means <project>/dist
};

struct BuildReport {
    CookReport cook;
    size_t shaders_copied = 0;
    size_t files_packaged = 0;
    size_t manifest_entries = 0;
    std::filesystem::path output_dir;
    std::filesystem::path packaged_project; // the .nfproj inside the package
};

// Cooks the project, stages its shaders, and writes a self-contained package.
//
// The package is a plain directory — deliberately not an archive. A container
// format would add something to maintain and debug for no user-visible benefit
// at this stage, and "it shipped and it runs" is provable either way.
//
//   dist/
//     <Name>.nfproj      mounts are relative, so the package is relocatable
//     <player>.exe
//     Content/           the source assets, mirrored
//     Cache/             the cooked assets the runtime actually reads
//     Shaders/Basic3D/   SPIR-V, so the hardcoded build-tree search is not load-bearing
//     manifest.txt       sorted "<fingerprint>  <relative path>", diffable between builds
//
// Re-running replaces Content/, Cache/ and Shaders/ inside the output directory
// so a removed asset does not linger in the package, but leaves anything else in
// that directory alone.
bool build_project(const BuildOptions& opts, BuildReport& out, std::string& out_error);

} // namespace nf::project
