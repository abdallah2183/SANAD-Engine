// NF/Runtime/src/WorldStreamer.cpp — world streaming policy (Phase 11, W5)

#include <NF/Runtime/WorldStreamer.hpp>

#include <algorithm>

namespace nf::runtime {

void WorldStreamer::set_handlers(LoadFn load, UnloadFn unload) {
    m_load = std::move(load);
    m_unload = std::move(unload);
}

void WorldStreamer::set_volume(const scene::StreamingVolume& volume) {
    if (!volume.valid()) {
        // A zero chunk_size would make chunk_at divide by zero and every distance
        // meaningless. Ignoring the volume leaves the previous one active, which
        // is a visible, recoverable state; accepting it would not be.
        return;
    }
    m_volume = volume;
    m_has_volume = true;
}

std::vector<scene::ChunkCoord> WorldStreamer::wanted_chunks() const {
    std::vector<scene::ChunkCoord> wanted;
    if (!m_has_volume) return wanted;
    scene::chunks_in_radius(m_volume.center, m_volume.load_radius, m_volume.chunk_size, wanted);
    return wanted;
}

std::vector<scene::ChunkCoord> WorldStreamer::next_wanted_loads(
    const std::unordered_set<scene::ChunkCoord>& in_flight, size_t cap) const {
    std::vector<scene::ChunkCoord> out;
    if (!m_has_volume || cap == 0) return out;
    for (const scene::ChunkCoord& coord : wanted_chunks()) {
        if (out.size() >= cap) break;
        if (m_loaded.find(coord) != m_loaded.end()) continue;
        if (in_flight.find(coord) != in_flight.end()) continue;
        out.push_back(coord);
    }
    return out;
}

size_t WorldStreamer::streamed_entity_count() const {
    size_t total = 0;
    for (const auto& [coord, chunk] : m_loaded) {
        (void)coord;
        total += chunk.entities.size();
    }
    return total;
}

u32 WorldStreamer::update() {
    if (!m_has_volume) return 0;

    const f32 unload_radius = m_volume.effective_unload_radius();

    // --- Release first ------------------------------------------------------
    //
    // Before loading, not after. A volume that jumps a long way should give back
    // the memory it no longer needs in the same frame it asks for more, rather
    // than briefly holding both regions.
    for (auto it = m_loaded.begin(); it != m_loaded.end();) {
        const f32 distance = scene::distance_to_chunk(m_volume.center, it->first, m_volume.chunk_size);
        if (distance > unload_radius) {
            if (m_unload) {
                m_unload(it->first, it->second.entities);
            }
            ++m_unload_requests;
            it = m_loaded.erase(it);
        } else {
            ++it;
        }
    }

    // --- Then enforce the memory budget -------------------------------------
    //
    // Farthest-first, and only outside the load radius: chunks inside it are
    // must-keep (evicting one reloads it on the very next update). Chunks in
    // the hysteresis band are fair game — that is what the budget is for.
    // When everything resident is must-keep, the budget holds its nose and
    // stays over rather than thrashing.
    //
    // This runs even when loading is disabled (max_loads_per_update == 0):
    // eviction is unloading, and unloading never waits for loading.
    if (m_max_loaded_chunks > 0) {
        while (m_loaded.size() > m_max_loaded_chunks) {
            auto victim = m_loaded.end();
            f32 victim_distance = 0.0f;
            for (auto it = m_loaded.begin(); it != m_loaded.end(); ++it) {
                const f32 distance =
                    scene::distance_to_chunk(m_volume.center, it->first, m_volume.chunk_size);
                if (distance <= m_volume.load_radius) continue;
                if (victim == m_loaded.end() || distance > victim_distance) {
                    victim = it;
                    victim_distance = distance;
                }
            }
            if (victim == m_loaded.end()) break;
            if (m_unload) {
                m_unload(victim->first, victim->second.entities);
            }
            ++m_unload_requests;
            m_loaded.erase(victim);
        }
    }

    // --- Then load ----------------------------------------------------------
    // Disabled entirely when the cap is zero (unloading above still ran:
    // releasing memory must never wait for loading).
    if (m_max_loads_per_update == 0) return 0;

    m_scratch.clear();
    scene::chunks_in_radius(m_volume.center, m_volume.load_radius, m_volume.chunk_size, m_scratch);

    u32 loaded = 0;
    for (const scene::ChunkCoord& coord : m_scratch) {
        if (loaded >= m_max_loads_per_update) break;
        if (m_loaded.find(coord) != m_loaded.end()) continue;
        if (!m_load) break;

        std::vector<ecs::Entity> created;
        std::string error;
        ++m_load_requests;

        if (!m_load(coord, created, error)) {
            // Not fatal and not cached as failed: a chunk file may appear later,
            // and the retry costs one map lookup. The error is kept so a caller
            // can see *why* rather than only that the count stayed flat.
            m_last_error = error;
            continue;
        }

        m_last_error.clear();
        m_loaded.emplace(coord, LoadedChunk{std::move(created)});
        ++loaded;
    }

    return loaded;
}

void WorldStreamer::clear() {
    if (m_unload) {
        for (auto& [coord, chunk] : m_loaded) {
            m_unload(coord, chunk.entities);
        }
    }
    m_unload_requests += static_cast<u32>(m_loaded.size());
    m_loaded.clear();
}

} // namespace nf::runtime
