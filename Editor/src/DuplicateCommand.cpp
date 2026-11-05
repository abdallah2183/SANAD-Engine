#include <NF/Editor/Commands.hpp>
#include <NF/Editor/SceneCopy.hpp>

#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace nf::editor {
namespace {

bool has_selected_ancestor(const ecs::World& world, ecs::Entity e,
                           const std::unordered_set<uint32_t>& selected) {
    std::unordered_set<uint32_t> visited;
    ecs::Entity p = scene::get_parent(world, e);
    while (p.valid() && world.is_alive(p) && visited.insert(p.id).second) {
        if (selected.count(p.id) != 0) {
            return true;
        }
        p = scene::get_parent(world, p);
    }
    return false;
}

ecs::Entity copy_subtree(const ecs::World& src, ecs::Entity from, ecs::World& dst,
                         ecs::Entity parent, std::vector<ecs::Entity>* created = nullptr) {
    ecs::Entity copy = dst.create_entity();
    if (created) created->push_back(copy);
    copy_entity_components(src, from, dst, copy);
    if (auto* t = dst.get<scene::Transform>(copy)) {
        t->parent = parent;
        t->dirty = true;
    }
    for (ecs::Entity child : scene::get_children(src, from)) {
        copy_subtree(src, child, dst, copy, created);
    }
    return copy;
}

std::string unique_copy_name(const ecs::World& world, const std::string& base) {
    std::unordered_set<std::string> names;
    for (ecs::Entity e : world.all_entities()) {
        if (const auto* n = world.get<scene::NameComponent>(e)) {
            names.insert(n->name);
        }
    }
    for (int i = 1; i < 10000; ++i) {
        const std::string candidate = base + " (" + std::to_string(i) + ")";
        if (names.count(candidate) == 0) {
            return candidate;
        }
    }
    return base + " (Copy)";
}

} // namespace

std::unique_ptr<DuplicateEntitiesCommand> DuplicateEntitiesCommand::capture(
    const scene::Scene& scene, const std::vector<ecs::Entity>& selected, std::string& out_err) {
    const ecs::World& world = scene.world();
    std::vector<ecs::Entity> alive;
    std::unordered_set<uint32_t> selected_ids;
    for (ecs::Entity e : selected) {
        if (e.valid() && world.is_alive(e) && selected_ids.insert(e.id).second) {
            alive.push_back(e);
        }
    }
    if (alive.empty()) {
        out_err = "Select at least one entity to duplicate";
        return nullptr;
    }

    auto cmd = std::unique_ptr<DuplicateEntitiesCommand>(new DuplicateEntitiesCommand());
    cmd->m_snapshot = std::make_unique<scene::Scene>("Duplicate");
    ecs::World& snapshot = cmd->m_snapshot->world();

    for (ecs::Entity e : alive) {
        if (has_selected_ancestor(world, e, selected_ids)) {
            continue; // already part of a selected ancestor's subtree
        }
        Root root;
        root.parent = scene::get_parent(world, e);
        if (root.parent.valid() && !world.is_alive(root.parent)) {
            root.parent = ecs::kInvalidEntity;
        }
        root.snapshot = copy_subtree(world, e, snapshot, ecs::kInvalidEntity);
        cmd->m_roots.push_back(root);
    }
    if (cmd->m_roots.empty()) {
        out_err = "Selection contains no duplicable root";
        return nullptr;
    }
    return cmd;
}

void DuplicateEntitiesCommand::apply(ecs::World& world) {
    m_created_roots.clear();
    m_created_all.clear();
    m_created_all.reserve(16);

    for (const Root& root : m_roots) {
        if (m_snapshot == nullptr || !m_snapshot->world().is_alive(root.snapshot)) {
            continue;
        }
        ecs::Entity parent = root.parent;
        if (parent.valid() && !world.is_alive(parent)) {
            parent = ecs::kInvalidEntity;
        }
        const ecs::Entity copy =
            copy_subtree(m_snapshot->world(), root.snapshot, world, parent, &m_created_all);
        if (auto* t = world.get<scene::Transform>(copy)) {
            // A duplicate exactly on top of the original reads as "nothing
            // happened". One step right is enough to see it and to grab it.
            t->local_x += 0.5f;
            t->dirty = true;
        }
        if (auto* n = world.get<scene::NameComponent>(copy)) {
            n->name = unique_copy_name(world, n->name);
        }
        m_created_roots.push_back(copy);
    }
    scene::propagate_transforms(world);
}

void DuplicateEntitiesCommand::undo(ecs::World& world) {
    // Children were appended after their parents, so reverse order destroys a
    // child before its parent and never leaves a dangling parent link.
    m_previous_created_all = m_created_all;
    for (auto it = m_created_all.rbegin(); it != m_created_all.rend(); ++it) {
        if (it->valid() && world.is_alive(*it)) {
            world.destroy_entity(*it);
        }
    }
    m_created_all.clear();
    m_created_roots.clear();
    scene::propagate_transforms(world);
}

void DuplicateEntitiesCommand::remap_entity(ecs::Entity old_entity, ecs::Entity new_entity) {
    for (auto& root : m_roots) if (root.parent == old_entity) root.parent = new_entity;
    for (auto& entity : m_created_roots) if (entity == old_entity) entity = new_entity;
    for (auto& entity : m_created_all) if (entity == old_entity) entity = new_entity;
}

void DuplicateEntitiesCommand::collect_redo_remaps(
    ecs::Entity, std::vector<std::pair<ecs::Entity, ecs::Entity>>& out) const {
    const size_t count = std::min(m_previous_created_all.size(), m_created_all.size());
    for (size_t i = 0; i < count; ++i) {
        if (m_previous_created_all[i].valid() && m_created_all[i].valid() &&
            m_previous_created_all[i] != m_created_all[i])
            out.emplace_back(m_previous_created_all[i], m_created_all[i]);
    }
}

} // namespace nf::editor
