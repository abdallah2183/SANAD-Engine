#include <NF/Assets/AssetManager.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/Jobs/JobSystem.hpp>

#include <filesystem>

namespace nf::assets {

AssetManager::AssetManager(VirtualFileSystem& vfs, AssetRegistry& registry, rhi::IGraphicsDevice* device)
    : m_vfs(vfs), m_registry(registry), m_device(device) {}

AssetManager::~AssetManager() = default;

std::shared_ptr<MeshHandle> AssetManager::load_mesh(AssetId id) {
    if (!id.valid()) {
        auto h = std::make_shared<MeshHandle>();
        h->id = id;
        h->state = AssetState::Failed;
        h->error = "Invalid AssetId";
        return h;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cache.find(id);
        if (it != m_cache.end()) return it->second;
    }

    const AssetMetadata* meta = m_registry.find(id);
    if (!meta) {
        auto h = std::make_shared<MeshHandle>();
        h->id = id;
        h->state = AssetState::Failed;
        h->error = "AssetId not found in registry: " + id.to_string();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache[id] = h;
        return h;
    }

    if (meta->type != AssetType::Mesh) {
        auto h = std::make_shared<MeshHandle>();
        h->id = id;
        h->state = AssetState::Failed;
        h->error = "Asset is not a Mesh: " + meta->logical_path;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache[id] = h;
        return h;
    }

    auto handle = std::make_shared<MeshHandle>();
    handle->id = id;
    handle->state = AssetState::Loading;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache[id] = handle;
    }

    start_async_load(*meta, handle);
    return handle;
}

std::shared_ptr<MeshHandle> AssetManager::load_mesh_sync(AssetId id) {
    auto handle = load_mesh(id);
    // If it was already cached and Ready/Failed, return immediately
    if (handle->state == AssetState::Ready || handle->state == AssetState::Failed) return handle;

    // For sync, we do the CPU load directly on this thread (not via JobSystem) and then GPU upload via update
    // But load_mesh already started an async job, so we need to wait for it
    // For simplicity, do a synchronous load here: directly read and parse
    const AssetMetadata* meta = m_registry.find(id);
    if (!meta) return handle;

    auto vfs_result = m_vfs.read_bytes(meta->cooked_path);
    if (!vfs_result.ok) {
        handle->state = AssetState::Failed;
        handle->error = vfs_result.error;
        return handle;
    }

    std::string parse_error;
    auto asset = MeshAsset::load_from_bytes(std::span<const uint8_t>(vfs_result.value), parse_error);
    if (!asset) {
        handle->state = AssetState::Failed;
        handle->error = parse_error;
        return handle;
    }

    handle->asset = std::shared_ptr<MeshAsset>(std::move(asset));
    // Now do GPU upload on this thread (since we're sync, we're on main thread, so it's safe)
    if (m_device) {
        auto mesh = handle->asset->to_static_mesh(meta->logical_path);
        if (mesh && mesh->upload(*m_device)) {
            handle->mesh = std::shared_ptr<rendering::StaticMesh>(std::move(mesh));
            handle->state = AssetState::Ready;
        } else {
            handle->state = AssetState::Failed;
            handle->error = "GPU upload failed";
        }
    } else {
        // Headless: no GPU, just mark Ready with CPU asset only
        handle->state = AssetState::Ready;
    }

    // Remove from pending GPU queue if it was there (the async job may have also queued)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Remove any pending that matches this handle (to avoid double upload)
        m_pending_gpu.erase(std::remove_if(m_pending_gpu.begin(), m_pending_gpu.end(),
            [&](const PendingGpuUpload& p){ return p.handle->id == id; }), m_pending_gpu.end());
    }

    return handle;
}

void AssetManager::start_async_load(const AssetMetadata& meta, std::shared_ptr<MeshHandle> handle) {
    // Capture by value for the job
    std::string cooked_path = meta.cooked_path;
    AssetId id = meta.id;

    // Use JobSystem if initialized, otherwise do it synchronously
    auto do_load = [this, cooked_path, handle]() {
        auto vfs_result = m_vfs.read_bytes(cooked_path);
        if (!vfs_result.ok) {
            handle->state = AssetState::Failed;
            handle->error = vfs_result.error;
            return;
        }
        std::string parse_error;
        auto asset = MeshAsset::load_from_bytes(std::span<const uint8_t>(vfs_result.value), parse_error);
        if (!asset) {
            handle->state = AssetState::Failed;
            handle->error = parse_error;
            return;
        }
        handle->asset = std::shared_ptr<MeshAsset>(std::move(asset));
        // Queue for GPU upload on main thread
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending_gpu.push_back({handle});
        }
    };

    if (nf::JobSystem::instance().is_initialized()) {
        nf::JobSystem::instance().enqueue([do_load]() { do_load(); });
    } else {
        do_load();
    }
}

size_t AssetManager::update() {
    std::vector<PendingGpuUpload> pending_copy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending_copy.swap(m_pending_gpu);
    }

    size_t completed = 0;
    for (auto& p : pending_copy) {
        auto handle = p.handle;
        if (!handle || handle->state != AssetState::Loading) continue;
        if (!handle->asset) {
            handle->state = AssetState::Failed;
            handle->error = "No CPU asset for GPU upload";
            ++completed;
            continue;
        }
        if (m_device) {
            auto mesh = handle->asset->to_static_mesh(handle->asset->logical_path);
            if (mesh && mesh->upload(*m_device)) {
                handle->mesh = std::shared_ptr<rendering::StaticMesh>(std::move(mesh));
                handle->state = AssetState::Ready;
            } else {
                handle->state = AssetState::Failed;
                handle->error = "GPU upload failed in update()";
            }
        } else {
            // Headless: no GPU, just mark Ready
            handle->state = AssetState::Ready;
        }
        ++completed;
    }
    return completed;
}

void AssetManager::unload(AssetId id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(id);
    // Also remove any pending GPU uploads for this id
    m_pending_gpu.erase(std::remove_if(m_pending_gpu.begin(), m_pending_gpu.end(),
        [&](const PendingGpuUpload& p){ return p.handle->id == id; }), m_pending_gpu.end());
}

std::shared_ptr<MeshHandle> AssetManager::find(AssetId id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(id);
    return it != m_cache.end() ? it->second : nullptr;
}

size_t AssetManager::cached_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.size();
}

void AssetManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
    m_pending_gpu.clear();
}

} // namespace nf::assets
