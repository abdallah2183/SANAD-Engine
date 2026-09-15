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

/// Copies every scene-file component from entity `se` (in `src`) onto the
/// already-created entity `de` (in `dst`): Transform, Name, PrefabLink, Mesh,
/// Light, Camera, RigidBody (with the live body handle reset — handles are
/// per-session, never scene data), Collider, Animation, Audio, GameplayModule.
///
/// This is the ONE component list shared by prefab cloning and scene merging:
/// a component type parsed by the loader above must be copied here, so the
/// two consumers cannot drift apart (a streamed chunk that silently drops
/// its physics, or a prefab that drops its audio, fails the same way).
/// Parent links are copied verbatim; remapping them is the caller's job.
void copy_scene_entity(const ecs::World& src, ecs::Entity se, ecs::World& dst, ecs::Entity de);

struct SceneMergeResult {
    bool success = false;
    std::string error;
    /// One new root per root subtree in the chunk file, in file order.
    std::vector<ecs::Entity> created;
    std::vector<std::string> warnings;
};

/// Loads a .nfscene chunk file and merges it into a LIVE world (world
/// streaming): every root subtree of the chunk is cloned under `dst_world`
/// with internal parent links remapped, roots becoming parentless. The parse
/// goes through load_scene_from_vfs, so the merge understands exactly what
/// the loader understands — no second component list.
///
/// Runtime handles (physics bodies, GPU mesh uploads) are deliberately NOT
/// created here: after merging into a live Runtime world, call
/// Runtime::rebuild_physics_from_scene() and let the next frames' asset sync
/// upload the meshes, exactly like a fresh load.
SceneMergeResult merge_scene_into_world(assets::VirtualFileSystem& vfs, const std::string& logical_path,
                                        ecs::World& dst_world);

/// The scene's on-disk text, without touching the filesystem.
///
/// Split out so the save system can take a snapshot on the main thread and hand
/// the bytes to a worker. Serializing the live scene off-thread would race with
/// the frame that is still running, and the alternative — holding the main
/// thread while a file is written — is what makes a save stutter.
std::string serialize_scene_to_text(const scene::Scene& scene);

} // namespace nf::runtime
