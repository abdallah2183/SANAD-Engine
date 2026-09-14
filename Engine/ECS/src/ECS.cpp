#include <NF/ECS/ECS.hpp>

#include <NF/Core/Logger.hpp>

#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nf::ecs {

// ComponentRegistry
ComponentRegistry& ComponentRegistry::instance() {
    static ComponentRegistry reg;
    return reg;
}

ComponentTypeId ComponentRegistry::get_id_by_name(const std::string& name) const {
    for (auto& [id, info] : m_id_to_info) {
        if (info.name == name) return id;
    }
    return kInvalidComponentId;
}

const ComponentInfo* ComponentRegistry::get_info(ComponentTypeId id) const {
    auto it = m_id_to_info.find(id);
    return it != m_id_to_info.end() ? &it->second : nullptr;
}

std::string ComponentRegistry::get_name(ComponentTypeId id) const {
    auto it = m_id_to_info.find(id);
    return it != m_id_to_info.end() ? it->second.name : "";
}

// World
Entity World::create_entity() {
    u32 id;
    u32 gen;
    if (!m_free_list.empty()) {
        id = m_free_list.back();
        m_free_list.pop_back();
        EntityEntry& entry = m_entities[id];
        // generation already incremented on destroy, keep it
        entry.alive = true;
        gen = entry.generation;
    } else {
        id = static_cast<u32>(m_entities.size());
        m_entities.push_back({0, true});
        gen = 0;
    }
    return Entity{id, gen};
}

void World::destroy_entity(Entity e) {
    if (!is_alive(e)) return;
    // Remove all components
    for (auto& [tid, storage] : m_storages) {
        if (storage->has(e)) storage->remove(e);
    }
    EntityEntry& entry = m_entities[e.id];
    entry.alive = false;
    entry.generation++;
    m_free_list.push_back(e.id);
}

bool World::is_alive(Entity e) const {
    if (e.id >= m_entities.size()) return false;
    const EntityEntry& entry = m_entities[e.id];
    return entry.alive && entry.generation == e.generation;
}

size_t World::entity_count() const {
    return m_entities.size();
}

size_t World::alive_entity_count() const {
    size_t c = 0;
    for (auto& e : m_entities) if (e.alive) ++c;
    return c;
}

std::vector<Entity> World::all_entities() const {
    std::vector<Entity> out;
    out.reserve(alive_entity_count());
    for (u32 i = 0; i < m_entities.size(); ++i) {
        if (m_entities[i].alive) out.push_back(Entity{i, m_entities[i].generation});
    }
    return out;
}

void World::clear() {
    for (auto& [tid, storage] : m_storages) storage->clear();
    m_entities.clear();
    m_free_list.clear();
}

// SystemScheduler
void SystemScheduler::add_system(System sys) {
    m_systems.push_back(std::move(sys));
}

bool SystemScheduler::build_order(std::vector<size_t>& order) const {
    size_t n = m_systems.size();
    order.clear();
    if (n == 0) return true;
    auto find_idx = [&](const std::string& name) -> int {
        for (size_t i = 0; i < n; ++i) if (m_systems[i].name == name) return static_cast<int>(i);
        return -1;
    };

    std::vector<std::vector<size_t>> adj(n);
    std::vector<int> indegree(n, 0);
    for (size_t i = 0; i < n; ++i) {
        for (auto& dep : m_systems[i].depends_on) {
            int dep_idx = find_idx(dep);
            if (dep_idx < 0) {
                NF_LOG_WARN(LogCategory::Core, "System '{}' depends on unknown system '{}'", m_systems[i].name, dep);
                continue;
            }
            adj[static_cast<size_t>(dep_idx)].push_back(i);
            indegree[i]++;
        }
    }
    std::queue<size_t> q;
    for (size_t i = 0; i < n; ++i) if (indegree[i]==0) q.push(i);
    while (!q.empty()) {
        size_t u = q.front(); q.pop();
        order.push_back(u);
        for (size_t v : adj[u]) if (--indegree[v]==0) q.push(v);
    }
    return order.size() == n;
}

void SystemScheduler::execute(World& world) {
    std::vector<size_t> order;
    if (!build_order(order)) {
        NF_LOG_ERROR(LogCategory::Core, "SystemScheduler: cycle detected, executing in insertion order");
        order.clear();
        for (size_t i = 0; i < m_systems.size(); ++i) order.push_back(i);
    }
    for (size_t idx : order) {
        auto& sys = m_systems[idx];
        if (sys.fn) {
            NF_LOG_TRACE(LogCategory::Core, "Executing system '{}'", sys.name);
            sys.fn(world);
        }
    }
}

} // namespace nf::ecs
