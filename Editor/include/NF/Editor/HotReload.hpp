#pragma once

// NF/Editor/HotReload.hpp — live asset refresh without restarting (Phase 5).
//
// Watches the files a session depends on and reloads what changed on disk:
//   meshes    -> re-copy source to cooked, refresh fingerprint, rebuild the
//                live GPU copy in place (handles stay valid)
//   textures  -> re-decode/re-upload, rebind every material sampling them
//   materials -> re-read into the live instance (SKIPPED while the entry has
//                unsaved editor edits: user work always wins over disk)
//
// Scene files are NEVER auto-reloaded (that would clobber unsaved edits).
// Everything is poll-driven and synchronous; failures warn and keep the last
// good GPU copy — a reload can never crash or blank the viewport.

#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/FileWatcher.hpp>

#include <string>
#include <vector>

namespace nf::runtime {
class Runtime;
} // namespace nf::runtime

namespace nf::editor {

struct ReloadResult {
    std::string path;
    std::string kind; // "mesh", "texture", "material"
    bool ok = false;
    std::string message;
};

class HotReload {
public:
    HotReload() = default;

    void watch_mesh(const assets::AssetId& id, const std::string& source_logical);
    void watch_texture(const std::string& logical_path);
    void watch_material(const std::string& logical_path);
    void clear();

    // Re-baselines a path without reloading (call after our own saves so
    // they never echo back as external changes).
    void refresh(assets::VirtualFileSystem& vfs, const std::string& logical_path);

    // Polls the watcher and reloads what changed. Never throws, never
    // crashes: every failure becomes a warn-level ReloadResult. (The
    // AssetManager is driven through Runtime internally — no direct handle
    // needed here.)
    std::vector<ReloadResult> poll(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg,
                                   runtime::Runtime& runtime);

    size_t watched_count() const;

    // The results of the last poll: the shell uses them to drop now-stale
    // editor-side caches (a reloaded texture source has an outdated thumbnail
    // even after the scene is already sampling the new bytes).
    const std::vector<ReloadResult>& last_results() const { return m_last; }

private:
    struct MeshWatch {
        assets::AssetId id;
        std::string source;
    };
    FileWatcher m_watcher;
    std::vector<MeshWatch> m_meshes;
    std::vector<std::string> m_textures;
    std::vector<std::string> m_materials;
    std::vector<ReloadResult> m_last;

    static bool contains(const std::vector<std::string>& v, const std::string& s);
};

} // namespace nf::editor
