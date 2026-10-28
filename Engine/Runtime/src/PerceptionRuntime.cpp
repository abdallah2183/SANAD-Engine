// NF/Runtime/src/PerceptionRuntime.cpp — the frame half of Phase 60.
//
// `ai::Perception` was written in Phase 22 and has been unit-tested ever since:
// four senses, a detection meter per sense, a linger window, a last known
// position, and a documented determinism contract. It was reachable from
// nothing. The Phase 25 audit said why and left it open — "it needs boards bound
// to entities, which is a design, not a wiring job" — and this file is that
// design's frame half: the scene's cast, the frame's own events as stimuli, and
// one live sensor per entity that authored a `Perception:` line.
//
// Three decisions are worth reading before the code:
//
//   * The cast carries STABLE ids. `PerceptionScene::add_target` hands out 1, 2,
//     3... in insertion order, and a sensor's whole memory is keyed on those
//     numbers, so the order is rebuilt the same way every frame: `all_entities()`
//     (ECS insertion order) filtered by what is worth noticing. Clearing and
//     refilling ONE scene would not work — `clear()` deliberately keeps the id
//     counter, so every frame would look like a cast of entirely new actors.
//
//   * A sensor is not in its own cast. `sight_strength` has no self-exclusion,
//     and at zero distance it returns a full-strength stimulus, so a guard would
//     detect itself in half a second and `most_threatening()` would answer "me".
//     Each sensor therefore gets its own copy of the cast with itself removed.
//     The copy is one vector of small structs per sensor per frame, which is the
//     honest price for a rule the module does not have.
//
//   * Pain is addressed, hearing is broadcast. A gunshot is a noise every sensor
//     in earshot may hear, so it goes in the shared cast. Being hit is not: the
//     damage sense is neither attenuated nor gradual (`detection = 1.0`), so a
//     shared pain stimulus would make every NPC in the level aware of whoever
//     shot one of them. Pain is copied into the victim's own cast and nowhere
//     else.

