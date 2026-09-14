#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nf::assets {

// A single VFS mount declared by a project.
struct ProjectMount {
    std::string logical;            // e.g. "content://"
    std::filesystem::path physical; // absolute, resolved against the project root
};

// ProjectDescriptor — the description of a game project: where its content,
// cache and shaders live, which scene to start, and how to open a window.
//
// Why this exists: the runtime used to discover its project by walking up the
// filesystem looking for a directory that contained BOTH `Engine/` and
// `Content/` (Application.cpp:39-52). That is the *engine source tree* layout, so
// a shipped game — which has neither — could not run at all. The descriptor
// replaces the guess with a declaration.
//
// Format: line-based tolerant text, matching .nfreg / .nfmat / .nfscene. The
// project has no JSON parser and hand-rolls its own formats; adding a dependency
// for this would be inconsistent.
//
//   # NOVAForge Project
//   version: 1
//   name: Demo
//   startup_scene: content://Scenes/Main.nfscene
//   mount: content:// -> Content
//
// Rules:
//   - Unknown keys are ignored, so an older engine can still open a newer file
//     that only added optional fields.
//   - A malformed `mount:` line is an error, not a silently ignored line.
//   - `version` is required, and a future version is rejected outright rather
//     than half-parsed.
//   - Relative mount paths resolve against the descriptor's own directory, so a
//     project stays relocatable. They may not escape it.
class ProjectDescriptor {
public:
    static constexpr uint32_t kCurrentVersion = 1;
    static constexpr const char* kExtension = ".nfproj";

    ProjectDescriptor() = default;

    // --- Loading ------------------------------------------------------------

    // Parse a descriptor from disk. Relative mounts resolve against the file's
    // own directory. Returns nullopt and fills out_error on any problem.
    static std::optional<ProjectDescriptor> load_from_file(const std::filesystem::path& file,
                                                           std::string& out_error);

    // Parse from text. `base_dir` is the project root that relative mounts
    // resolve against — exposed separately so tests need no temp files.
    static std::optional<ProjectDescriptor> parse(std::string_view text,
                                                  const std::filesystem::path& base_dir,
                                                  std::string& out_error);

    // --- Saving -------------------------------------------------------------

    // Write the descriptor. Atomic (temp + rename), same as AssetRegistry.
    bool save_to_file(const std::filesystem::path& file, std::string& out_error) const;

    // A ready-to-use descriptor for a brand new project rooted at `root`:
    // standard mounts, default startup scene, default window. Used by `nf new`.
    static ProjectDescriptor make_default(const std::filesystem::path& root, const std::string& name);

    // --- Use ----------------------------------------------------------------

    // Mount every declared mount on the VFS. Stops at the first failure and
    // reports which mount failed, because "VFS setup failed" alone is useless
    // when a project declares five of them.
    bool apply_mounts(VirtualFileSystem& vfs, std::string& out_error) const;

    // Physical path for a logical mount, or an empty path when not declared.
    std::filesystem::path mount_path(std::string_view logical) const;

    bool has_mount(std::string_view logical) const { return !mount_path(logical).empty(); }

    // --- Accessors ----------------------------------------------------------

    const std::filesystem::path& root() const { return m_root; }
    const std::string& name() const { return m_name; }
    const std::string& title() const { return m_title; }
    const std::string& startup_scene() const { return m_startup_scene; }
    uint32_t window_width() const { return m_window_width; }
    uint32_t window_height() const { return m_window_height; }
    const std::vector<ProjectMount>& mounts() const { return m_mounts; }

    // --- Mutators (used by tooling) -----------------------------------------

    void set_root(std::filesystem::path root) { m_root = std::move(root); }
    void set_name(std::string name) { m_name = std::move(name); }
    void set_title(std::string title) { m_title = std::move(title); }
    void set_startup_scene(std::string scene) { m_startup_scene = std::move(scene); }
    void set_window(uint32_t width, uint32_t height) {
        m_window_width = width;
        m_window_height = height;
    }
    // Replaces any existing mount with the same logical prefix.
    void add_mount(std::string logical, std::filesystem::path physical);

private:
    std::filesystem::path m_root;
    std::string m_name;
    std::string m_title;
    std::string m_startup_scene = "content://Scenes/Main.nfscene";
    uint32_t m_window_width = 1280;
    uint32_t m_window_height = 720;
    std::vector<ProjectMount> m_mounts;
};

} // namespace nf::assets
