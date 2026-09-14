#include <NF/Editor/Outliner.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <vector>

namespace nf::editor {

std::string entity_label(const ecs::World& world, ecs::Entity e) {
    if (const auto* n = world.get<scene::NameComponent>(e)) {
        if (!n->name.empty()) {
            return n->name;
        }
    }
    return "Entity " + std::to_string(e.id);
}

bool is_descendant_of(const ecs::World& world, ecs::Entity e, ecs::Entity ancestor) {
    if (!e.valid() || !ancestor.valid()) {
        return false;
    }
    std::set<uint32_t> visited;
    ecs::Entity cur = e;
    while (cur.valid()) {
        if (visited.count(cur.id) != 0) {
            break;
        }
        visited.insert(cur.id);
        const auto* t = world.get<scene::Transform>(cur);
        if (t == nullptr || !t->parent.valid()) {
            break;
        }
        if (t->parent == ancestor) {
            return true;
        }
        cur = t->parent;
    }
    return false;
}

std::vector<OutlinerRow> build_outliner_rows(const ecs::World& world, const OutlinerState& state) {
    std::vector<OutlinerRow> rows;
    const std::vector<ecs::Entity> all = world.all_entities();
    if (all.empty()) {
        return rows;
    }
    // Children lookup rebuilt from live parent links every call, so there are
    // no stale child lists: parent id -> sorted children.
    std::vector<ecs::Entity> roots;
    std::unordered_map<uint32_t, std::vector<ecs::Entity>> children;
    for (ecs::Entity e : all) {
        const auto* t = world.get<scene::Transform>(e);
        if (t != nullptr && t->parent.valid() && world.is_alive(t->parent)) {
            children[t->parent.id].push_back(e);
        } else {
            roots.push_back(e);
        }
    }
    auto by_id = [](ecs::Entity a, ecs::Entity b) { return a.id < b.id; };
    std::sort(roots.begin(), roots.end(), by_id);
    for (auto& kv : children) {
        std::sort(kv.second.begin(), kv.second.end(), by_id);
    }
    // Iterative DFS.
    struct Frame {
        ecs::Entity e;
        int depth;
    };
    std::vector<Frame> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        stack.push_back(Frame{*it, 0});
    }
    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();
        auto cit = children.find(f.e.id);
        const bool has_children = (cit != children.end() && !cit->second.empty());
        rows.push_back(OutlinerRow{f.e, f.depth, has_children,
                                  world.has<scene::PrefabLinkComponent>(f.e),
                                  entity_label(world, f.e)});
        if (has_children && state.is_expanded(f.e)) {
            for (auto it = cit->second.rbegin(); it != cit->second.rend(); ++it) {
                stack.push_back(Frame{*it, f.depth + 1});
            }
        }
    }
    return rows;
}

} // namespace nf::editor
