#include <NF/Runtime/RunConfigResolver.hpp>

#include <NF/Runtime/Application.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Core/Logger.hpp>

#include <filesystem>

namespace nf::runtime {

namespace {

// How far up the tree the legacy engine-tree search will look. Kept at the
// original value so behaviour is unchanged.
constexpr int kMaxWalkUpLevels = 5;

} // namespace

bool resolve_run_config(const ApplicationConfig& config,
                        assets::VirtualFileSystem& vfs,
                        ResolvedRunConfig& out,
                        std::string& out_error) {
    out = ResolvedRunConfig{};
    out.scene_path = config.scene_path;
    out.title = config.title;
    out.width = config.width;
    out.height = config.height;

    if (!config.project_path.empty()) {
        auto project = assets::ProjectDescriptor::load_from_file(config.project_path, out_error);
        if (!project) {
            out_error = "project '" + config.project_path + "': " + out_error;
            return false;
        }
        if (!project->apply_mounts(vfs, out_error)) {
            out_error = "project '" + project->name() + "': " + out_error;
            return false;
        }

        out.project_name = project->name();
        // The project is the authority for presentation; an explicit scene_path
        // still wins so `--scene` can point at a different scene inside it.
        out.title = project->title();
        out.width = project->window_width();
        out.height = project->window_height();
        if (out.scene_path.empty()) {
            out.scene_path = project->startup_scene();
        }

        NF_LOG_INFO(LogCategory::Core, "Project '{}' — {} mounts from '{}'", project->name(),
                    project->mounts().size(), config.project_path);
        for (const auto& m : project->mounts()) {
            NF_LOG_INFO(LogCategory::Core, "VFS mount {} -> {}", m.logical, m.physical.string());
        }
        return true;
    }

    // --- Legacy path: find the engine source tree by walking up -------------
    std::filesystem::path project_root = std::filesystem::current_path();
    for (int i = 0; i < kMaxWalkUpLevels; ++i) {
        if (std::filesystem::exists(project_root / "Engine") &&
            std::filesystem::exists(project_root / "Content")) {
            break;
        }
        auto parent = project_root.parent_path();
        if (parent == project_root) {
            break;
        }
        project_root = parent;
    }
    if (!std::filesystem::exists(project_root / "Content")) {
        project_root = std::filesystem::current_path();
    }

    auto setup_mount = [&](const std::string& logical, const std::filesystem::path& physical) {
        std::error_code ec;
        std::filesystem::create_directories(physical, ec);
        auto r = vfs.mount(logical, physical);
        if (!r.ok) {
            // A warning, not a failure: the legacy path tolerates a missing
            // mount because a bare sample run should still get as far as it can.
            NF_LOG_WARN(LogCategory::Core, "VFS mount failed {} -> {}: {}", logical,
                        physical.string(), r.error);
        } else {
            NF_LOG_INFO(LogCategory::Core, "VFS mount {} -> {}", logical, physical.string());
        }
    };
    setup_mount("engine://", project_root / "Engine");
    setup_mount("project://", project_root);
    setup_mount("content://", project_root / "Content");
    setup_mount("cache://", project_root / "Cache");

    if (out.scene_path.empty()) {
        out.scene_path = kDefaultScenePath;
    }
    return true;
}

} // namespace nf::runtime
