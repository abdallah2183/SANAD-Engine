#include <NF/Editor/PlayMode.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>

#include <unordered_map>
#include <vector>

namespace nf::editor {

std::unique_ptr<scene::Scene> clone_scene(const scene::Scene& src) {
    auto dst = std::make_unique<scene::Scene>(src.name());
    dst->metadata() = src.metadata();
    const ecs::World& sw = src.world();
    ecs::World& dw = dst->world();
    // Pass 1: create one entity per source entity, copy components.
    // v0.1 used to copy only Transform/Name/Mesh/Camera/Light, so a play
    // session silently dropped physics, animation, audio, gameplay, sky and
    // destruction — Play simulated "the scene as it was on disk minus
    // everything that moves". Every scene component is copied now; the live
    // physics body handle is reset (per-session, rebuilt by the runtime).
    std::unordered_map<uint32_t, ecs::Entity> remap;
    for (ecs::Entity se : sw.all_entities()) {
        ecs::Entity de = dw.create_entity();
        remap[se.id] = de;
        if (const auto* t = sw.get<scene::Transform>(se)) {
            dw.add<scene::Transform>(de, *t);
            // Parent fixed in pass 2 (source ids are meaningless here).
            dw.get<scene::Transform>(de)->parent = ecs::kInvalidEntity;
        }
        if (const auto* n = sw.get<scene::NameComponent>(se)) {
            dw.add<scene::NameComponent>(de, *n);
        }
        if (const auto* m = sw.get<runtime::MeshComponent>(se)) {
            dw.add<runtime::MeshComponent>(de, *m);
        }
        if (const auto* c = sw.get<runtime::CameraComponent>(se)) {
            dw.add<runtime::CameraComponent>(de, *c);
        }
        if (const auto* l = sw.get<runtime::DirectionalLight>(se)) {
            dw.add<runtime::DirectionalLight>(de, *l);
        }
        if (const auto* s = sw.get<runtime::SkyComponent>(se)) {
            dw.add<runtime::SkyComponent>(de, *s);
        }
        if (const auto* p = sw.get<scene::PrefabLinkComponent>(se)) {
            dw.add<scene::PrefabLinkComponent>(de, *p);
        }
        if (const auto* rb = sw.get<physics::RigidBodyComponent>(se)) {
            physics::RigidBodyComponent fresh = *rb;
            fresh.body = physics::BodyHandle{};
            dw.add<physics::RigidBodyComponent>(de, fresh);
        }
        if (const auto* col = sw.get<physics::ColliderComponent>(se)) {
            dw.add<physics::ColliderComponent>(de, *col);
        }
        if (const auto* d = sw.get<runtime::DestructibleComponent>(se)) {
            dw.add<runtime::DestructibleComponent>(de, *d);
        }
        if (const auto* a = sw.get<animation::AnimationComponent>(se)) {
            dw.add<animation::AnimationComponent>(de, *a);
        }
        if (const auto* au = sw.get<audio::AudioComponent>(se)) {
            dw.add<audio::AudioComponent>(de, *au);
        }
        if (const auto* g = sw.get<gameplay::GameplayModuleComponent>(se)) {
            dw.add<gameplay::GameplayModuleComponent>(de, *g);
        }
    }
    // Pass 2: remap parents.
    for (ecs::Entity se : sw.all_entities()) {
        const auto* st = sw.get<scene::Transform>(se);
        if (st == nullptr || !st->parent.valid()) {
            continue;
        }
        auto dit = remap.find(se.id);
        auto pit = remap.find(st->parent.id);
        if (dit == remap.end() || pit == remap.end()) {
            continue;
        }
        // Generation check: only remap when the source parent link is live.
        if (!sw.is_alive(st->parent)) {
            continue;
        }
        scene::set_parent(dw, dit->second, pit->second);
    }
    scene::propagate_transforms(dw);
    return dst;
}

namespace {

std::string signature_of(const ecs::World& w, ecs::Entity e) {
    std::string s;
    s += w.has<scene::Transform>(e) ? "T" : "-";
    s += w.has<scene::NameComponent>(e) ? "N" : "-";
    s += w.has<runtime::MeshComponent>(e) ? "M" : "-";
    s += w.has<runtime::CameraComponent>(e) ? "C" : "-";
    s += w.has<runtime::DirectionalLight>(e) ? "L" : "-";
    s += w.has<scene::PrefabLinkComponent>(e) ? "P" : "-";
    s += w.has<runtime::SkyComponent>(e) ? "S" : "-";
    s += w.has<physics::RigidBodyComponent>(e) ? "R" : "-";
    s += w.has<physics::ColliderComponent>(e) ? "K" : "-";
    s += w.has<runtime::DestructibleComponent>(e) ? "D" : "-";
    s += w.has<animation::AnimationComponent>(e) ? "A" : "-";
    s += w.has<audio::AudioComponent>(e) ? "U" : "-";
    s += w.has<gameplay::GameplayModuleComponent>(e) ? "G" : "-";
    return s;
}

} // namespace

bool scenes_equal_structure(const scene::Scene& a, const scene::Scene& b, std::string& out_diff) {
    const auto alist = a.world().all_entities();
    const auto blist = b.world().all_entities();
    if (alist.size() != blist.size()) {
        out_diff = "entity count differs";
        return false;
    }
    // Index b by name (names are unique in editor-managed scenes).
    std::unordered_map<std::string, ecs::Entity> by_name_b;
    for (ecs::Entity e : blist) {
        const auto* n = b.world().get<scene::NameComponent>(e);
        const std::string key = (n != nullptr && !n->name.empty()) ? n->name : ("#"+std::to_string(e.id));
        by_name_b[key] = e;
    }
    for (ecs::Entity ea : alist) {
        const auto* na = a.world().get<scene::NameComponent>(ea);
        const std::string key =
            (na != nullptr && !na->name.empty()) ? na->name : ("#" + std::to_string(ea.id));
        auto it = by_name_b.find(key);
        if (it == by_name_b.end()) {
            out_diff = "entity '" + key + "' missing in second scene";
            return false;
        }
        ecs::Entity eb = it->second;
        if (signature_of(a.world(), ea) != signature_of(b.world(), eb)) {
            out_diff = "component set differs for '" + key + "'";
            return false;
        }
        const auto* ta = a.world().get<scene::Transform>(ea);
        const auto* tb = b.world().get<scene::Transform>(eb);
        if ((ta == nullptr) != (tb == nullptr)) {
            out_diff = "transform presence differs for '" + key + "'";
            return false;
        }
        if (ta != nullptr && tb != nullptr) {
            if (ta->local_x != tb->local_x || ta->local_y != tb->local_y ||
                ta->local_z != tb->local_z || ta->rot_x != tb->rot_x || ta->rot_y != tb->rot_y ||
                ta->rot_z != tb->rot_z || ta->scale_x != tb->scale_x || ta->scale_y != tb->scale_y ||
                ta->scale_z != tb->scale_z) {
                out_diff = "transform values differ for '" + key + "'";
                return false;
            }
            const auto* pa = (ta->parent.valid()) ? a.world().get<scene::NameComponent>(ta->parent) : nullptr;
            const auto* pb = (tb->parent.valid()) ? b.world().get<scene::NameComponent>(tb->parent) : nullptr;
            const std::string pan = (pa != nullptr) ? pa->name : std::string{};
            const std::string pbn = (pb != nullptr) ? pb->name : std::string{};
            const bool a_root = !ta->parent.valid() || !a.world().is_alive(ta->parent);
            const bool b_root = !tb->parent.valid() || !b.world().is_alive(tb->parent);
            if (a_root != b_root || pan != pbn) {
                out_diff = "parent differs for '" + key + "'";
                return false;
            }
        }
        const auto* pla = a.world().get<scene::PrefabLinkComponent>(ea);
        const auto* plb = b.world().get<scene::PrefabLinkComponent>(eb);
        if ((pla == nullptr) != (plb == nullptr) ||
            (pla != nullptr && pla->prefab_path != plb->prefab_path)) {
            out_diff = "prefab link differs for '" + key + "'";
            return false;
        }
    }
    out_diff.clear();
    return true;
}

std::optional<ecs::Entity> find_by_name(const scene::Scene& scene, const std::string& name) {
    for (ecs::Entity e : scene.world().all_entities()) {
        const auto* n = scene.world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == name) {
            return e;
        }
    }
    return std::nullopt;
}

