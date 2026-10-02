#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace nf::project {

/// The only project templates `nf new` may select. Names are case-sensitive;
/// keeping this as an allow-list makes path traversal and accidental fallback
/// impossible by construction.
inline constexpr std::array<std::string_view, 4> kProjectTemplateNames{
    "Default", "ThirdPerson", "FPSStarter", "Platformer2D"};

/// Human-readable valid-name list used in CLI errors and help-facing callers.
std::string project_template_name_list();

/// Resolves one explicitly requested template name beneath `templates_root`.
/// An empty name preserves the no-flag path and returns `default_template_dir`
/// unchanged. Unknown names (including paths and differently-cased names) fail;
/// they never resolve to Default.
std::filesystem::path resolve_project_template(std::string_view name,
                                               const std::filesystem::path& templates_root,
                                               const std::filesystem::path& default_template_dir,
                                               std::string& out_error);

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
