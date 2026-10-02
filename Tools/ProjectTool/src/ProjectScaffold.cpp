#include <NF/Project/ProjectScaffold.hpp>

#include <NF/Assets/ProjectDescriptor.hpp>

#include "PathUtils.hpp"

#include <filesystem>

namespace nf::project {

namespace {

bool is_valid_name(const std::string& name) {
    if (name.empty()) return false;
    // A name containing a separator would put the .nfproj somewhere other than
    // the project root, which the mount defaults would then disagree with.
    return name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
}

} // namespace

std::string project_template_name_list() {
    std::string result;
    for (std::size_t i = 0; i < kProjectTemplateNames.size(); ++i) {
        if (i != 0) result += ", ";
        result += kProjectTemplateNames[i];
    }
    return result;
}

std::filesystem::path resolve_project_template(std::string_view name,
                                               const std::filesystem::path& templates_root,
                                               const std::filesystem::path& default_template_dir,
                                               std::string& out_error) {
    out_error.clear();
    if (name.empty()) return default_template_dir;

    for (const std::string_view allowed : kProjectTemplateNames) {
        if (name != allowed) continue;

        if (templates_root.empty()) {
            out_error = "template '" + std::string(allowed) +
                        "' is unavailable because the templates root is not configured; valid templates: " +
                        project_template_name_list();
            return {};
        }

        const auto selected = (templates_root / allowed).lexically_normal();
        std::error_code ec;
        if (!std::filesystem::is_directory(selected, ec) || ec) {
            out_error = "template '" + std::string(allowed) + "' is not available at '" +
                        selected.string() + "'; valid templates: " + project_template_name_list();
            return {};
        }
        return selected;
    }

    out_error = "unknown template '" + std::string(name) + "'; valid templates: " +
                project_template_name_list();
    return {};
}

bool scaffold_project(const ScaffoldOptions& opts, std::string& out_error) {
    if (!is_valid_name(opts.name)) {
        out_error = "invalid project name '" + opts.name +
                    "' (no path separators, no leading dot)";
        return false;
    }
    if (opts.root.empty()) {
        out_error = "project root is empty";
        return false;
    }

    const auto root = opts.root.lexically_normal();
    const auto project_file = root / (opts.name + assets::ProjectDescriptor::kExtension);

    std::error_code ec;
    if (std::filesystem::exists(project_file, ec)) {
        // Refuse rather than clobber: replacing a project file is not something
        // the user can undo by re-running the command.
        out_error = "a project already exists at '" + project_file.string() + "'";
        return false;
    }

    std::filesystem::create_directories(root, ec);
    if (ec) {
        out_error = "failed to create '" + root.string() + "': " + ec.message();
        return false;
    }

    if (!opts.template_dir.empty()) {
        if (!detail::copy_tree(opts.template_dir, root, out_error)) {
            return false;
        }
    }

    // The directories the descriptor's mounts point at. Created up front so a
    // freshly scaffolded project can be cooked and run immediately.
    for (const char* sub : {"Cache", "Shaders", "dist", "Content"}) {
        std::filesystem::create_directories(root / sub, ec);
        if (ec) {
            out_error = "failed to create '" + (root / sub).string() + "': " + ec.message();
            return false;
        }
    }

    auto desc = assets::ProjectDescriptor::make_default(root, opts.name);
    desc.set_title(opts.name);
    desc.set_startup_scene("content://Scenes/Main.nfscene");
    if (!desc.save_to_file(project_file, out_error)) {
        return false;
    }

    // Generated output must not be committed. Without this the first `nf build`
    // drops a Cache/ and a dist/ into the user's repository.
    if (!detail::write_text(root / ".gitignore", "Cache/\ndist/\n*.nfproj.tmp\n", out_error)) {
        return false;
    }

    return true;
}

} // namespace nf::project
