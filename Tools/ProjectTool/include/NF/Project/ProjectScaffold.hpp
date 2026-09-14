#pragma once

#include <filesystem>
#include <string>

namespace nf::project {

struct ScaffoldOptions {
    std::filesystem::path root;         // directory to create the project in
    std::string name;                   // project name, also the .nfproj stem
    std::filesystem::path template_dir; // template to copy; empty means a bare project
};

// Creates a runnable project:
//
//   <root>/<Name>.nfproj      the descriptor
//   <root>/Content/...        copied from the template (scene, mesh, material)
//   <root>/Content/AssetRegistry.nfreg
//   <root>/Cache/             cooked output lands here
//   <root>/Shaders/           shipped SPIR-V lands here (shaders://)
//   <root>/dist/              packaging output
//   <root>/.gitignore         ignores Cache/ and dist/
//
// Refuses to overwrite an existing project file: silently replacing someone's
// project is not a recoverable mistake.
bool scaffold_project(const ScaffoldOptions& opts, std::string& out_error);

} // namespace nf::project