namespace {

void copy_entity_all(const ecs::World& sw, ecs::Entity se, ecs::World& dw, ecs::Entity de) {
    if (const auto* t = sw.get<scene::Transform>(se)) {
        dw.add<scene::Transform>(de, *t);
    }
    if (const auto* n = sw.get<scene::NameComponent>(se)) {
        dw.add<scene::NameComponent>(de, *n);
    }
    if (const auto* m = sw.get<runtime::MeshComponent>(se)) {
        dw.add<runtime::MeshComponent>(de, *m);
    }
    if (const auto* c = sw.get<runtime::CameraComponent>(se)) {
        dw.add<runtime::CameraComponent>(de, *c);
    }
    if (const auto* l = sw.get<runtime::DirectionalLight>(se)) {
        dw.add<runtime::DirectionalLight>(de, *l);
    }
    if (const auto* s = sw.get<runtime::SkyComponent>(se)) {
        dw.add<runtime::SkyComponent>(de, *s);
    }
    if (const auto* p = sw.get<scene::PrefabLinkComponent>(se)) {
        dw.add<scene::PrefabLinkComponent>(de, *p);
    }
    if (const auto* rb = sw.get<physics::RigidBodyComponent>(se)) {
        physics::RigidBodyComponent fresh = *rb;
        fresh.body = physics::BodyHandle{};
        dw.add<physics::RigidBodyComponent>(de, fresh);
    }
    if (const auto* col = sw.get<physics::ColliderComponent>(se)) {
        dw.add<physics::ColliderComponent>(de, *col);
    }
    if (const auto* d = sw.get<runtime::DestructibleComponent>(se)) {
        dw.add<runtime::DestructibleComponent>(de, *d);
    }
    if (const auto* a = sw.get<animation::AnimationComponent>(se)) {
        dw.add<animation::AnimationComponent>(de, *a);
    }
    if (const auto* au = sw.get<audio::AudioComponent>(se)) {
        dw.add<audio::AudioComponent>(de, *au);
    }
    if (const auto* g = sw.get<gameplay::GameplayModuleComponent>(se)) {
        dw.add<gameplay::GameplayModuleComponent>(de, *g);
    }
}

} // namespace

