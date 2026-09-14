#pragma once

#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/RHI/RHI.hpp>

#include <functional>
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
    std::shared_ptr<MeshAsset> asset; // CPU data
    std::shared_ptr<rendering::StaticMesh> mesh; // GPU (after upload)
    AssetState state = AssetState::Unloaded;
    std::string error;
    bool operator==(const MeshHandle& o) const { return id == o.id; }
};

class AssetManager {
public:
    AssetManager(VirtualFileSystem& vfs, AssetRegistry& registry, rhi::IGraphicsDevice* device = nullptr);
    ~AssetManager();

    void set_device(rhi::IGraphicsDevice* device) { m_device = device; }

    // Load a mesh by AssetId (async CPU, GPU upload on update)
    // Returns a handle (shared_ptr) that will be updated as loading progresses.
    // If the asset is already cached, returns the same handle (cache identity).
    std::shared_ptr<MeshHandle> load_mesh(AssetId id);

    // Synchronous load for tests / simple use (blocks until Ready or Failed)
    std::shared_ptr<MeshHandle> load_mesh_sync(AssetId id);

    // Unload an asset (removes from cache, GPU resources will be freed when handle is released)
    void unload(AssetId id);

    // Must be called on main/render thread to process GPU uploads and completions
    // Returns number of assets that transitioned to Ready/Failed
    size_t update();

    // For testing: get the handle if it exists (even if not Ready)
    std::shared_ptr<MeshHandle> find(AssetId id) const;

    size_t cached_count() const;
    void clear();

private:
    struct PendingGpuUpload {
        std::shared_ptr<MeshHandle> handle;
    };

    void start_async_load(const AssetMetadata& meta, std::shared_ptr<MeshHandle> handle);

    VirtualFileSystem& m_vfs;
    AssetRegistry& m_registry;
    rhi::IGraphicsDevice* m_device = nullptr;

    mutable std::mutex m_mutex;
    std::unordered_map<AssetId, std::shared_ptr<MeshHandle>> m_cache;
    std::vector<PendingGpuUpload> m_pending_gpu;
};

} // namespace nf::assets
