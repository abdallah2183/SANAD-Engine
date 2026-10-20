#pragma once

#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/MeshAsset.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::assets {

enum class AssetState : uint8_t {
    Unloaded = 0,
    Loading = 1,
    Ready = 2,
    Failed = 3,
};

struct MeshHandle {
    AssetId id;
    // CPU data — the only copy this class holds. Before Phase 11 W1 a second,
    // GPU-side StaticMesh lived here too, which is what dragged RHI and
    // Rendering into this header. The GPU copy is Runtime's business now
    // (rendering::MeshLibrary).
    //
    // `asset` and `error` are written by whichever thread wins the load
    // (the worker job or the synchronous path) and read by the main thread.
    // They are written under `mutex`: two threads assigning the same
    // std::shared_ptr object is undefined behaviour, and a std::string
    // assignment tears.
    std::shared_ptr<MeshAsset> asset;
    // The state is observed from both threads, so it is atomic. It is stored
    // with release semantics AFTER the asset/error writes, so a reader that
    // acquires the state and then reads the asset sees a consistent pair.
    std::atomic<AssetState> state{AssetState::Unloaded};
    std::string error;
    mutable std::mutex mutex;
    bool operator==(const MeshHandle& o) const { return id == o.id; }
};

// CPU-pure asset loading (Phase 11, W1).
//
// This class reads bytes and parses them; that is all it does. Before W1 it
// also held an rhi::IGraphicsDevice and uploaded meshes itself, duplicating the
// upload path that rendering::MeshLibrary already owned — and that duplication
// is what forced Engine/Assets to depend on Engine/Rendering. Turning a cached
// asset into a drawable mesh is rendering::make_static_mesh (Rendering/
// MeshUpload.hpp), on the renderer's side of the boundary.
class AssetManager {
public:
    AssetManager(VirtualFileSystem& vfs, AssetRegistry& registry);
    ~AssetManager();

    // Load a mesh by AssetId (async CPU read + parse; finalized by update()).
    // Returns a handle (shared_ptr) that will be updated as loading progresses.
    // If the asset is already cached, returns the same handle (cache identity).
    std::shared_ptr<MeshHandle> load_mesh(AssetId id);

    // Synchronous load for tests / simple use (blocks until Ready or Failed)
    std::shared_ptr<MeshHandle> load_mesh_sync(AssetId id);

    // Unload an asset (removes from cache)
    void unload(AssetId id);

    // Finalizes loads whose worker job finished: flips Loading -> Ready.
    // Returns the number of assets finalized this call. Before Phase 11 W1
    // this is where the GPU upload happened; the renderer owns that now, so
    // this is a state transition, not a transfer.
    size_t update();

    // For testing: get the handle if it exists (even if not Ready)
    std::shared_ptr<MeshHandle> find(AssetId id) const;

    size_t cached_count() const;
    void clear();

private:
    // A worker job that finished its read + parse and is waiting for the main
    // thread to flip its state. (Replaces the pre-W1 PendingGpuUpload queue,
    // which existed only because the upload had to happen on this thread.)
    struct CompletedLoad {
        std::shared_ptr<MeshHandle> handle;
    };

    void start_async_load(const AssetMetadata& meta, std::shared_ptr<MeshHandle> handle);

    VirtualFileSystem& m_vfs;
    AssetRegistry& m_registry;

    mutable std::mutex m_mutex;
    std::unordered_map<AssetId, std::shared_ptr<MeshHandle>> m_cache;
    std::vector<CompletedLoad> m_completed;
};

} // namespace nf::assets
