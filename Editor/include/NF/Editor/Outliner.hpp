#pragma once

// NF/Editor/Outliner.hpp — scene hierarchy model for the Scene Outliner panel.
//
// Rows are built from Transform parent/children links (DFS from roots sorted
// by entity id), labeled via NameComponent with an "Entity <id>" fallback.
// Entities without Transform appear as roots. Collapse/expand state lives in
// OutlinerState (keyed by entity id); deleted entities never linger because
// rows are rebuilt from the live world every frame.

#include <NF/ECS/ECS.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace nf::editor {

struct OutlinerRow {
    ecs::Entity entity;
    int depth = 0;
    bool has_children = false;
    bool is_prefab = false; // carries a PrefabLinkComponent
    std::string label;
};

struct OutlinerState {
    // Entity ids whose children are expanded (default: all expanded).
    std::set<uint32_t> collapsed;
    // Inline-delete confirmation target (kInvalidEntity when none pending).
    ecs::Entity pending_delete = ecs::kInvalidEntity;

    bool is_expanded(ecs::Entity e) const { return collapsed.count(e.id) == 0; }
    void set_expanded(ecs::Entity e, bool expanded) {
        if (expanded) {
            collapsed.erase(e.id);
        } else {
            collapsed.insert(e.id);
        }
    }
};

std::string entity_label(const ecs::World& world, ecs::Entity e);
std::vector<OutlinerRow> build_outliner_rows(const ecs::World& world, const OutlinerState& state);
bool is_descendant_of(const ecs::World& world, ecs::Entity e, ecs::Entity ancestor);

} // namespace nf::editor
