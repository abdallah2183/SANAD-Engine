#include <NF/Editor/PlayMode.hpp>
#include <NF/Editor/SceneCopy.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Vfx/Components.hpp>

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
        if (const auto* pl = sw.get<runtime::PointLightComponent>(se)) {
            dw.add<runtime::PointLightComponent>(de, *pl);
        }
        if (const auto* sl = sw.get<runtime::SpotLightComponent>(se)) {
            dw.add<runtime::SpotLightComponent>(de, *sl);
        }
        if (const auto* s = sw.get<runtime::SkyComponent>(se)) {
            dw.add<runtime::SkyComponent>(de, *s);
        }
        if (const auto* pp = sw.get<runtime::PostProcessComponent>(se)) {
            dw.add<runtime::PostProcessComponent>(de, *pp);
        }
        // Phase 59: Play was the other half of the same gap. A scene authored
        // with a `Feedback:` line entered play mode with no feedback component,
        // so the shots the player fired were silent in Play and audible in the
        // packaged build — the worst possible place for a mismatch.
        if (const auto* fb = sw.get<runtime::FeedbackComponent>(se)) {
            dw.add<runtime::FeedbackComponent>(de, *fb);
        }
        // Perception (Phase 60): the senses are authored data and the component
        // carries no session state — the live memory belongs to the Runtime — so
        // a copy is a full copy. Without this, a guard authored to hear would
        // enter Play blind and deaf while the packaged build heard everything.
        if (const auto* pc = sw.get<runtime::PerceptionComponent>(se)) {
            dw.add<runtime::PerceptionComponent>(de, *pc);
        }
        // Squad radio (Phase 68) is authored data as well: the live contacts
        // belong to the Runtime, so the token and range are all a clone needs.
        if (const auto* squad = sw.get<runtime::SquadComponent>(se)) {
            dw.add<runtime::SquadComponent>(de, *squad);
        }
        // A jammer field (Phase 70) is authored data on the same terms: the
        // radius and strength are what a clone needs, with no session state.
        if (const auto* jammer = sw.get<runtime::JammerComponent>(se)) {
            dw.add<runtime::JammerComponent>(de, *jammer);
        }
        // The decision layer (Phase 61) is authored data on the same terms.
        if (const auto* brain = sw.get<runtime::BrainComponent>(se)) {
            dw.add<runtime::BrainComponent>(de, *brain);
        }
        if (const auto* machine = sw.get<runtime::StateMachineComponent>(se)) {
            dw.add<runtime::StateMachineComponent>(de, *machine);
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
        // The audio ENVIRONMENT: a reverb zone, the level's music and its
        // ambience bed. The runtime reads all three from the LIVE world, so a
        // clone that dropped them took the whole audio environment with it —
        // and the snapshot is what Stop restores the scene FROM, so the scene
        // came back without its echo, its music and its wind.
        if (const auto* rz = sw.get<audio::ReverbZoneComponent>(se)) {
            dw.add<audio::ReverbZoneComponent>(de, *rz);
        }
        if (const auto* mu = sw.get<audio::MusicComponent>(se)) {
            // The decoded samples travel with it: the component is the ONLY
            // record of the track (the path is re-resolved on load), and a
            // clone without them would restore a silent music bed.
            dw.add<audio::MusicComponent>(de, *mu);
        }
        if (const auto* am = sw.get<audio::AmbienceComponent>(se)) {
            dw.add<audio::AmbienceComponent>(de, *am);
        }
        // The day/night cycle. It attaches to the same entity as the sky and
        // drives it, and the runtime advances it from the LIVE world: a clone
        // without it left the sun at the authored hour and the sky frozen.
        if (const auto* tod = sw.get<runtime::TimeOfDayComponent>(se)) {
            dw.add<runtime::TimeOfDayComponent>(de, *tod);
        }
        if (const auto* g = sw.get<gameplay::GameplayModuleComponent>(se)) {
            dw.add<gameplay::GameplayModuleComponent>(de, *g);
        }
        if (const auto* sc = sw.get<scripting::ScriptComponent>(se)) {
            dw.add<scripting::ScriptComponent>(de, *sc);
        }
        if (const auto* pc = sw.get<vfx::ParticleComponent>(se)) {
            dw.add<vfx::ParticleComponent>(de, *pc);
        }
        if (const auto* cc = sw.get<physics::ClothComponent>(se)) {
            dw.add<physics::ClothComponent>(de, *cc);
        }
        if (const auto* ch = sw.get<physics::CharacterComponent>(se)) {
            physics::CharacterComponent fresh = *ch;
            fresh.wish_dir = Vec3{0.0f, 0.0f, 0.0f};
            fresh.jump = false;
            fresh.grounded = false;
            dw.add<physics::CharacterComponent>(de, fresh);
        }
        if (const auto* nm = sw.get<runtime::NavMeshComponent>(se)) {
            // Pure configuration (the bake is rebuilt from it), nothing live.
            dw.add<runtime::NavMeshComponent>(de, *nm);
        }
        if (const auto* na = sw.get<runtime::NavAgentComponent>(se)) {
            // Pure configuration plus the authored goal (the path lives
            // beside the component in the runtime, never inside it).
            dw.add<runtime::NavAgentComponent>(de, *na);
        }
        if (const auto* terrain = sw.get<runtime::TerrainComponent>(se)) {
            // Pure scene data: the runtime rebuilds the terrain state and the
            // navigation bake from it on adopt, so a copy is a full copy.
            dw.add<runtime::TerrainComponent>(de, *terrain);
        }
        if (const auto* route = sw.get<runtime::NavRouteComponent>(se)) {
            // The authored points cross; the live cursor is runtime state and
            // is not inside the component.
            dw.add<runtime::NavRouteComponent>(de, *route);
        }
        if (const auto* vc = sw.get<physics::VehicleComponent>(se)) {
            // Tune only: live inputs are per-session and the JoltVehicle view
            // points at another world's simulation. A clone that carried
            // either would drive a fresh car with a stale foot — or worse,
            // through a dangling pointer.
            physics::VehicleComponent fresh = *vc;
            fresh.throttle = 0.0f;
            fresh.steer = 0.0f;
            fresh.brake = 0.0f;
            fresh.handbrake = 0.0f;
            fresh.vehicle = nullptr;
            dw.add<physics::VehicleComponent>(de, fresh);
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
    // The audio environment and the day/night cycle are part of the scene's
    // structure: a clone that dropped them looked identical to the original
    // and the comparison said so, which is how a Stop that restored a scene
    // without its echo passed the integrity check.
    s += w.has<audio::ReverbZoneComponent>(e) ? "Z" : "-";
    s += w.has<audio::MusicComponent>(e) ? "Y" : "-";
    s += w.has<audio::AmbienceComponent>(e) ? "Q" : "-";
    s += w.has<runtime::TimeOfDayComponent>(e) ? "O" : "-";
    s += w.has<gameplay::GameplayModuleComponent>(e) ? "G" : "-";
    s += w.has<scripting::ScriptComponent>(e) ? "X" : "-";
    s += w.has<vfx::ParticleComponent>(e) ? "V" : "-";
    s += w.has<physics::ClothComponent>(e) ? "F" : "-";
    s += w.has<physics::CharacterComponent>(e) ? "H" : "-";
    // Navigation (Phase 28/63/64): the volume, its walker, the ground the bake
    // samples and the patrol. A clone that dropped any of them changed what the
    // session simulated while looking structurally identical without these.
    s += w.has<runtime::NavMeshComponent>(e) ? "B" : "-";
    s += w.has<runtime::NavAgentComponent>(e) ? "W" : "-";
    s += w.has<runtime::TerrainComponent>(e) ? "E" : "-";
    s += w.has<runtime::NavRouteComponent>(e) ? "r" : "-";
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
    // The comparison key for an entity: its authored name, or a positional
    // fallback for an entity that has none.
    //
    // `#<id>` is a WEAK key — the live and reloaded scenes do not have to agree
    // on ids — but it can only ever apply to an entity that is unnamed in BOTH
    // scenes, which the editor path cannot produce: Runtime assigns a
    // NameComponent to every entity that lacks one, so there is always a name to
    // key on. It is kept for the raw-scene callers (tests) that build a world by
    // hand.
    const auto key_of = [](const scene::Scene& s, ecs::Entity e) {
        const auto* n = s.world().get<scene::NameComponent>(e);
        return (n != nullptr && !n->name.empty()) ? n->name : ("#" + std::to_string(e.id));
    };

    // Index b by name into a LIST, not a single entity — names are NOT unique.
    // Runtime synthesizes a name from the entity's KIND for every entity with no
    // `Name:` line, so a scene with two meshes is a scene with two entities both
    // called "Mesh" (Panels.cpp already works around the same collision for its
    // ground-plane glyph). Keeping only the last one, which this used to do,
    // paired every live "Mesh" against the LAST saved "Mesh": a scene that
    // round-tripped perfectly reported "transform values differ for 'Mesh'" and
    // the editor exited 1, sending the reader to the serializer to hunt a
    // corruption that was never there.
    std::unordered_map<std::string, std::vector<ecs::Entity>> by_name_b;
    for (ecs::Entity e : blist) {
        by_name_b[key_of(b, e)].push_back(e);
    }
    // How many entities of each name b has already been paired. The Nth entity
    // with a given name in `a` pairs with the Nth with that name in `b`, which
    // is the right pairing: both lists come from the same file order and a
    // re-save cannot reorder what the loader wrote in order. A cursor rather
    // than erase-from-front keeps a 2000-entity scene linear.
    std::unordered_map<std::string, std::size_t> paired;
    for (ecs::Entity ea : alist) {
        const std::string key = key_of(a, ea);
        auto it = by_name_b.find(key);
        std::size_t& taken = paired[key];
        if (it == by_name_b.end() || taken >= it->second.size()) {
            out_diff = "entity '" + key + "' missing in second scene";
            return false;
        }
        ecs::Entity eb = it->second[taken++];
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
            // Epsilon, not ==. A save/load round trip puts every float through
            // the scene text format, which writes 9 significant digits — enough to
            // round-trip any f32, but only because `Transform` is written with
            // `setprecision(9)`. A transform that reached here by a gizmo drag is
            // the product of an add per frame, so its bits are whatever that sum
            // produced; re-reading it as text can land one ULP away even when the
            // two are the same number to any author-visible precision.
            //
            // The tolerance is relative for the large components (a world position
            // after a long walk) and absolute for the small ones, so neither a tiny
            // near-zero drift nor a large-coordinate last-bit difference is read as
            // "the save corrupted the scene" — which is the failure this check
            // exists to catch.
            auto near = [](f32 x, f32 y) {
                if (x == y) return true;              // covers both zero
                const f32 diff = std::fabs(x - y);
                const f32 scale = std::max({1.0f, std::fabs(x), std::fabs(y)});
                return diff <= 1e-5f * scale;
            };
            if (!near(ta->local_x, tb->local_x) || !near(ta->local_y, tb->local_y) ||
                !near(ta->local_z, tb->local_z) || !near(ta->rot_x, tb->rot_x) ||
                !near(ta->rot_y, tb->rot_y) || !near(ta->rot_z, tb->rot_z) ||
                !near(ta->scale_x, tb->scale_x) || !near(ta->scale_y, tb->scale_y) ||
                !near(ta->scale_z, tb->scale_z)) {
                // The values go IN the message: "transform values differ" alone
                // sent whoever reads this log to the serializer to look for a
                // precision bug that is not there, when the actual difference is
                // usually a harness that edited the scene without saving.
                {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                                  "transform values differ for '%s' live=(%.6f,%.6f,%.6f) "
                                  "saved=(%.6f,%.6f,%.6f)",
                                  key.c_str(), ta->local_x, ta->local_y, ta->local_z,
                                  tb->local_x, tb->local_y, tb->local_z);
                    out_diff = buf;
                }
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
    copy_entity_components(sw, se, dw, de);
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
