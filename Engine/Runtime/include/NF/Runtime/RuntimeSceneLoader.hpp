#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Scene/Scene.hpp>

#include <string>

namespace nf::runtime {

// Loads a .nfscene file (versioned) into a Scene object
// The file is read via VFS logical path (e.g. content://Scenes/Example.nfscene)
// It validates version, AssetId, hierarchy (no cycles), and handles missing assets gracefully

struct SceneLoadResult {
    bool success = false;
    std::string error;
    std::unique_ptr<scene::Scene> scene;
    // List of missing assets (if any, but scene is still considered partially successful)
    std::vector<std::string> missing_assets;
    // Warnings (e.g. unknown version, but loaded anyway)
    std::vector<std::string> warnings;
};

SceneLoadResult load_scene_from_vfs(assets::VirtualFileSystem& vfs, const std::string& logical_path);
SceneLoadResult load_scene_from_physical(const std::filesystem::path& physical_path);

bool save_scene_to_vfs(assets::VirtualFileSystem& vfs, const std::string& logical_path, const scene::Scene& scene, std::string& out_error);
bool save_scene_to_physical(const std::filesystem::path& physical_path, const scene::Scene& scene, std::string& out_error);

} // namespace nf::runtime