#include <NF/Runtime/Runtime.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/Runtime/Factions.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {
namespace {

/// FNV-1a over the authored team name. `PerceptionTarget::team` is an
/// uninterpreted number ("whatever the game filters on"), and this is that
/// number derived from the same string the damage rules read — so two entities
/// the faction rules call teammates are teammates to a sensor as well. 0 is
/// neutral, matching the damage system's "empty team".
u32 team_signature(std::string_view team) {
    if (team.empty()) {
        return 0u;
    }
    u32 h = 2166136261u;
    for (const char c : team) {
        h ^= static_cast<u32>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    return h != 0u ? h : 1u;
}

// A gunshot is a moment, but not an instant. A stimulus that existed for one
// frame would deliver `gain * strength * dt` — 0.03 of a meter that needs 1.0 to
// report — and no NPC could ever react to a noise. These lifetimes are what the
// module's own `PerceptionScene::age` would give them if the scene were not
// rebuilt per frame; see `m_perception_noises`.
//
// The strengths are calibrated against the sensor's own default `gain = 2`, so
// "noticed" is a distance an author can reason about. Detection fills at
// `gain * strength * falloff` per second, and the witness has `seconds` of it:
//
//   * a gunshot   (4.0 over 0.25 s) fills inside half the hearing reach;
//   * a grenade   (6.0 over 0.35 s) fills across most of it;
//   * a bullet on a wall (8.0 over 0.10 s) fills inside the nearer third.
//
// A silent approach, a longer reach or a slower sense is authored with the
// sensor's own `gain`, which is exactly what that field is for.
constexpr f32 kShotNoiseSeconds = 0.25f;
constexpr f32 kShotNoiseStrength = 4.0f;
constexpr f32 kImpactNoiseSeconds = 0.10f;
constexpr f32 kImpactNoiseStrength = 8.0f;
constexpr f32 kBlastNoiseSeconds = 0.35f;
constexpr f32 kBlastNoiseStrength = 6.0f;

/// A noise shorter than a frame would be dropped by the ageing pass before any
/// sensor read it. One frame is the floor, so a short crackle is still heard.
constexpr f32 kMinimumNoiseSeconds = 1.0e-4f;

// Hard caps. A firefight must not grow unbounded lists: the oldest noise is
// dropped first, which is also the one nobody can still hear.
constexpr usize kMaxPerceptionNoises = 64u;
constexpr usize kMaxPerceptionPain = 64u;

} // namespace

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void Runtime::bind_scene_perception() {
    // A scene change takes the fight with it: the actors a sensor remembered
    // belong to the level that has just been replaced, and a memory that
    // survived would make the first frame of the new level open with an NPC
    // already tracking a corpse position from the old one.
    m_perceptions.clear();
    m_perception_scene = ai::PerceptionScene{};
    m_perception_target_entities.clear();
    m_perception_noises.clear();
    m_perception_pain.clear();
    m_perception_sight_targets.clear();
    m_perception_lost_callouts.clear();

    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    for (const ecs::Entity e : world.all_entities()) {
        const auto* pc = world.get<PerceptionComponent>(e);
        if (pc == nullptr || !pc->enabled) {
            continue;
        }
        // Senses need an origin to see from. A component on an entity without a
        // Transform is authored data that cannot work, and the validator says so
        // rather than the frame inventing a position at the world origin.
        if (world.get<scene::Transform>(e) == nullptr) {
            continue;
        }

        PerceptionBinding binding;
        binding.entity = e;
        ai::Perception& p = binding.perception;

        // Sight is the authored sense; the other three take their defaults from
        // the module (hearing and touch omnidirectional, damage unattenuated)
        // and share sight's gain and loss so one `gain=` knob means one thing.
        ai::SenseConfig sight;
        sight.range = pc->sight_range;
        sight.half_fov_rad = to_radians(std::clamp(pc->sight_half_fov_deg, 0.0f, 180.0f));
        sight.gain = pc->sight_gain;
        sight.loss = pc->sight_loss;
        sight.linger = pc->sight_linger;
        p.sense(ai::StimulusKind::Sight) = sight;

        ai::SenseConfig hearing = sight;
        hearing.range = pc->hearing_range;
        hearing.half_fov_rad = PI;
        p.sense(ai::StimulusKind::Sound) = hearing;

        ai::SenseConfig touch = sight;
        touch.range = pc->touch_range;
        touch.half_fov_rad = PI;
        // Contact needs no staring: the module's own default is a fast gain and
        // a short memory, because touching something is not a suspicion.
        touch.gain = 4.0f;
        touch.linger = 1.0f;
        p.sense(ai::StimulusKind::Touch) = touch;

        ai::SenseConfig damage;
        damage.range = pc->pain ? 1.0f : 0.0f; // any positive range enables it
        damage.half_fov_rad = PI;
        damage.gain = 1.0f;
        damage.loss = 0.5f;
        damage.linger = 5.0f;
        p.sense(ai::StimulusKind::Damage) = damage;

        p.set_radius(std::max(0.0f, pc->radius));
        // The blocker is asked only about sight, and only about a segment whose
        // end is a cast member — see perception_sight_blocked.
        const ecs::Entity sensor = e;
        p.set_sight_blocker([this, sensor](Vec3 from, Vec3 to) {
            return perception_sight_blocked(sensor, from, to);
        });
        m_perceptions.push_back(std::move(binding));
    }

    if (!m_perceptions.empty()) {
        NF_LOG_INFO(LogCategory::Core, "Runtime: {} perception sensor(s) bound from the scene",
                    m_perceptions.size());
    }
}

// ---------------------------------------------------------------------------
// Squad radio (Phase 68)
// ---------------------------------------------------------------------------

bool Runtime::squad_contact_allowed(ecs::Entity reporter, ecs::Entity target,
                                    const std::string& token) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || reporter == target) {
        return false;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    if (!world.is_alive(target)) {
        return false;
    }
    // A corpse is not a contact, the same rule the nav gate uses. An entity
    // with no Health is still alive (a deciding NPC, a turret).
    if (const auto* health = world.get<HealthComponent>(target);
        health != nullptr && !health_alive(*health)) {
        return false;
    }
    // A same-token squadmate is never a contact, even on a scene that forgot
    // to author factions. Reporting one would turn the squad into a circle of
    // ghosts chasing each other.
    if (const auto* target_squad = world.get<SquadComponent>(target);
        target_squad != nullptr && target_squad->enabled && target_squad->token == token) {
        return false;
    }
    // Faction is the same social filter the hold uses: a member on a real team
    // reports only enemies; one with no team cannot tell friend from foe and
    // reports anyone it notices.
    const FactionComponent* own = world.get<FactionComponent>(reporter);
    if (own != nullptr && own->enabled && !own->team.empty()) {
        const auto* other = world.get<FactionComponent>(target);
        if (other == nullptr || !other->enabled || other->team.empty() ||
            team_relation(own->team, other->team) != TeamRelation::Hostile) {
            return false;
        }
    }
    return true;
}

