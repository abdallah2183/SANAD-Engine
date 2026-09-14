#pragma once

#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <string>
#include <vector>
#include <unordered_map>

namespace nf::scene {

struct SceneMetadata {
    std::string name;
    std::string version = "0.1.0";
    uint64_t timestamp = 0;
};

// Scene is a container/serialization/editor layer above ECS World
class Scene {
public:
    explicit Scene(std::string name = "Untitled");

    ecs::World& world() { return m_world; }
    const ecs::World& world() const { return m_world; }

    const std::string& name() const { return m_metadata.name; }
    void set_name(const std::string& n) { m_metadata.name = n; }

    SceneMetadata& metadata() { return m_metadata; }
    const SceneMetadata& metadata() const { return m_metadata; }

    // Root entities (those with no parent) — maintained automatically, but can be queried
    std::vector<ecs::Entity> root_entities() const;

    // Serialization to/from JSON-like string (no pointers, stable IDs)
    std::string serialize() const;
    bool deserialize(const std::string& data);

    // For sub-scenes (not fully implemented, but structure exists)
    void add_subscene(std::unique_ptr<Scene> subscene);
    const std::vector<std::unique_ptr<Scene>>& subscenes() const { return m_subscenes; }

    void clear();

private:
    ecs::World m_world;
    SceneMetadata m_metadata;
    std::vector<std::unique_ptr<Scene>> m_subscenes;
};

// Asset reference — stable string id, not pointer
using AssetRef = std::string;

} // namespace nf::scene
