#pragma once

#include <NF/Editor/Inspector.hpp>

namespace nf::editor {

// Keep unapplied shadow edits between frames, refreshing when the selected
// light or its authored shadows change (including undo/redo and reload).
struct ShadowSettingsDraft {
    float strength = 1.0f;
    float bias = 0.0005f;
    int cascades = 4;

    void sync(ecs::Entity entity, const LightEdit& live, const ecs::World* world = nullptr) {
        if (world != m_world || entity != m_entity || live.shadow_strength != m_source.shadow_strength ||
            live.shadow_bias != m_source.shadow_bias || live.shadow_cascades != m_source.shadow_cascades) {
            strength = live.shadow_strength;
            bias = live.shadow_bias;
            cascades = live.shadow_cascades;
            m_source = live;
            m_entity = entity;
            m_world = world;
        }
    }

    LightEdit merge(const LightEdit& live) const {
        LightEdit result = live;
        result.shadow_strength = strength;
        result.shadow_bias = bias;
        result.shadow_cascades = cascades;
        return result;
    }

private:
    ecs::Entity m_entity = ecs::kInvalidEntity;
    const ecs::World* m_world = nullptr;
    LightEdit m_source{};
};

} // namespace nf::editor