void Runtime::exchange_squad_radio() {
    if (m_perceptions.size() < 2 || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        m_perception_lost_callouts.clear();
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    // One contact, snapshotted from the reporter before anything is delivered.
    // The token and range are copied by value: delivery writes memories, never
    // components, and a snapshot is what makes same-frame relays impossible.
    struct Report {
        ecs::Entity reporter{};
        std::string token;
        f32 radio = 0.0f;
        Vec3 origin{};
        u32 target_shared_id = 0;
        Vec3 position{};
        /// A lost-contact callout (Phase 69): the receiver's own `lost` window
        /// is the memory, with its sight linger as the floor. A live report is
        /// unchanged from Phase 68 and uses the linger alone.
        bool lost = false;
    };
    std::vector<Report> reports;
    reports.reserve(m_perceptions.size() + m_perception_lost_callouts.size());

    // --- 1. lost-contact callouts queued at the end of the previous step ------
    // Delivered BEFORE the live snapshot: if a mate is seeing the target right
    // now, the fresh position must win over the older last-known one.
    for (const SquadLostCallout& callout : m_perception_lost_callouts) {
        if (!world.is_alive(callout.reporter) || !world.is_alive(callout.target)) {
            continue;
        }
        const auto* squad = world.get<SquadComponent>(callout.reporter);
        if (squad == nullptr || !squad->enabled || squad->token.empty() ||
            !(squad->radio > 0.0f) || !std::isfinite(squad->radio) ||
            !(squad->lost > 0.0f) || !std::isfinite(squad->lost)) {
            continue;
        }
        const u32 shared = perception_target_id(callout.target);
        if (shared == 0u ||
            !squad_contact_allowed(callout.reporter, callout.target, squad->token)) {
            continue;
        }
        const auto* transform = world.get<scene::Transform>(callout.reporter);
        if (transform == nullptr) {
            continue;
        }
        Report report;
        report.reporter = callout.reporter;
        report.token = squad->token;
        report.radio = squad->radio;
        report.origin = Vec3{transform->world_x, transform->world_y, transform->world_z};
        report.target_shared_id = shared;
        report.position = callout.position;
        report.lost = true;
        reports.push_back(std::move(report));
    }
    // The callouts were one-shot messages: this exchange was their delivery.
    m_perception_lost_callouts.clear();

    // --- 2. this step's live contacts ---------------------------------------
    for (const PerceptionBinding& binding : m_perceptions) {
        const auto* squad = world.get<SquadComponent>(binding.entity);
        if (squad == nullptr || !squad->enabled || squad->token.empty() ||
            !(squad->radio > 0.0f) || !std::isfinite(squad->radio)) {
            continue;
        }
        const auto* transform = world.get<scene::Transform>(binding.entity);
        if (transform == nullptr) {
            continue;
        }

        for (const ai::PerceivedTarget& memory : binding.perception.perceived()) {
            // Only a contact this member is reading PERSONALLY this tick. A
            // radio-born memory is aware and never sensed, so it can never be
            // relayed; `personally_perceived` is the history half of the rule.
            if (!memory.sensed || !memory.personally_perceived) {
                continue;
            }
            const u32 shared = perception_shared_target_id(binding.entity, memory.target_id);
            if (shared == 0u) {
                continue;
            }
            const ecs::Entity target = m_perception_target_entities[shared - 1u];
            if (!squad_contact_allowed(binding.entity, target, squad->token)) {
                continue;
            }
            Report report;
            report.reporter = binding.entity;
            report.token = squad->token;
            report.radio = squad->radio;
            report.origin = Vec3{transform->world_x, transform->world_y, transform->world_z};
            report.target_shared_id = shared;
            report.position = memory.last_known_position;
            report.lost = false;
            reports.push_back(std::move(report));
        }
    }
    if (reports.empty()) {
        return;
    }

    // Deliver the snapshot. Receivers are visited in the same bind order as
    // reporters, so two runs with the same scene and the same dt produce the
    // same memories. Lost callouts come first in the list, live reports after.
    for (const Report& report : reports) {
        const ecs::Entity target = m_perception_target_entities[report.target_shared_id - 1u];
        const auto* target_faction = world.get<FactionComponent>(target);
        for (PerceptionBinding& receiver : m_perceptions) {
            if (receiver.entity == report.reporter) {
                continue;
            }
            const auto* squad = world.get<SquadComponent>(receiver.entity);
            if (squad == nullptr || !squad->enabled || !(squad->radio > 0.0f) ||
                !std::isfinite(squad->radio) || squad->token != report.token) {
                continue;
            }
            // A lost callout needs the RECEIVER to opt in too: its own `lost`
            // window is the memory it is willing to give the message. Zero
            // keeps that member exactly at Phase 68.
            if (report.lost && (!(squad->lost > 0.0f) || !std::isfinite(squad->lost))) {
                continue;
            }
            const auto* transform = world.get<scene::Transform>(receiver.entity);
            if (transform == nullptr) {
                continue;
            }
            const Vec3 origin{transform->world_x, transform->world_y, transform->world_z};
            const Vec3 delta = origin - report.origin;
            // A link needs BOTH radios to reach: a scout with a short antenna
            // cannot shout past it even when the listener could hear farther.
            const f32 reach = std::min(report.radio, squad->radio);
            if (delta.length_sq() > reach * reach) {
                continue;
            }
            if (receiver.entity == target) {
                continue;
            }
            const FactionComponent* own = world.get<FactionComponent>(receiver.entity);
            if (own != nullptr && own->enabled && !own->team.empty()) {
                if (target_faction == nullptr || !target_faction->enabled ||
                    target_faction->team.empty() ||
                    team_relation(own->team, target_faction->team) != TeamRelation::Hostile) {
                    continue;
                }
            }
            const u32 local = perception_local_target_id(receiver.entity, target);
            if (local == 0u) {
                continue;
            }
            // The receiver keeps a rumour for its own search window. A sensor
            // with no linger has no memory to give the report, by design. A
            // lost callout asks for its own `lost` window instead, never less
            // than that same linger.
            f32 linger = receiver.perception.sense(ai::StimulusKind::Sight).linger;
            if (report.lost) {
                linger = std::max(linger, squad->lost);
            }
            receiver.perception.receive_report(local, report.position, linger);
        }
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const ai::Perception* Runtime::perception_of(ecs::Entity entity) const {
    for (const PerceptionBinding& binding : m_perceptions) {
        if (binding.entity == entity) {
            return &binding.perception;
        }
    }
    return nullptr;
}

u32 Runtime::perception_target_id(ecs::Entity entity) const {
    for (usize i = 0; i < m_perception_target_entities.size(); ++i) {
        const ecs::Entity candidate = m_perception_target_entities[i];
        if (candidate.id == entity.id && candidate.generation == entity.generation) {
            // `add_target` numbers the cast from 1, so the id is the position
            // plus one. 0 stays reserved for "not a target at all".
            return static_cast<u32>(i) + 1u;
        }
    }
    return 0u;
}

u32 Runtime::perception_local_target_id(ecs::Entity sensor, ecs::Entity target) const {
    if (sensor == target) {
        return 0u; // a sensor is not in its own cast
    }
    const u32 shared = perception_target_id(target);
    if (shared == 0u) {
        return 0u;
    }
    // Where the sensor sits in the SHARED cast decides the shift: the sensor is
    // one actor that this cast does not contain, so everything numbered after it
    // moves down by one.
    for (usize i = 0; i < m_perception_target_entities.size(); ++i) {
        if (m_perception_target_entities[i] == sensor) {
            const u32 sensor_shared = static_cast<u32>(i) + 1u;
            return shared > sensor_shared ? shared - 1u : shared;
        }
    }
    // A sensor that is not in the cast has no local cast at all: no shift.
    return shared;
}

u32 Runtime::perception_shared_target_id(ecs::Entity sensor, u32 local) const {
    if (local == 0u) {
        return 0u;
    }
    for (usize i = 0; i < m_perception_target_entities.size(); ++i) {
        if (m_perception_target_entities[i] == sensor) {
            const u32 sensor_shared = static_cast<u32>(i) + 1u;
            const u32 shared = local >= sensor_shared ? local + 1u : local;
            return shared <= m_perception_target_entities.size() ? shared : 0u;
        }
    }
    // A sensor outside the shared cast (authored without a Transform when the
    // cast was built) has no shift.
    return local <= m_perception_target_entities.size() ? local : 0u;
}

bool Runtime::entity_aware_of(ecs::Entity sensor, ecs::Entity target) const {
    const ai::Perception* p = perception_of(sensor);
    if (p == nullptr) {
        return false;
    }
    const u32 id = perception_local_target_id(sensor, target);
    return id != 0u && p->is_aware_of(id);
}

bool Runtime::entity_sensing(ecs::Entity sensor, ecs::Entity target) const {
    const ai::Perception* p = perception_of(sensor);
    if (p == nullptr) {
        return false;
    }
    const u32 id = perception_local_target_id(sensor, target);
    return id != 0u && p->is_sensing(id);
}

bool Runtime::entity_last_known(ecs::Entity sensor, ecs::Entity target,
                                Vec3& out_position) const {
    const ai::Perception* p = perception_of(sensor);
    if (p == nullptr) {
        return false;
    }
    const u32 id = perception_local_target_id(sensor, target);
    if (id == 0u) {
        return false;
    }
    const ai::PerceivedTarget* memory = p->find(id);
    if (memory == nullptr) {
        return false;
    }
    out_position = memory->last_known_position;
    return true;
}

bool Runtime::entity_forget(ecs::Entity sensor, ecs::Entity target) {
    for (PerceptionBinding& binding : m_perceptions) {
        if (!(binding.entity == sensor)) {
            continue;
        }
        const u32 id = perception_local_target_id(sensor, target);
        return id != 0u && binding.perception.forget(id);
    }
    return false;
}

// ---------------------------------------------------------------------------
// The cast
// ---------------------------------------------------------------------------

bool Runtime::is_perception_target(const ecs::World& world, ecs::Entity entity) const {
    if (world.get<scene::Transform>(entity) == nullptr) {
        return false;
    }
    // Something "alive about it": it can be hurt, it can shoot, it can decide,
    // or it belongs to a side. A crate is not a thing an NPC forms an opinion
    // about, and every extra target costs every sensor a pass per frame.
    return world.get<HealthComponent>(entity) != nullptr ||
           world.get<WeaponComponent>(entity) != nullptr ||
           world.get<CombatAgentComponent>(entity) != nullptr ||
           world.get<PerceptionComponent>(entity) != nullptr ||
           world.get<FactionComponent>(entity) != nullptr;
}

void Runtime::step_perception(f32 dt) {
    if (m_perceptions.empty() || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    // A frozen editor frame (dt <= 0) still rebuilds the cast — a gizmo drag has
    // to move what a sensor sees — but integrates nothing: detection is a
    // function of time, and no time passed.
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        dt = 0.0f;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    // --- 1. the cast, in entity order (see the file header) ------------------
    m_perception_scene = ai::PerceptionScene{};
    m_perception_target_entities.clear();
    for (const ecs::Entity e : world.all_entities()) {
        if (!is_perception_target(world, e)) {
            continue;
        }
        const auto* transform = world.get<scene::Transform>(e);
        if (transform == nullptr) {
            continue;
        }
        const auto* own = world.get<PerceptionComponent>(e);
        const auto* faction = world.get<FactionComponent>(e);

        ai::PerceptionTarget target;
        target.position = Vec3{transform->world_x, transform->world_y, transform->world_z};
        target.radius = own != nullptr ? std::max(0.0f, own->radius) : 0.5f;
        target.visibility = own != nullptr ? std::clamp(own->visibility, 0.0f, 1.0f) : 1.0f;
        target.noise = own != nullptr ? std::clamp(own->noise, 0.0f, 1.0f) : 0.0f;
        target.team = (faction != nullptr && faction->enabled) ? team_signature(faction->team) : 0u;

        m_perception_scene.add_target(target);
        m_perception_target_entities.push_back(e);
    }

    // --- 1b. the squad radios --------------------------------------------------
    // Reports are read from the memories the PREVIOUS step left behind and
    // delivered as awareness only, before any sensor integrates. That ordering
    // is the anti-relay rule: every receiver sees the same snapshot, so a
    // report cannot spread through a squad within one frame. Navigation also
    // runs before this step, so a radio hold or investigation has the same
    // documented one-frame latency as a sighting.
    exchange_squad_radio();

    // --- 2. the noises still audible ----------------------------------------
    // Aged first, so a noise emitted below gets its whole lifetime rather than
    // one frame less. The scene is rebuilt every frame, which is why the
    // lifetimes live here instead of in `PerceptionScene::age`.
    for (auto it = m_perception_noises.begin(); it != m_perception_noises.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f) {
            it = m_perception_noises.erase(it);
        } else {
            ++it;
        }
    }
    m_perception_pain.clear();

    const auto add_noise = [this](Vec3 position, f32 strength, u32 source, f32 seconds) {
        if (m_perception_noises.size() >= kMaxPerceptionNoises) {
            // Oldest out, deterministically: a long firefight must not grow an
            // unbounded list of noises nobody can still hear.
            m_perception_noises.erase(m_perception_noises.begin());
        }
        PerceptionNoise noise;
        noise.position = position;
        noise.strength = strength;
        noise.source_target = source;
        noise.remaining = std::max(seconds, kMinimumNoiseSeconds);
        m_perception_noises.push_back(noise);
    };

    // The same completed frame the feedback director consumes, so a shot is
    // heard exactly once and by the frame that drew its flash.
    for (const ShotEvent& shot : m_shots.previous()) {
        add_noise(shot.origin, kShotNoiseStrength, perception_target_id(shot.shooter),
                  kShotNoiseSeconds);
    }
    for (const ImpactEvent& impact : m_impacts.previous()) {
        const bool blast = impact.damage_type == DamageType::Explosive && !impact.splash;
        // A splash victim is the consequence of a blast, not a new round landing
        // — the same rule the feedback layer follows. A status tick is not a
        // round either, but it IS pain: a burning NPC knows who set it alight.
        if (!impact.status && !impact.splash) {
            add_noise(impact.position, blast ? kBlastNoiseStrength : kImpactNoiseStrength,
                      perception_target_id(impact.shooter),
                      blast ? kBlastNoiseSeconds : kImpactNoiseSeconds);
        }
        // Pain is addressed, not broadcast: the damage sense ignores distance, so
        // a shared stimulus would wake every sensor in the level. A victim that
        // is not a cast member is skipped by the module itself (an unattributed
        // stimulus records nothing), which is stated in its header.
        if (impact.victim.valid()) {
            const u32 source = perception_target_id(impact.shooter);
            if (source != 0u) {
                PerceptionPain pain;
                pain.victim = impact.victim;
                pain.source_target = source;
                pain.position = impact.position;
                if (m_perception_pain.size() >= kMaxPerceptionPain) {
                    m_perception_pain.erase(m_perception_pain.begin());
                }
                m_perception_pain.push_back(pain);
            }
        }
    }

    // --- 3. one world cast for the sight rays --------------------------------
    // Collected with no ignore entity: the sensor is skipped inside the ray
    // itself, because the cast a sensor reads is not the cast a ray tests.
    collect_hitscan_targets(world, ecs::kInvalidEntity);
    m_perception_sight_targets = m_hitscan_targets;

    // --- 4. step every sensor -------------------------------------------------
    for (PerceptionBinding& binding : m_perceptions) {
        const auto* transform = world.get<scene::Transform>(binding.entity);
        const auto* pc = world.get<PerceptionComponent>(binding.entity);
        if (transform == nullptr || pc == nullptr || !pc->enabled) {
            // A sensor that cannot step cannot lose a contact either: drop its
            // bookkeeping so re-enabling starts from what it actually sees.
            binding.squad_contacts.clear();
            continue;
        }
        ai::Perception& p = binding.perception;
        p.set_position(Vec3{transform->world_x, transform->world_y + pc->eye_height,
                            transform->world_z});
        p.set_radius(std::max(0.0f, pc->radius));

        // A brain looks where it is AIMING, not where its body was modelled to
        // face: a guard that has already turned towards a noise must not keep
        // staring at its Transform's forward. A sensor with no brain looks down
        // its Transform's yaw, which is what an author drags in the editor.
        const auto* agent = world.get<CombatAgentComponent>(binding.entity);
        const f32 yaw = agent != nullptr ? agent->state.yaw_deg : transform->rot_y;
        const f32 pitch = agent != nullptr ? agent->state.pitch_deg : 0.0f;
        p.set_forward(aim_direction(yaw, pitch));

        // The sensor's own cast: the shared cast minus itself, plus its own pain.
        // `operator=` from a fresh scene keeps the vectors' capacity, so this is
        // a copy, not a fresh allocation, once the frame has run once.
        binding.cast = ai::PerceptionScene{};
        const std::vector<ai::PerceptionTarget>& targets = m_perception_scene.targets();
        for (usize i = 0; i < targets.size() && i < m_perception_target_entities.size(); ++i) {
            if (m_perception_target_entities[i] == binding.entity) {
                continue;
            }
            binding.cast.add_target(targets[i]);
        }
        // A stimulus carries the SHARED id of whoever caused it; the sensor reads
        // its own cast. `perception_local_target_id` is that translation, and it
        // is the same function the public queries and the agent bridge use — one
        // rule, one place.
        const auto local_id = [this, &binding](u32 shared_id) -> u32 {
            if (shared_id == 0u || shared_id > m_perception_target_entities.size()) {
                return 0u;
            }
            return perception_local_target_id(binding.entity,
                                              m_perception_target_entities[shared_id - 1u]);
        };

        for (const PerceptionPain& pain : m_perception_pain) {
            if (!(pain.victim == binding.entity)) {
                continue;
            }
            const u32 source = local_id(pain.source_target);
            if (source == 0u) {
                continue; // the attacker is not in this sensor's cast at all
            }
            ai::Stimulus stimulus;
            stimulus.kind = ai::StimulusKind::Damage;
            stimulus.position = pain.position;
            stimulus.strength = 1.0f;
            stimulus.source_id = source;
            stimulus.remaining = dt;
            binding.cast.emit(stimulus);
        }
        for (const PerceptionNoise& noise : m_perception_noises) {
            ai::Stimulus stimulus;
            stimulus.kind = ai::StimulusKind::Sound;
            stimulus.position = noise.position;
            stimulus.strength = noise.strength;
            stimulus.source_id = local_id(noise.source_target);
            stimulus.remaining = noise.remaining;
            binding.cast.emit(stimulus);
        }

        p.update(binding.cast, dt);
        track_squad_contacts(binding);
    }
}

void Runtime::track_squad_contacts(PerceptionBinding& binding) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    const auto* squad = world.get<SquadComponent>(binding.entity);
    const bool radio_live = squad != nullptr && squad->enabled && !squad->token.empty() &&
                            squad->radio > 0.0f && std::isfinite(squad->radio);
    // `lost` is the RECEIVER's memory window; on the reporter it is the opt-in
    // switch. A member with lost=0 keeps Phase 68 exactly: no callout at all.
    const bool callouts = radio_live && squad->lost > 0.0f && std::isfinite(squad->lost);

    std::vector<PerceptionBinding::SquadContact> next;
    next.reserve(binding.squad_contacts.size());
    if (radio_live) {
        for (const ai::PerceivedTarget& memory : binding.perception.perceived()) {
            // The same anti-relay gate as the live report: only what this
            // sensor read with its OWN senses is allowed to move on.
            if (!memory.sensed || !memory.personally_perceived) {
                continue;
            }
            const u32 shared = perception_shared_target_id(binding.entity, memory.target_id);
            if (shared == 0u) {
                continue;
            }
            const ecs::Entity target = m_perception_target_entities[shared - 1u];
            if (!squad_contact_allowed(binding.entity, target, squad->token)) {
                continue;
            }
            bool updated = false;
            for (PerceptionBinding::SquadContact& contact : binding.squad_contacts) {
                if (contact.target == target) {
                    contact.last_position = memory.last_known_position;
                    next.push_back(contact);
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                next.push_back(PerceptionBinding::SquadContact{target,
                                                               memory.last_known_position});
            }
        }
        if (callouts) {
            for (const PerceptionBinding::SquadContact& previous : binding.squad_contacts) {
                bool still_sensed = false;
                for (const PerceptionBinding::SquadContact& current : next) {
                    if (current.target == previous.target) {
                        still_sensed = true;
                        break;
                    }
                }
                if (still_sensed) {
                    continue;
                }
                // A corpse is a kill, not a lost contact, and an actor that
                // left the cast has nothing to call out. A target still in the
                // cast but no longer reportable (a token or faction change) is
                // dropped silently for the same reason the live report skips it.
                if (!squad_contact_allowed(binding.entity, previous.target, squad->token)) {
                    continue;
                }
                m_perception_lost_callouts.push_back(SquadLostCallout{
                    binding.entity, previous.target, previous.last_position});
            }
        }
    }
    // Entries are replaced, never appended across steps: a contact that fell
    // out stays gone, so its callout is queued exactly once.
    binding.squad_contacts = std::move(next);
}

bool Runtime::perception_sight_blocked(ecs::Entity sensor, const Vec3& from, const Vec3& to) {
    const Vec3 delta = to - from;
    const f32 distance = delta.length();
    if (!(distance > 1.0e-4f)) {
        return false; // a target at the eye is not behind anything
    }
    // +0.05 so a target standing exactly on a wall is seen rather than counted
    // as hidden by the wall behind it.
    const HitscanHit hit =
        hitscan_nearest(m_perception_sight_targets, from, delta, distance + 0.05f, sensor);
    if (!hit.hit) {
        return false;
    }
    // The actor the ray ENDS on is not an obstruction to itself. The value handed
    // here is the exact copy of a cast position written by step_perception, so
    // the match is by equality (tolerance 0) rather than by a radius: a tolerance
    // would make two actors standing close together shield each other.
    const std::vector<ai::PerceptionTarget>& targets = m_perception_scene.targets();
    for (usize i = 0; i < targets.size() && i < m_perception_target_entities.size(); ++i) {
        if (!targets[i].position.nearly_equals(to, 0.0f)) {
            continue;
        }
        return !(hit.entity == m_perception_target_entities[i]);
    }
    // Nothing in the cast ends there: whatever the ray found is in the way.
    return true;
}

} // namespace nf::runtime
