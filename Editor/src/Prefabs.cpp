// NF/Editor/Prefabs.cpp — subtree clone helpers (see header).

#include <NF/Editor/Prefabs.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>

#include <unordered_map>

namespace nf::editor {

std::vector<ecs::Entity> collect_subtree(const ecs::World& world, ecs::Entity root) {
    std::vector<ecs::Entity> out;
    if (!root.valid() || !world.is_alive(root)) {
        return out;
    }
    // Iterative pre-order DFS (parent before children).
    std::vector<ecs::Entity> stack{root};
    while (!stack.empty()) {
        ecs::Entity cur = stack.back();
        stack.pop_back();
        if (!world.is_alive(cur)) {
            continue;
        }
        out.push_back(cur);
        auto kids = scene::get_children(world, cur);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    return out;
}

namespace {

void copy_components(const ecs::World& src, ecs::Entity se, ecs::World& dst, ecs::Entity de) {
    // The component list itself lives in the runtime scene loader, next to
    // the parser: prefab cloning and scene merging share it, so a newly
    // parsed component type cannot reach one consumer but not the other.
    // Only the clone-specific fixup stays here: a clone starts parentless
    // (the remap pass below re-parents it) with a dirty transform.
    runtime::copy_scene_entity(src, se, dst, de);
    if (dst.get<scene::Transform>(de) != nullptr) {
        dst.get<scene::Transform>(de)->parent = ecs::kInvalidEntity;
        dst.get<scene::Transform>(de)->dirty = true;
    }
}

} // namespace

ecs::Entity clone_subtree(const ecs::World& src, ecs::Entity root, ecs::World& dst,
                           ecs::Entity dst_parent) {
    const std::vector<ecs::Entity> members = collect_subtree(src, root);
    if (members.empty()) {
        return ecs::kInvalidEntity;
    }
    std::unordered_map<uint32_t, ecs::Entity> remap;
    ecs::Entity new_root;
    bool first = true;
    for (ecs::Entity se : members) {
        ecs::Entity de = dst.create_entity();
        remap[se.id] = de;
        copy_components(src, se, dst, de);
        if (first) {
            new_root = de;
            first = false;
        }
    }
    // Remap internal parents; the new root goes under dst_parent.
    for (ecs::Entity se : members) {
        const auto* st = src.get<scene::Transform>(se);
        if (st == nullptr) {
            continue;
        }
        ecs::Entity de = remap[se.id];
        if (se == root) {
            if (dst_parent.valid() && dst.is_alive(dst_parent)) {
                scene::set_parent(dst, de, dst_parent);
            }
            continue;
        }
        if (st->parent.valid()) {
            auto it = remap.find(st->parent.id);
            if (it != remap.end()) {
                scene::set_parent(dst, de, it->second);
            }
        }
    }
    scene::propagate_transforms(dst);
    return new_root;
}

std::vector<ecs::Entity> prefab_template_roots(const scene::Scene& prefab) {
    std::vector<ecs::Entity> roots;
    const ecs::World& w = prefab.world();
    for (ecs::Entity e : w.all_entities()) {
        const auto* t = w.get<scene::Transform>(e);
        if (t == nullptr || !t->parent.valid() || !w.is_alive(t->parent)) {
            roots.push_back(e);
        }
    }
    return roots;
}

std::vector<ecs::Entity> clone_prefab_roots(const scene::Scene& prefab, ecs::World& dst,
                                            ecs::Entity dst_parent) {
    std::vector<ecs::Entity> created;
    for (ecs::Entity r : prefab_template_roots(prefab)) {
        ecs::Entity ne = clone_subtree(prefab.world(), r, dst, dst_parent);
        if (ne.valid()) {
            created.push_back(ne);
        }
    }
    return created;
}

} // namespace nf::editor
