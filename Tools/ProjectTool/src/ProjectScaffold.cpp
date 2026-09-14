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
