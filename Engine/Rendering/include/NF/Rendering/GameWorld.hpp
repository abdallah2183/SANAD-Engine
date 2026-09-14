#pragma once

// NF/Rendering/GameWorld.hpp — Minimal Game World for extraction tests
//
// This is NOT the full ECS — it's a minimal stand-in that lets us prove
// the extraction architecture: Game Entity has gameplay components that must
// NOT reach the Render World.

#include <NF/Core/Types.hpp>
#include <NF/Rendering/RenderWorld.hpp>

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace nf::game {

struct Transform {
    float x = 0, y = 0, z = 0;
};

// Gameplay-only components (must NOT reach Render World)
struct Health { int hp = 100; };
struct Inventory { std::vector<std::string> items; };
struct QuestState { std::string quest_id; };

// Rendering-relevant components
struct MeshComponent { std::string mesh_id; };
struct MaterialComponent { std::string material_name; };

struct GameEntity {
    u32 id = 0;
    Transform transform;
    std::optional<MeshComponent> mesh;
    std::optional<MaterialComponent> material;
    bool visible = true;

    // Gameplay-only
    std::optional<Health> health;
    std::optional<Inventory> inventory;
    std::optional<QuestState> quest;
};

struct GameWorld {
    // Use deque for stable references (vector would invalidate on reallocation)
    std::deque<GameEntity> entities;

    GameEntity& create_entity() {
        GameEntity e;
        e.id = static_cast<u32>(entities.size() + 1);
        entities.push_back(std::move(e));
        return entities.back();
    }

    void clear() { entities.clear(); }
    size_t size() const { return entities.size(); }
};

} // namespace nf::game
