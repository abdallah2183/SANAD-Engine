#include <NF/Assets/AssetManager.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/Jobs/JobSystem.hpp>

#include <algorithm>
#include <filesystem>

namespace nf::assets {

AssetManager::AssetManager(VirtualFileSystem& vfs, AssetRegistry& registry)
    : m_vfs(vfs), m_registry(registry) {}

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
    // Already cached and terminal -> nothing to do.
    if (handle->state.load(std::memory_order_acquire) == AssetState::Ready ||
        handle->state.load(std::memory_order_acquire) == AssetState::Failed) {
        return handle;
    }

    const AssetMetadata* meta = m_registry.find(id);
    if (!meta) return handle;

    // The worker job and this path can both be loading the SAME handle:
    // load_mesh() already enqueued the async job, and then the synchronous
    // path did the read+parse itself. Two threads assigning the same
    // std::shared_ptr is undefined behaviour, and a std::string assignment
    // tears. The handle's own mutex makes the two paths mutually exclusive:
    // whoever gets there first does the work, the other finds the result
    // already there.
    std::lock_guard<std::mutex> handle_lock(handle->mutex);

    if (handle->state.load(std::memory_order_acquire) == AssetState::Ready ||
        handle->state.load(std::memory_order_acquire) == AssetState::Failed) {
        return handle;
    }
    if (handle->asset) {
        // The worker already produced the asset; only the state was missing.
        handle->state.store(AssetState::Ready, std::memory_order_release);
        return handle;
    }

    auto vfs_result = m_vfs.read_bytes(meta->cooked_path);
    if (!vfs_result.ok && meta->logical_path != meta->cooked_path) {
        // A freshly scaffolded project has Content/ but an empty Cache/ until
        // `nf cook` runs. The editor opens the project before that, so a
        // cooked-path-only read renders a brand-new template as an empty scene
        // ("mesh asset ... failed: cache://Meshes/cube.nfmesh"). The authored
        // mesh IS the cooked format (.nfmesh), so read it directly; shipping
        // builds still prefer the cooked copy.
        vfs_result = m_vfs.read_bytes(meta->logical_path);
    }
    if (!vfs_result.ok) {
        handle->error = vfs_result.error;
        handle->state.store(AssetState::Failed, std::memory_order_release);
        return handle;
    }

    std::string parse_error;
    auto asset = MeshAsset::load_from_bytes(std::span<const uint8_t>(vfs_result.value), parse_error);
    if (!asset) {
        handle->error = parse_error;
        handle->state.store(AssetState::Failed, std::memory_order_release);
        return handle;
    }

    handle->asset = std::shared_ptr<MeshAsset>(std::move(asset));
    // Phase 11 W1: no GPU upload here. The caller (Runtime) turns the CPU asset
    // into a StaticMesh via rendering::make_static_mesh and uploads it.
    handle->state.store(AssetState::Ready, std::memory_order_release);

    // Drain any CompletedLoad the racing async job may have queued for this id;
    // a leftover would be a no-op (state is already terminal) but leaving it
    // would grow m_completed for nothing.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_completed.erase(std::remove_if(m_completed.begin(), m_completed.end(),
            [&](const CompletedLoad& c){ return c.handle->id == id; }), m_completed.end());
    }

    return handle;
}

void AssetManager::start_async_load(const AssetMetadata& meta, std::shared_ptr<MeshHandle> handle) {
    // Capture the paths and id by value; the job outlives `meta`.
    std::string cooked_path = meta.cooked_path;
    std::string authored_path = meta.logical_path;
    AssetId id = meta.id;

    auto do_load = [this, cooked_path, authored_path, id, handle]() {
        // The handle's mutex serialises this worker against the synchronous
        // path, which may be loading the same handle right now. Without it
        // both would assign handle->asset concurrently.
        std::lock_guard<std::mutex> handle_lock(handle->mutex);

        // A sync load may have already finalized this handle. Never overwrite a
        // terminal state from a worker.
        if (handle->state.load(std::memory_order_acquire) != AssetState::Loading) {
            return;
        }
        auto vfs_result = m_vfs.read_bytes(cooked_path);
        if (!vfs_result.ok && authored_path != cooked_path) {
            // Same un-cooked-project fallback the sync path documents above.
            vfs_result = m_vfs.read_bytes(authored_path);
        }
        if (!vfs_result.ok) {
            handle->error = vfs_result.error;
            handle->state.store(AssetState::Failed, std::memory_order_release);
            return;
        }
        std::string parse_error;
        auto asset = MeshAsset::load_from_bytes(std::span<const uint8_t>(vfs_result.value), parse_error);
        if (!asset) {
            handle->error = parse_error;
            handle->state.store(AssetState::Failed, std::memory_order_release);
            return;
        }
        handle->asset = std::shared_ptr<MeshAsset>(std::move(asset));
        // Do NOT flip to Ready here: the worker thread must not publish the
        // state change without a memory fence the main thread observes. Queue
        // a CompletedLoad; update() finalizes on the main thread.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_completed.push_back({handle});
        }
    };

    if (nf::JobSystem::instance().is_initialized()) {
        nf::JobSystem::instance().enqueue([do_load]() { do_load(); });
    } else {
        do_load();
    }
}

size_t AssetManager::update() {
    std::vector<CompletedLoad> completed_copy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        completed_copy.swap(m_completed);
    }

    size_t finalized = 0;
    for (auto& c : completed_copy) {
        auto handle = c.handle;
        if (!handle) continue;
        // The handle's mutex guards the asset/error writes. The state is
        // atomic, so the check below is safe against a worker that is
        // publishing a failure at the same time.
        std::lock_guard<std::mutex> handle_lock(handle->mutex);
        // Only finalize a load that is still pending. A sync path or a later
        // unload may have changed the state already; do not clobber it.
        if (handle->state.load(std::memory_order_acquire) != AssetState::Loading) continue;
        if (!handle->asset) {
            handle->error = "CPU parse produced no asset";
            handle->state.store(AssetState::Failed, std::memory_order_release);
            ++finalized;
            continue;
        }
        handle->state.store(AssetState::Ready, std::memory_order_release);
        ++finalized;
    }
    return finalized;
}

void AssetManager::unload(AssetId id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(id);
    // Also drop any in-flight completed loads for this id; their handle would
    // otherwise be finalized into Ready for an id the caller just evicted.
    m_completed.erase(std::remove_if(m_completed.begin(), m_completed.end(),
        [&](const CompletedLoad& c){ return c.handle && c.handle->id == id; }), m_completed.end());
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
    m_completed.clear();
}

} // namespace nf::assets
