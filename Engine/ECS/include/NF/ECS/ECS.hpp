#pragma once

// NF/ECS/ECS.hpp — Data-oriented ECS (World/Entity/ComponentStorage/Query/System/Schedule)
// Design: Entity ID + Generation, Component Type ID, SparseSet storage, Archetype-ish Query

#include <NF/ECS/Entity.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Core/Assert.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace nf::ecs {

// Component Type Registry
using ComponentTypeId = u32;
static constexpr ComponentTypeId kInvalidComponentId = u32_max;

struct ComponentInfo {
    std::string name;
    size_t size = 0;
    size_t alignment = 0;
};

class ComponentRegistry {
public:
    static ComponentRegistry& instance();

    template<typename T>
    ComponentTypeId register_component(const std::string& name) {
        std::string key = name.empty() ? typeid(T).name() : name;
        auto it = m_name_to_id.find(key);
        if (it != m_name_to_id.end()) return it->second;
        ComponentTypeId id = m_next_id++;
        m_name_to_id[key] = id;
        m_id_to_info[id] = {name, sizeof(T), alignof(T)};
        // Also map type_index for get_id<T> without name
        m_type_to_id[std::type_index(typeid(T))] = id;
        return id;
    }

    template<typename T>
    ComponentTypeId get_id() const {
        auto it = m_type_to_id.find(std::type_index(typeid(T)));
        return it != m_type_to_id.end() ? it->second : kInvalidComponentId;
    }

    ComponentTypeId get_id_by_name(const std::string& name) const;
    const ComponentInfo* get_info(ComponentTypeId id) const;
    std::string get_name(ComponentTypeId id) const;

private:
    ComponentRegistry() = default;
    std::map<std::string, ComponentTypeId> m_name_to_id;
    std::map<std::type_index, ComponentTypeId> m_type_to_id;
    std::map<ComponentTypeId, ComponentInfo> m_id_to_info;
    ComponentTypeId m_next_id = 0;
};

template<typename T>
inline ComponentTypeId component_type_id() {
    static ComponentTypeId id = ComponentRegistry::instance().register_component<T>(typeid(T).name());
    return id;
}

// Forward
class World;

// IComponentStorage — type-erased storage
class IComponentStorage {
public:
    virtual ~IComponentStorage() = default;
    virtual bool has(Entity e) const = 0;
    virtual void remove(Entity e) = 0;
    virtual void clear() = 0;
    virtual size_t size() const = 0;
    virtual ComponentTypeId type_id() const = 0;
};

// SparseSet storage for component T
template<typename T>
class ComponentStorage : public IComponentStorage {
public:
    explicit ComponentStorage(ComponentTypeId tid) : m_type_id(tid) {}

    bool has(Entity e) const override {
        if (e.id >= m_sparse.size()) return false;
        int dense_idx = m_sparse[e.id];
        return dense_idx != -1 && dense_idx < static_cast<int>(m_dense_entities.size()) && m_dense_entities[dense_idx] == e;
    }

    T* get(Entity e) {
        if (!has(e)) return nullptr;
        return &m_dense_components[m_sparse[e.id]];
    }

    const T* get(Entity e) const {
        if (!has(e)) return nullptr;
        return &m_dense_components[m_sparse[e.id]];
    }

    T& add(Entity e, const T& value = T{}) {
        NF_ASSERT(e.valid(), "add: invalid entity");
        if (has(e)) {
            m_dense_components[m_sparse[e.id]] = value;
            return m_dense_components[m_sparse[e.id]];
        }
        if (e.id >= m_sparse.size()) m_sparse.resize(e.id + 1, -1);
        int idx = static_cast<int>(m_dense_entities.size());
        m_sparse[e.id] = idx;
        m_dense_entities.push_back(e);
        m_dense_components.push_back(value);
        return m_dense_components.back();
    }

    void remove(Entity e) override {
        if (!has(e)) return;
        int idx = m_sparse[e.id];
        int last = static_cast<int>(m_dense_entities.size()) - 1;
        if (idx != last) {
            Entity last_e = m_dense_entities[last];
            m_dense_entities[idx] = last_e;
            m_dense_components[idx] = std::move(m_dense_components[last]);
            m_sparse[last_e.id] = idx;
        }
        m_dense_entities.pop_back();
        m_dense_components.pop_back();
        m_sparse[e.id] = -1;
    }

    void clear() override {
        m_dense_entities.clear();
        m_dense_components.clear();
        std::fill(m_sparse.begin(), m_sparse.end(), -1);
    }

    size_t size() const override { return m_dense_entities.size(); }
    ComponentTypeId type_id() const override { return m_type_id; }

