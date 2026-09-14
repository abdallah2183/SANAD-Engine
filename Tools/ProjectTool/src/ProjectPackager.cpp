#include <NF/Project/ProjectPackager.hpp>

#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include "PathUtils.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace nf::project {

namespace {

constexpr const char* kRegistryLogical = "content://AssetRegistry.nfreg";
constexpr const char* kManifestName = "manifest.txt";

// Collect "<fingerprint>  <relative path>" for everything under `dir`, sorted so
// two builds can be compared with diff rather than by eye.
bool collect_manifest(const std::filesystem::path& dir,
                      const std::filesystem::path& base,
                      std::vector<std::string>& out_lines,
                      size_t& out_count,
                      std::string& out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return true; // an absent optional subtree is not an error
    }
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto rel = std::filesystem::relative(it->path(), base, ec);
        if (ec) continue;
        std::string fp_err;
        const auto fp = detail::fingerprint_file(it->path(), fp_err);
        if (fp.empty()) {
            out_error = fp_err;
            return false;
        }
        out_lines.push_back(fp + "  " + rel.generic_string());
        ++out_count;
    }
    if (ec) {
        out_error = "failed to enumerate '" + dir.string() + "': " + ec.message();
        return false;
    }
    return true;
}

} // namespace

bool build_project(const BuildOptions& opts, BuildReport& out, std::string& out_error) {
    out = BuildReport{};

    if (opts.project_file.empty()) {
        out_error = "no project file given";
        return false;
    }

    auto desc = assets::ProjectDescriptor::load_from_file(opts.project_file, out_error);
    if (!desc) {
        return false;
    }
    const auto root = desc->root();

    // --- cook ---------------------------------------------------------------
    assets::VirtualFileSystem vfs;
    if (!desc->apply_mounts(vfs, out_error)) {
        return false;
    }

    assets::AssetRegistry registry;
    {
        auto exists = vfs.exists(kRegistryLogical);
        if (exists.ok && exists.value) {
            if (!registry.load(vfs, kRegistryLogical, out_error)) {
                return false;
            }
        } else {
            registry.set_version(assets::AssetRegistry::kCurrentVersion);
        }
    }

    if (!cook_all(vfs, registry, "content://", "cache://", out.cook, out_error)) {
        return false;
    }
    if (!out.cook.ok()) {
        // Do not package a project with a broken asset: the failure would show
        // up as a missing mesh in the shipped game instead of here.
        return false;
    }
    if (out.cook.cooked > 0 || out.cook.pruned > 0) {
        if (!registry.save(vfs, kRegistryLogical, out_error)) {
            return false;
        }
    }

    // --- stage shaders ------------------------------------------------------
    // Copied into the project so the package carries them. Without this the
    // runtime falls back to searching build/DebugNinja/..., which does not exist
    // outside the engine tree.
    const auto project_shaders = root / "Shaders" / "Basic3D";
    if (!opts.shader_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(opts.shader_dir, ec)) {
            if (!detail::copy_tree(opts.shader_dir, project_shaders, out_error)) {
                return false;
            }
            for (auto it = std::filesystem::recursive_directory_iterator(project_shaders, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec) ++out.shaders_copied;
            }
        } else {
            out_error = "shader directory does not exist: '" + opts.shader_dir.string() +
                        "' (build the engine first)";
            return false;
        }
    }

    // --- assemble the package ----------------------------------------------
    const auto out_dir = opts.output_dir.empty() ? (root / "dist") : opts.output_dir;
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        out_error = "failed to create '" + out_dir.string() + "': " + ec.message();
        return false;
    }

    // Replace only the subtrees this tool owns, so a removed asset does not
    // linger in the package and anything else the user keeps there survives.
    for (const char* sub : {"Content", "Cache", "Shaders"}) {
        std::filesystem::remove_all(out_dir / sub, ec);
    }
    std::filesystem::remove(out_dir / kManifestName, ec);

    for (const char* sub : {"Content", "Cache", "Shaders"}) {
        const auto src = root / sub;
        if (!std::filesystem::exists(src, ec)) continue;
        if (!detail::copy_tree(src, out_dir / sub, out_error)) {
            return false;
        }
    }

    // The descriptor declares relative mounts, so copying it into the package is
    // what makes the package relocatable.
    const auto packaged_project = out_dir / opts.project_file.filename();
    std::filesystem::copy_file(opts.project_file, packaged_project,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        out_error = "failed to copy project file into the package: " + ec.message();
        return false;
    }

    if (!opts.player_exe.empty()) {
        if (!std::filesystem::exists(opts.player_exe, ec)) {
            out_error = "player executable does not exist: '" + opts.player_exe.string() + "'";
            return false;
        }
        std::filesystem::copy_file(opts.player_exe, out_dir / opts.player_exe.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            out_error = "failed to copy player into the package: " + ec.message();
            return false;
        }
    }

    // --- manifest -----------------------------------------------------------
    std::vector<std::string> lines;
    size_t count = 0;
    for (const char* sub : {"Content", "Cache", "Shaders"}) {
        if (!collect_manifest(out_dir / sub, out_dir, lines, count, out_error)) {
            return false;
        }
    }
    std::sort(lines.begin(), lines.end());

    std::string manifest = "# NOVAForge package manifest\n";
    manifest += "# fingerprint       relative path\n";
    for (const auto& l : lines) {
        manifest += l;
        manifest += "\n";
    }
    if (!detail::write_text(out_dir / kManifestName, manifest, out_error)) {
        return false;
    }

    out.manifest_entries = count;
    out.files_packaged = count;
    out.output_dir = out_dir;
    out.packaged_project = packaged_project;
    return true;
}

} // namespace nf::project
