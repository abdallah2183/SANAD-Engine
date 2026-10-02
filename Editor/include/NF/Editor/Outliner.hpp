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

/// What an entity IS, for the outliner's type glyph.
///
/// One value per row even though an entity can carry several components: this is
/// a priority pick, not a list. A camera that also has a mesh reads as a camera,
/// because the camera is the thing you scan a hierarchy looking for.
enum class OutlinerKind : uint8_t {
    Empty = 0, // transform only (or nothing at all)
    Mesh,
    Sky,
    Light,
    Camera,
};

struct OutlinerRow {
    ecs::Entity entity;
    int depth = 0;
    bool has_children = false;
    bool is_prefab = false; // carries a PrefabLinkComponent
    std::string label;
    OutlinerKind kind = OutlinerKind::Empty;
};

struct OutlinerState {
    // Entity ids whose children are expanded (default: all expanded).
    std::set<uint32_t> collapsed;
    // A4: there is no pending_delete field any more. Delete is immediate and
    // undoable, so there is no confirmation target to carry between frames.
    // Live search text (bound to the panel's input box). Empty = no filter.
    std::string filter_text;

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
/// Case-insensitive substring over row labels for the panel's search box.
/// Empty filter passes rows through untouched; a non-empty one returns the
/// matches flattened (depth 0) in entity-id order, so a match nested under a
/// collapsed parent is still reachable.
std::vector<OutlinerRow> filter_outliner_rows(const std::vector<OutlinerRow>& rows,
                                              const std::string& filter);
bool is_descendant_of(const ecs::World& world, ecs::Entity e, ecs::Entity ancestor);

} // namespace nf::editor