    // For queries: iterate over dense
    const std::vector<Entity>& entities() const { return m_dense_entities; }
    std::vector<Entity>& entities() { return m_dense_entities; }
    const std::vector<T>& components() const { return m_dense_components; }
    std::vector<T>& components() { return m_dense_components; }

private:
    ComponentTypeId m_type_id = kInvalidComponentId;
    std::vector<Entity> m_dense_entities;
    std::vector<T> m_dense_components;
    std::vector<int> m_sparse; // entity id -> dense index, -1 if none
};

// World
class World {
public:
    World() = default;
    ~World() = default;

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = default;
    World& operator=(World&&) = default;

    Entity create_entity();
    void destroy_entity(Entity e);
    bool is_alive(Entity e) const;

    template<typename T, typename... Args>
    T& add(Entity e, Args&&... args) {
        NF_ASSERT(is_alive(e), "add: entity not alive");
        auto* storage = get_or_create_storage<T>();
        T value(std::forward<Args>(args)...);
        return storage->add(e, value);
    }

    template<typename T>
    void remove(Entity e) {
        auto* storage = get_storage<T>();
        if (storage) storage->remove(e);
    }

    template<typename T>
    bool has(Entity e) const {
        auto* storage = get_storage<T>();
        return storage ? storage->has(e) : false;
    }

    template<typename T>
    T* get(Entity e) {
        auto* storage = get_storage<T>();
        return storage ? storage->get(e) : nullptr;
    }

    template<typename T>
    const T* get(Entity e) const {
        auto* storage = get_storage<T>();
        return storage ? storage->get(e) : nullptr;
    }

    // Query: returns entities that have all of Ts
    template<typename... Ts>
    std::vector<Entity> query() const {
        std::vector<Entity> result;
        if constexpr (sizeof...(Ts) == 0) return result;
        // Find smallest storage to iterate
        const IComponentStorage* smallest = nullptr;
        size_t min_size = SIZE_MAX;
        // Use fold to find smallest
        auto check = [&](auto* dummy) {
            using T = std::decay_t<decltype(*dummy)>;
            auto* s = get_storage<T>();
            if (s && s->size() < min_size) {
                min_size = s->size();
                smallest = s;
            }
        };
        (check(static_cast<Ts*>(nullptr)), ...);
        if (!smallest) return result;

        // Find which type is smallest to iterate
        // For simplicity, just iterate over first type's storage and check others
        using First = std::tuple_element_t<0, std::tuple<Ts...>>;
        auto* first_storage = get_storage<First>();
        if (!first_storage) return result;
        for (Entity e : first_storage->entities()) {
            if (!is_alive(e)) continue;
            bool ok = (has<Ts>(e) && ...);
            if (ok) result.push_back(e);
        }
        return result;
    }

    template<typename T>
    std::vector<Entity> query_with() const {
        return query<T>();
    }

    size_t entity_count() const;
    size_t alive_entity_count() const;

    void clear();

    // For serialization / iteration
    std::vector<Entity> all_entities() const;

private:
    template<typename T>
    ComponentStorage<T>* get_or_create_storage() {
        ComponentTypeId tid = component_type_id<T>();
        auto it = m_storages.find(tid);
        if (it != m_storages.end()) return static_cast<ComponentStorage<T>*>(it->second.get());
        auto storage = std::make_unique<ComponentStorage<T>>(tid);
        auto* raw = storage.get();
        m_storages[tid] = std::move(storage);
        return raw;
    }

    template<typename T>
    ComponentStorage<T>* get_storage() {
        ComponentTypeId tid = component_type_id<T>();
        auto it = m_storages.find(tid);
        return it != m_storages.end() ? static_cast<ComponentStorage<T>*>(it->second.get()) : nullptr;
    }

    template<typename T>
    const ComponentStorage<T>* get_storage() const {
        ComponentTypeId tid = component_type_id<T>();
        auto it = m_storages.find(tid);
        return it != m_storages.end() ? static_cast<const ComponentStorage<T>*>(it->second.get()) : nullptr;
    }

    struct EntityEntry { u32 generation = 0; bool alive = false; };
    std::vector<EntityEntry> m_entities;
    std::vector<u32> m_free_list;
    std::map<ComponentTypeId, std::unique_ptr<IComponentStorage>> m_storages;
};

// System
struct System {
    std::string name;
    std::vector<std::string> depends_on;
    std::function<void(World&)> fn;
};

// Scheduler with dependency graph
class SystemScheduler {
public:
    void add_system(System sys);
    void execute(World& world); // sequential in dependency order (parallelism can be added via Jobs)
    void clear() { m_systems.clear(); }

private:
    std::vector<System> m_systems;
    bool build_order(std::vector<size_t>& order) const;
};

} // namespace nf::ecs
