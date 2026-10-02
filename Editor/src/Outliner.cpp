#include <NF/Editor/Outliner.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cctype>
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
        // Type glyph: most specific first, so an entity that is both a camera
        // and a mesh still reads as a camera in the tree.
        OutlinerKind kind = OutlinerKind::Empty;
        if (world.has<runtime::CameraComponent>(f.e)) {
            kind = OutlinerKind::Camera;
        } else if (world.has<runtime::DirectionalLight>(f.e)) {
            kind = OutlinerKind::Light;
        } else if (world.has<runtime::SkyComponent>(f.e)) {
            kind = OutlinerKind::Sky;
        } else if (world.has<runtime::MeshComponent>(f.e)) {
            kind = OutlinerKind::Mesh;
        }
        rows.push_back(OutlinerRow{f.e, f.depth, has_children,
                                  world.has<scene::PrefabLinkComponent>(f.e),
                                  entity_label(world, f.e), kind});
        if (has_children && state.is_expanded(f.e)) {
            for (auto it = cit->second.rbegin(); it != cit->second.rend(); ++it) {
                stack.push_back(Frame{*it, f.depth + 1});
            }
        }
    }
    return rows;
}

std::vector<OutlinerRow> filter_outliner_rows(const std::vector<OutlinerRow>& rows,
                                              const std::string& filter) {
    if (filter.empty()) {
        return rows;
    }
    std::string needle = filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<OutlinerRow> out;
    for (const OutlinerRow& row : rows) {
        std::string hay = row.label;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (hay.find(needle) != std::string::npos) {
            OutlinerRow flat = row;
            flat.depth = 0;
            out.push_back(flat);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const OutlinerRow& a, const OutlinerRow& b) { return a.entity.id < b.entity.id; });
    return out;
}

} // namespace nf::editor
