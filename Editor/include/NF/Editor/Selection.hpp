#pragma once

// NF/Editor/Selection.hpp — entity selection (single required, multi optional).
//
// Selection is identity-based (Entity id + generation), never index-based: a
// stale handle simply stops matching after the entity dies or is recycled.
// prune() drops dead handles; editors call it after every structural change
// (delete/undo/load) so a deleted entity is never left selected.

#include <NF/ECS/ECS.hpp>

#include <vector>

namespace nf::editor {

class Selection {
public:
    Selection() = default;

    void set_single(ecs::Entity e);
    void add(ecs::Entity e);
    void remove(ecs::Entity e);
    void clear();

    bool has_selection() const { return m_primary.valid(); }
    ecs::Entity primary() const { return m_primary; }
    const std::vector<ecs::Entity>& all() const { return m_multi; }

    bool contains(ecs::Entity e) const;

    // Drops handles whose entity is no longer alive (deleted or recycled).
    void prune(const ecs::World& world);

private:
    ecs::Entity m_primary = ecs::kInvalidEntity;
    std::vector<ecs::Entity> m_multi;
};

} // namespace nf::editor
