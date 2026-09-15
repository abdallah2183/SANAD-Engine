#pragma once

// NF/Runtime/WorldStreamer.hpp — which chunks are loaded, and when (Phase 11, W5)
//
// The policy half of world streaming. It decides *which* chunks should be loaded
// for a given volume and drives a load/unload callback pair; it does no IO, knows
// nothing about the VFS, and owns no entities. That split is what makes the
// hysteresis and range behaviour testable without a filesystem or a GPU.
//
// The volume and the grid math are in NF/Scene/StreamingVolume.hpp. The IO half
// is the Runtime's, supplied through `set_handlers`.
//
// Scope note (unchanged from the plan): distance-based load/unload on a uniform
// grid with hysteresis. LOD, load priority, a memory budget and background
// refinement are deliberately absent. The per-update load cap below is not a
// priority system — it exists so a volume that teleports does not try to load a
// hundred chunks in one frame.

#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>
#include <NF/Scene/StreamingVolume.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nf::runtime {

class WorldStreamer {
public:
    /// Loads one chunk's content into the live world.
    ///
    /// `out_created` must receive exactly the entities the implementation added,
    /// so an unload can remove precisely those and nothing else. Returning false
    /// leaves the chunk unloaded and is retried on a later update — a missing
    /// chunk file is not a reason to give up on the region.
    using LoadFn = std::function<bool(const scene::ChunkCoord& coord,
                                      std::vector<ecs::Entity>& out_created,
                                      std::string& out_error)>;

    /// Releases one chunk. Receives the same entity list the load produced.
    using UnloadFn = std::function<void(const scene::ChunkCoord& coord,
                                        const std::vector<ecs::Entity>& created)>;

    void set_handlers(LoadFn load, UnloadFn unload);

    /// Replaces the active volume. Call every frame with the moving volume, or
    /// once if it is static. A volume with a non-positive `chunk_size` is
    /// ignored rather than treated as a zero-sized grid.
    void set_volume(const scene::StreamingVolume& volume);

    [[nodiscard]] const scene::StreamingVolume& volume() const { return m_volume; }
    [[nodiscard]] bool has_volume() const { return m_has_volume; }

    /// Upper bound on loads per `update()`. Default 4. Zero disables loading
    /// entirely (unloading still runs), which is what a paused or backgrounded
    /// session wants.
    void set_max_loads_per_update(u32 max_loads) { m_max_loads_per_update = max_loads; }
    [[nodiscard]] u32 max_loads_per_update() const { return m_max_loads_per_update; }

    /// One step: load what came into range, release what left it.
    ///
    /// Returns the number of chunks loaded this call. Unloading is not capped —
    /// releasing memory should never be the thing that is deferred, and it is
    /// cheap next to a load.
    u32 update();

    /// Drops every loaded chunk through the unload handler. Call before the world
    /// it refers to is destroyed.
    void clear();

    // --- Observables --------------------------------------------------------
    //
    // These are what a test asserts on, and what a HUD would show. A streamer
    // that "ran" but loaded nothing looks identical to one that is working, from
    // the outside, unless the counts are visible.

    [[nodiscard]] size_t loaded_chunk_count() const { return m_loaded.size(); }
    [[nodiscard]] u32 load_requests() const { return m_load_requests; }
    [[nodiscard]] u32 unload_requests() const { return m_unload_requests; }
    [[nodiscard]] bool is_loaded(const scene::ChunkCoord& coord) const {
        return m_loaded.find(coord) != m_loaded.end();
    }

    /// Entity count currently owned by loaded chunks.
    [[nodiscard]] size_t streamed_entity_count() const;

    /// The last load error, or "" if the last load succeeded. Kept rather than
    /// logged and forgotten, so a test can assert *why* a chunk did not load.
    [[nodiscard]] const std::string& last_error() const { return m_last_error; }

    /// Chunks the current volume wants loaded, sorted. Exposed so a caller can
    /// tell "the volume is wrong" from "the streamer is wrong".
    [[nodiscard]] std::vector<scene::ChunkCoord> wanted_chunks() const;

    /// The next chunks to start loading, in dispatch order: wanted
    /// (nearest-first), skipping already-loaded chunks and `in_flight` ones
    /// (work already dispatched but not committed), up to `cap` entries.
    ///
    /// Pure with respect to threads — it only reads the wanted set, the
    /// loaded set and the given exclusion set — so an async loader can decide
    /// what to dispatch without racing its workers, and a test can pin the
    /// order without any.
    [[nodiscard]] std::vector<scene::ChunkCoord> next_wanted_loads(
        const std::unordered_set<scene::ChunkCoord>& in_flight, size_t cap) const;

    /// Upper bound on resident loaded chunks. 0 (default) = unlimited.
    ///
    /// Enforcement is farthest-first and hysteresis-aware: only chunks OUTSIDE
    /// the load radius are evictable, never must-keep ones inside it — evicting
    /// those would reload them on the next update and turn the budget into a
    /// thrash generator. Eviction flows through the unload handler and counts
    /// like any unload. When everything resident is must-keep, the budget is
    /// best-effort (over, but stable) rather than violated by force.
    void set_max_loaded_chunks(size_t max_chunks) { m_max_loaded_chunks = max_chunks; }
    [[nodiscard]] size_t max_loaded_chunks() const { return m_max_loaded_chunks; }

private:
    struct LoadedChunk {
        std::vector<ecs::Entity> entities;
    };

    LoadFn m_load;
    UnloadFn m_unload;

    scene::StreamingVolume m_volume;
    bool m_has_volume = false;

    std::unordered_map<scene::ChunkCoord, LoadedChunk> m_loaded;
    std::vector<scene::ChunkCoord> m_scratch;

    u32 m_max_loads_per_update = 4;
    u32 m_load_requests = 0;
    u32 m_unload_requests = 0;
    size_t m_max_loaded_chunks = 0;
    std::string m_last_error;
};

} // namespace nf::runtime
