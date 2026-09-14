#pragma once

#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Scene.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nf::scene {

// Prefab asset — a World that can be instantiated
class Prefab {
public:
    explicit Prefab(std::string name);

    const std::string& name() const { return m_name; }
    ecs::World& world() { return m_world; }
    const ecs::World& world() const { return m_world; }

    // For nested prefabs: a prefab can contain instances of other prefabs
    // (represented as entities with a PrefabInstanceComponent)

private:
    std::string m_name;
    ecs::World m_world;
};

// Per-instance override tracking
struct PrefabOverride {
    std::string component_name; // e.g. "Transform", "Mesh"
    std::string field_name;     // e.g. "local_x", "mesh_id"
    std::string base_value;     // serialized base
    std::string override_value; // serialized override
    bool overridden = false;
};

struct PrefabInstanceComponent {
    std::string prefab_name; // which prefab this instance came from
    ecs::Entity prefab_entity; // original entity in prefab
    std::vector<PrefabOverride> overrides;
};

// PrefabSystem handles instantiation, overrides, nesting, variants
class PrefabSystem {
public:
    // Creates a prefab asset from a scene/world snapshot
    std::shared_ptr<Prefab> create_prefab(const std::string& name, const ecs::World& source_world);

    // Instantiates a prefab into a target world, returns the root entity of the instance
    // If parent is valid, the instance root is parented to it
    ecs::Entity instantiate(const Prefab& prefab, ecs::World& target_world, ecs::Entity parent = ecs::kInvalidEntity);

    // Nested: instantiate a prefab that itself contains prefab instances (recursively)
    ecs::Entity instantiate_nested(const Prefab& prefab, ecs::World& target_world, ecs::Entity parent = ecs::kInvalidEntity);

    // Overrides
    void set_override(ecs::World& world, ecs::Entity instance_entity, const PrefabOverride& ov);
    bool has_override(const ecs::World& world, ecs::Entity e, const std::string& field) const;
    void reset_override(ecs::World& world, ecs::Entity e, const std::string& field);
    void reset_all_overrides(ecs::World& world, ecs::Entity e);
    void apply_override(ecs::World& world, ecs::Entity e); // makes override the new base

    // For variants: create a new prefab that inherits from a base prefab
    std::shared_ptr<Prefab> create_variant(const std::string& variant_name, const Prefab& base);

private:
    std::map<std::string, std::shared_ptr<Prefab>> m_prefabs;
};

} // namespace nf::scene