void restore_scene(scene::Scene& dst, const scene::Scene& src) {
    dst.world().clear();
    dst.set_name(src.name());
    dst.metadata() = src.metadata();
    const ecs::World& sw = src.world();
    ecs::World& dw = dst.world();
    std::unordered_map<uint32_t, ecs::Entity> remap;
    for (ecs::Entity se : sw.all_entities()) {
        ecs::Entity de = dw.create_entity();
        remap[se.id] = de;
        copy_entity_all(sw, se, dw, de);
        if (auto* t = dw.get<scene::Transform>(de)) {
            t->parent = ecs::kInvalidEntity;
        }
    }
    for (ecs::Entity se : sw.all_entities()) {
        const auto* st = sw.get<scene::Transform>(se);
        if (st == nullptr || !st->parent.valid()) {
            continue;
        }
        auto dit = remap.find(se.id);
        auto pit = remap.find(st->parent.id);
        if (dit == remap.end() || pit == remap.end()) {
            continue;
        }
        if (!sw.is_alive(st->parent)) {
            continue;
        }
        scene::set_parent(dw, dit->second, pit->second);
    }
    scene::propagate_transforms(dw);
}

bool PlaySession::play(const scene::Scene& edit, std::string& out_err) {
    if (m_playing) {
        out_err = "Already playing";
        return false;
    }
    if (edit.world().alive_entity_count() == 0) {
        out_err = "Nothing to play (empty scene)";
        return false;
    }
    m_snapshot = clone_scene(edit);
    m_playing = true;
    return true;
}

bool PlaySession::stop(std::string& out_err) {
    if (!m_playing) {
        out_err = "Not playing";
        return false;
    }
    m_snapshot.reset();
    m_playing = false;
    return true;
}

} // namespace nf::editor
