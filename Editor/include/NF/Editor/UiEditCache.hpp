#pragma once

#include <NF/ECS/ECS.hpp>
#include <cstdint>

namespace nf::editor {

// A UI draft survives idle frames. Scene replacement, a command, or undo/redo
// invalidates it even if the entity id and generation happen to be reused.
struct UiEditCacheKey {
    bool same_scene(const ecs::World* world, uint64_t scene) const {
        return m_valid && m_world == world && m_scene == scene;
    }
    bool refresh(const ecs::World* world, ecs::Entity entity, uint64_t scene, uint64_t revision) {
        if (m_valid && m_world == world && m_entity == entity && m_scene == scene && m_revision == revision)
            return false;
        m_valid = true;
        m_world = world;
        m_entity = entity;
        m_scene = scene;
        m_revision = revision;
        return true;
    }
private:
    bool m_valid = false;
    const ecs::World* m_world = nullptr;
    ecs::Entity m_entity = ecs::kInvalidEntity;
    uint64_t m_scene = 0, m_revision = 0;
};

} // namespace nf::editor
