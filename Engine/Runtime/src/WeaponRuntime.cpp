// NF/Runtime/WeaponRuntime.cpp — the part of a gun that needs a world.
//
// Weapon.hpp decides what the trigger does; this decides what the bullet hits
// and who loses hit points. The three pieces it joins:
//
//   * the camera, for the view the crosshair sits on (including this frame's
//     recoil, so the crosshair and the bullet never disagree),
//   * the scene's colliders, for the boxes a shot can land on (Hitscan.hpp),
//   * the target's HealthComponent, for what the hit costs.
//
// The rule that makes "the player" mean one thing: it is the entity the ACTIVE
// camera follows — the same name lookup update_follow_camera does. A scene with
// no camera has no player and fires nothing, which is why a headless validation
// run is quiet rather than shooting at the origin.

#include <NF/Runtime/Runtime.hpp>

#include <NF/Runtime/Damage.hpp>
#include <NF/Runtime/Explosion.hpp>
#include <NF/Runtime/Hitscan.hpp>
#include <NF/Runtime/Factions.hpp>
#include <NF/Runtime/Projectile.hpp>
#include <NF/Runtime/Weapon.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace nf::runtime {

namespace {

// The active camera, or null. Same selection rule as update_follow_camera: the
// first camera marked active in entity order.
CameraComponent* active_camera(ecs::World& world) {
    for (ecs::Entity e : world.query<CameraComponent>()) {
        CameraComponent* c = world.get<CameraComponent>(e);
        if (c != nullptr && c->is_active) {
            return c;
        }
    }
    return nullptr;
}

// The living entity with this name. The same lookup the Lua binding does, so a
// camera that follows "Player" and a script that finds "Player" agree.
ecs::Entity entity_by_name(ecs::World& world, const std::string& name) {
    if (name.empty()) {
        return ecs::kInvalidEntity;
    }
    for (ecs::Entity e : world.all_entities()) {
        if (!world.is_alive(e)) {
            continue;
        }
        const scene::NameComponent* n = world.get<scene::NameComponent>(e);
        if (n != nullptr && n->name == name) {
            return e;
        }
    }
    return ecs::kInvalidEntity;
}

// The affiliation the damage rules see. An absent, disabled or empty faction
// is neutral, which is exactly the behaviour a scene without Phase 46 data
// already had.
TeamIdentity team_identity(ecs::World& world, ecs::Entity entity) {
    const FactionComponent* faction = world.get<FactionComponent>(entity);
    if (faction == nullptr || !faction->enabled || faction->team.empty()) {
        return TeamIdentity{};
    }
    return TeamIdentity{faction->team, faction->friendly_fire};
}

bool team_allows_damage(ecs::World& world, ecs::Entity attacker, ecs::Entity victim) {
    return team_damage_allowed(team_identity(world, attacker), team_identity(world, victim));
}

// A target an auto-acquiring agent may pick: both sides must have a real team,
// and those teams must be different. Neutral entities are deliberately NOT
// auto-targeted — a guard should not start shooting a crate because it has hit
// points, and a scene that wants that can name the crate explicitly.
bool hostile_faction_pair(ecs::World& world, ecs::Entity self, ecs::Entity other) {
    const FactionComponent* a = world.get<FactionComponent>(self);
    const FactionComponent* b = world.get<FactionComponent>(other);
    if (a == nullptr || b == nullptr || !a->enabled || !b->enabled || a->team.empty() ||
        b->team.empty()) {
        return false;
    }
    return team_relation(a->team, b->team) == TeamRelation::Hostile;
}

// The victim's collider, in world space. False for an entity with no collider,
// an unbounded one (a plane has no meaningful height), or a broken transform.
// A hit zone is geometry first: an entity with no box is a body hit.
bool entity_collider_bounds(ecs::World& world, ecs::Entity entity, physics::Aabb& out) {
    const physics::ColliderComponent* col = world.get<physics::ColliderComponent>(entity);
    const scene::Transform* t = world.get<scene::Transform>(entity);
    if (col == nullptr || t == nullptr || col->shape.is_unbounded()) {
        return false;
    }
    col->shape.compute_aabb(Vec3{t->world_x, t->world_y, t->world_z}, t->world_rot, out.min,
                            out.max);
    return true;
}

// Where a world-space impact landed on the victim's collider. The classification
// is always geometric; whether the victim's HealthComponent honours it is a
// separate switch in health_apply_hit(). That split lets an event report a head
// hit even when the target's damage rules are still the old flat body model.
HitZone hit_zone_for_impact(ecs::World& world, ecs::Entity victim, const Vec3& point) {
    physics::Aabb bounds{};
    if (!entity_collider_bounds(world, victim, bounds)) {
        return HitZone::Body;
    }
    return hit_zone_from_height(point.y, bounds.min.y, bounds.max.y);
}

// Collects every finite scene collider into `out`, skipping one entity. The
// member Runtime::collect_hitscan_targets() uses this for a volley; a blast uses
// a LOCAL vector because it must exclude the direct victim without changing the
// boxes the next projectile in the same frame will trace against.
// Applies a weapon's authored status to a living victim. Called only after a
// hit was accepted, so an invulnerable target or a dead one does not catch
// fire. The component is created lazily; most scenes never carry one.
void apply_weapon_status(ecs::World& world, ecs::Entity shooter, ecs::Entity victim,
                         const WeaponProfile& profile) {
    if (profile.status == StatusKind::None) {
        return;
    }
    const HealthComponent* health = world.get<HealthComponent>(victim);
    if (health == nullptr || !health->enabled || health->invulnerable ||
        !health_alive(*health)) {
        return;
    }
    StatusEffectComponent* status = world.get<StatusEffectComponent>(victim);
    if (status == nullptr) {
        status = &world.add<StatusEffectComponent>(victim, StatusEffectComponent{});
    }
    if (status == nullptr || !status->enabled) {
        return;
    }
    const StatusEffect effect =
        make_status(profile.status, profile.status_damage_per_second, profile.status_seconds,
                    profile.status_tick_seconds, shooter);
    (void)status_add(status->effects, effect);
}

void collect_targets_into(ecs::World& world, ecs::Entity ignore,
                          std::vector<HitscanTarget>& out) {
    out.clear();
    for (ecs::Entity e : world.query<physics::ColliderComponent>()) {
        if (!world.is_alive(e)) {
            continue;
        }
        if (ignore.valid() && e.id == ignore.id && e.generation == ignore.generation) {
            continue;
        }
        physics::Aabb bounds{};
        if (!entity_collider_bounds(world, e, bounds)) {
            continue;
        }
        out.push_back(HitscanTarget{e, bounds});
    }
}

} // namespace

ecs::Entity Runtime::player_entity(ecs::World& world) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return ecs::kInvalidEntity;
    }
    CameraComponent* cam = active_camera(world);
    if (cam == nullptr || cam->follow_target.empty()) {
        return ecs::kInvalidEntity;
    }
    return entity_by_name(world, cam->follow_target);
}

void Runtime::collect_hitscan_targets(ecs::World& world, ecs::Entity ignore) {
    collect_targets_into(world, ignore, m_hitscan_targets);
}

u32 Runtime::fire_volley(ecs::Entity shooter, WeaponComponent& weapon, const CameraComponent& cam,
                         const Vec3& pivot, const WeaponStep& step) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || step.shots.empty()) {
        return 0u;
    }
    // The view the crosshair sits on: the authored orbit plus the live recoil.
    // Computing it here rather than reading the placed camera keeps the shot
    // consistent with the frame it belongs to — update_follow_camera runs
    // AFTER this, so its position is still last frame's.
    const f32 view_yaw = look_wrap_deg(cam.yaw + m_recoil.yaw);
    const f32 view_pitch = std::clamp(cam.pitch + m_recoil.pitch, cam.pitch_min, cam.pitch_max);
    const f32 aim_dist = aim_distance(cam.distance, cam.ads_distance, m_ads_blend);
    const Vec3 origin = follow_camera_position(pivot, view_yaw, view_pitch, aim_dist);
    // The shot leaves from the CAMERA and travels through the pivot, so the
    // crosshair is exactly what it covers. Firing from the character's hip
    // instead would miss by the parallax between the two.

    // The kick belongs to the SHOT, not to the pellet: a shotgun's eight
    // pellets climb the muzzle once. Applied before the trace so the camera
    // this frame shows the recoil of the shot this frame fired.
    const u32 index = weapon.state.shot_index > 0u ? weapon.state.shot_index - 1u : 0u;
    const RecoilKick kick = weapon_recoil_kick(weapon.profile, index);
    add_recoil(kick.pitch, kick.yaw);
    add_camera_shake(weapon.profile.shake);

    return fire_volley_along(shooter, weapon, origin, view_yaw, view_pitch, step);
}

u32 Runtime::fire_volley_along(ecs::Entity shooter, WeaponComponent& weapon, const Vec3& origin,
                               f32 base_yaw, f32 base_pitch, const WeaponStep& step) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || step.shots.empty()) {
        return 0u;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    const WeaponProfile& profile = weapon.profile;
    collect_hitscan_targets(world, shooter);

    for (usize shot_index = 0; shot_index < step.shots.size(); ++shot_index) {
        const WeaponShot& shot = step.shots[shot_index];
        // Spread is an ANGLE OFFSET, so it is applied to the aim and the
        // direction rebuilt from it: the cone is then measured in the same
        // degrees the camera uses, at every pitch, with no gimbal special case.
        const f32 yaw = base_yaw + shot.yaw_deg;
        const f32 pitch = std::clamp(base_pitch + shot.pitch_deg, -89.0f, 89.0f);
        const Vec3 direction = aim_direction(yaw, pitch);

        // Phase 53: publish the shot itself, before it hits or misses. A game
        // draws the muzzle flash and tracer from this; a hit is a separate
        // ImpactEvent. One event per pellet, with the spread that was traced.
        ShotEvent event;
        event.shooter = shooter;
        event.origin = origin;
        event.direction = direction;
        event.damage = profile.damage;
        event.pellet = static_cast<u32>(shot_index + 1u);
        event.pellets = static_cast<u32>(step.shots.size());
        event.projectile = profile.delivery == WeaponDelivery::Projectile;
        event.seed = profile.seed;
        m_shots.push(std::move(event));

        // Phase 46: a projectile is a body, not a ray. It leaves the muzzle
        // now and is traced frame by frame in step_projectiles; no damage is
        // applied until it actually arrives. The profile is snapshotted so an
        // edit after the shot cannot change a round already in the air.
        if (profile.delivery == WeaponDelivery::Projectile) {
            if (m_projectiles.size() >= kMaxActiveProjectiles) {
                // Oldest first: a scene that floods the cap keeps the newest
                // rounds, and the eviction order does not depend on a hash.
                m_projectiles.erase(m_projectiles.begin());
            }
            ActiveProjectile projectile;
            projectile.owner = shooter;
            projectile.profile = profile;
            projectile.body.position = origin;
            projectile.body.velocity = direction * profile.projectile_speed;
            projectile.body.lifetime_seconds =
                projectile_lifetime_for_range(profile.range, profile.projectile_speed);
            m_projectiles.push_back(projectile);
            m_shots_fired_total += 1u;
            continue;
        }

        const HitscanHit hit = hitscan_nearest(m_hitscan_targets, origin, direction, profile.range,
                                               shooter);
        m_last_weapon_hit = resolve_weapon_hit(shooter, profile, hit, hit.distance);
        if (hit.hit) {
            // A hitscan explosive (a grenade launcher modelled as a ray) still
            // blasts the space around the impact. The direct victim is skipped
            // by apply_explosion; its damage is the direct hit above.
            (void)apply_explosion(shooter, profile, hit.position, hit.entity);
        }
        m_shots_fired_total += 1u;
    }
    return static_cast<u32>(step.shots.size());
}

Runtime::WeaponHitInfo Runtime::resolve_weapon_hit(ecs::Entity shooter,
                                                   const WeaponProfile& profile,
                                                   const HitscanHit& hit,
                                                   f32 distance_travelled) {
    WeaponHitInfo info;
    info.hit = hit.hit;
    info.entity = hit.entity;
    info.position = hit.position;
    info.normal = hit.normal;
    info.distance = distance_travelled;
    if (!hit.hit || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return info;
    }

    ecs::World& world = m_scene_data_ptr->scene->world();
    const scene::NameComponent* n = world.get<scene::NameComponent>(hit.entity);
    if (n != nullptr) {
        info.name = n->name;
    }
    info.damage = weapon_damage_at(profile, distance_travelled);
    info.hit_zone = hit_zone_for_impact(world, hit.entity, hit.position);
    info.damage_type = profile.damage_type;

    const auto push_impact = [&](bool blocked, bool killed) {
        ImpactEvent event;
        event.shooter = shooter;
        event.victim = hit.entity;
        event.victim_name = info.name;
        event.position = hit.position;
        event.normal = hit.normal;
        event.distance = distance_travelled;
        event.damage = info.damage;
        event.applied = info.applied;
        event.armor_absorbed = info.armor_absorbed;
        event.hit_zone = info.hit_zone;
        event.damage_type = info.damage_type;
        event.blocked_by_faction = blocked;
        event.killed = killed;
        m_impacts.push(std::move(event));
    };

    // The social rule is checked before hit points and before destruction: a
    // round that strikes a teammate stops there, and a teammate's crate does
    // not explode either. The geometry hit is still counted; damage is not.
    if (!team_allows_damage(world, shooter, hit.entity)) {
        info.blocked_by_faction = true;
        m_hits_landed_total += 1u;
        push_impact(true, false);
        return info;
    }

    HealthComponent* victim_health = world.get<HealthComponent>(hit.entity);
    // Phase 50: a weapon hit is the layered path. It reads the zone and spends
    // armor; health_apply_damage() remains the raw path for scripts/hazards.
    DamageResult damage;
    if (victim_health != nullptr) {
        damage = health_apply_hit(*victim_health, info.damage, info.hit_zone,
                                  profile.armor_penetration, profile.damage_type);
        info.applied = damage.health_damage;
        info.armor_absorbed = damage.armor_absorbed;
    }
    m_hits_landed_total += 1u;
    m_damage_dealt_total += info.applied;
    m_armor_damage_total += info.armor_absorbed;
    const bool killed = damage.killed;
    if (damage.accepted) {
        apply_weapon_status(world, shooter, hit.entity, profile);
        record_damage(hit.entity, shooter, damage.health_damage, damage.armor_absorbed,
                      killed);
    }

    // A destructible takes the same shot as fracture damage: a small, sharp
    // blast at the impact point. The impulse is scaled from the bullet's damage
    // so a rifle opens a crate and a pistol does not.
    if (m_destruction && m_destruction_enabled &&
        world.get<DestructibleComponent>(hit.entity) != nullptr) {
        destruction::DamageEvent blast;
        blast.world_point = hit.position;
        blast.radius = 0.35f;
        blast.impulse = info.damage * 2.0f;
        apply_damage(hit.entity, blast);
    }
    push_impact(false, killed);
    return info;
}

void Runtime::step_weapons(f32 dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    CameraComponent* cam = active_camera(world);
    const ecs::Entity shooter = cam != nullptr ? player_entity(world) : ecs::kInvalidEntity;

    // The trigger and the reload button, read once for the whole scene. Both
    // are ordinary actions, so a scene can put them on a mouse button, a pad
    // button or a key without the runtime knowing which.
    WeaponInput input;
    if (shooter.valid() && m_playing && m_input_source != nullptr) {
        input.trigger_held = m_input_source->action_pressed("fire");
        const bool reload_down = m_input_source->action_pressed("reload");
        input.reload_pressed = reload_down && !m_reload_was_down;
        m_reload_was_down = reload_down;
    } else {
        m_reload_was_down = false;
    }

    // The pivot the shot leaves from: the target's head, in the target's own
    // space — the same point the camera orbits.
    Vec3 pivot{0.0f, 0.0f, 0.0f};
    bool have_pivot = false;
    if (shooter.valid() && cam != nullptr) {
        const scene::Transform* t = world.get<scene::Transform>(shooter);
        if (t != nullptr) {
            const Vec3 offset{cam->pivot_x, cam->pivot_y, cam->pivot_z};
            const Vec3 rotated = t->world_rot.rotate(offset);
            pivot = Vec3{t->world_x + rotated.x, t->world_y + rotated.y, t->world_z + rotated.z};
            have_pivot = true;
        }
    }

    // Every weapon ticks; the player's reads the input and an NPC's reads its
    // agent's trigger. An NPC's rifle therefore finishes its reload on its own
    // clock, fires only when its aim says so, and never fires because the
    // player happened to hold a button.
    for (ecs::Entity e : world.query<WeaponComponent>()) {
        WeaponComponent* w = world.get<WeaponComponent>(e);
        if (w == nullptr) {
            continue;
        }
        const bool is_player = shooter.valid() && e.id == shooter.id &&
                               e.generation == shooter.generation;
        CombatAgentComponent* agent = world.get<CombatAgentComponent>(e);
        WeaponInput in;
        if (is_player) {
            in = input;
        } else if (agent != nullptr && agent->enabled) {
            // The agent's trigger, decided this frame by step_combat_ai. A
            // semi-automatic weapon on an NPC therefore fires once per burst
            // phase rather than once per frame — the burst IS the cadence the
            // AI asked for.
            in.trigger_held = agent->state.trigger;
        }
        const WeaponStep step = weapon_step(w->state, w->profile, in, dt);

        if (is_player) {
            if (!step.shots.empty() && have_pivot && cam != nullptr) {
                fire_volley(shooter, *w, *cam, pivot, step);
            }
            continue;
        }
        if (agent != nullptr && agent->enabled && !step.shots.empty()) {
            const scene::Transform* t = world.get<scene::Transform>(e);
            if (t != nullptr) {
                const Vec3 eye{t->world_x, t->world_y + agent->eye_height, t->world_z};
                fire_volley_along(e, *w, eye, agent->state.yaw_deg, agent->state.pitch_deg, step);
            }
        }
    }
}

void Runtime::step_projectiles(f32 dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || m_projectiles.empty()) {
        return;
    }
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    // Every round sees the same boxes this frame. The owner is ignored per
    // trace rather than filtered out here, because different rounds can belong
    // to different shooters.
    collect_hitscan_targets(world, ecs::kInvalidEntity);

    usize index = 0u;
    while (index < m_projectiles.size()) {
        ActiveProjectile& projectile = m_projectiles[index];
        const f32 travelled_before = projectile.body.distance_travelled;
        const ProjectileAdvance advance =
            projectile_advance(projectile.body, Vec3{0.0f, -projectile.profile.projectile_gravity,
                                                     0.0f},
                               dt);

        bool remove = advance.expired;
        bool hit_something = false;
        const Vec3 segment = advance.to - advance.from;
        const f32 segment_length = segment.length();
        if (advance.moved && segment_length > 1e-6f) {
            // Sweep the segment, not the endpoint. A 900 m/s round covers 15 m
            // in a 60 fps frame, and a wall is thinner than that.
            const HitscanHit hit = hitscan_nearest(m_hitscan_targets, advance.from, segment,
                                                   segment_length + 1e-3f, projectile.owner);
            if (hit.hit) {
                // Falloff uses the path flown plus the part of this segment
                // before the impact, not the straight-line range to the
                // shooter: a dropping round really did travel farther.
                m_last_weapon_hit = resolve_weapon_hit(
                    projectile.owner, projectile.profile, hit,
                    travelled_before + hit.distance);
                // The blast happens at the surface, not at the muzzle. A round
                // that reaches its range without hitting anything still
                // detonates at the end of its flight below.
                (void)apply_explosion(projectile.owner, projectile.profile, hit.position,
                                      hit.entity);
                hit_something = true;
                remove = true;
            }
        }

        if (advance.expired && !hit_something) {
            (void)apply_explosion(projectile.owner, projectile.profile, advance.to,
                                  ecs::kInvalidEntity);
        }

        if (remove) {
            m_projectiles.erase(m_projectiles.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
}

void Runtime::step_status_effects(f32 dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    std::vector<StatusTick> ticks;

    for (ecs::Entity e : world.query<StatusEffectComponent>()) {
        StatusEffectComponent* status = world.get<StatusEffectComponent>(e);
        if (status == nullptr || !status->enabled || status->effects.count == 0u) {
            continue;
        }
        HealthComponent* health = world.get<HealthComponent>(e);
        if (health == nullptr || !health_alive(*health)) {
            status_clear(status->effects);
            continue;
        }

        ticks.clear();
        (void)status_step(status->effects, dt, ticks);
        if (ticks.empty()) {
            continue;
        }

        const scene::Transform* transform = world.get<scene::Transform>(e);
        const scene::NameComponent* name = world.get<scene::NameComponent>(e);
        for (const StatusTick& tick : ticks) {
            // A status remembers who applied it. A teammate's burn does not
            // damage a teammate unless the attacker licensed friendly fire.
            if (tick.source.valid() && !team_allows_damage(world, tick.source, e)) {
                continue;
            }
            const DamageResult result = health_apply_hit(*health, tick.damage, HitZone::Body,
                                                         0.0f, tick.damage_type);
            if (!result.accepted) {
                continue;
            }

            m_status_ticks_total += 1u;
            m_status_damage_total += result.health_damage;
            m_damage_dealt_total += result.health_damage;
            m_armor_damage_total += result.armor_absorbed;
            record_damage(e, tick.source, result.health_damage, result.armor_absorbed,
                          result.killed);

            ImpactEvent event;
            event.shooter = tick.source;
            event.victim = e;
            if (name != nullptr) {
                event.victim_name = name->name;
            }
            if (transform != nullptr) {
                event.position = Vec3{transform->world_x, transform->world_y,
                                      transform->world_z};
            }
            event.normal = Vec3{0.0f, 1.0f, 0.0f};
            event.distance = 0.0f;
            event.damage = tick.damage;
            event.applied = result.health_damage;
            event.armor_absorbed = result.armor_absorbed;
            event.hit_zone = HitZone::Body;
            event.damage_type = tick.damage_type;
            event.status = true;
            event.killed = result.killed;
            m_impacts.push(std::move(event));

            if (result.killed) {
                status_clear(status->effects);
                break;
            }
        }
        if (!health_alive(*health)) {
            status_clear(status->effects);
        }
    }
}

bool Runtime::apply_status(ecs::Entity entity, std::string_view kind, f32 damage_per_second,
                           f32 seconds, f32 tick_seconds) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    if (!world.is_alive(entity)) {
        return false;
    }
    StatusKind status_kind = StatusKind::None;
    if (!status_kind_from_name(kind, status_kind) || status_kind == StatusKind::None) {
        return false;
    }
    HealthComponent* health = world.get<HealthComponent>(entity);
    if (health == nullptr || !health->enabled || health->invulnerable ||
        !health_alive(*health)) {
        return false;
    }
    StatusEffectComponent* status = world.get<StatusEffectComponent>(entity);
    if (status == nullptr) {
        status = &world.add<StatusEffectComponent>(entity, StatusEffectComponent{});
    }
    if (status == nullptr || !status->enabled) {
        return false;
    }
    return status_add(status->effects,
                      make_status(status_kind, damage_per_second, seconds, tick_seconds));
}

f32 Runtime::status_remaining(ecs::Entity entity, std::string_view kind) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0.0f;
    }
    StatusKind status_kind = StatusKind::None;
    if (!status_kind_from_name(kind, status_kind) || status_kind == StatusKind::None) {
        return 0.0f;
    }
    ecs::World& world = const_cast<ecs::World&>(m_scene_data_ptr->scene->world());
    const StatusEffectComponent* component = world.get<StatusEffectComponent>(entity);
    return component != nullptr ? nf::runtime::status_remaining(component->effects, status_kind)
                                : 0.0f;
}

bool Runtime::clear_status(ecs::Entity entity, std::string_view kind) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }
    StatusKind status_kind = StatusKind::None;
    if (!status_kind_from_name(kind, status_kind) || status_kind == StatusKind::None) {
        return false;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    StatusEffectComponent* status = world.get<StatusEffectComponent>(entity);
    return status != nullptr && status_remove(status->effects, status_kind);
}

void Runtime::record_damage(ecs::Entity victim, ecs::Entity attacker, f32 health_damage,
                            f32 armor_damage, bool killed) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    if (!world.is_alive(victim)) {
        return;
    }
    DamageLedgerComponent* ledger = world.get<DamageLedgerComponent>(victim);
    if (ledger == nullptr) {
        ledger = &world.add<DamageLedgerComponent>(victim, DamageLedgerComponent{});
    }
    if (ledger != nullptr) {
        damage_ledger_record(ledger->ledger, attacker, health_damage, armor_damage, killed);
    }
}

void Runtime::step_damage_ledgers(f32 dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || !(dt > 0.0f) ||
        !std::isfinite(dt)) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    for (ecs::Entity e : world.query<DamageLedgerComponent>()) {
        DamageLedgerComponent* ledger = world.get<DamageLedgerComponent>(e);
        if (ledger != nullptr) {
            damage_ledger_step(ledger->ledger, dt);
        }
    }
}

void Runtime::step_suppression(f32 dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || !(dt > 0.0f) ||
        !std::isfinite(dt)) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    std::vector<ecs::Entity> agents;
    for (ecs::Entity e : world.query<CombatAgentComponent>()) {
        CombatAgentComponent* agent = world.get<CombatAgentComponent>(e);
        if (agent == nullptr || !agent->enabled) {
            continue;
        }
        suppression_step(agent->suppression, dt);
        agents.push_back(e);
    }
    if (agents.empty() || m_shots.current().empty()) {
        return;
    }

    for (const ShotEvent& shot : m_shots.current()) {
        if (!shot.shooter.valid()) {
            continue;
        }
        for (ecs::Entity e : agents) {
            if (e.id == shot.shooter.id && e.generation == shot.shooter.generation) {
                continue;
            }
            CombatAgentComponent* agent = world.get<CombatAgentComponent>(e);
            const scene::Transform* transform = world.get<scene::Transform>(e);
            if (agent == nullptr || transform == nullptr) {
                continue;
            }
            // A teammate's near miss is not suppressive fire.
            if (!team_allows_damage(world, shot.shooter, e)) {
                continue;
            }
            const Vec3 point{transform->world_x, transform->world_y + agent->target_height,
                             transform->world_z};
            const f32 distance =
                distance_point_to_ray(point, shot.origin, shot.direction, 200.0f);
            if (distance <= kSuppressionRadius) {
                suppression_add(agent->suppression, kSuppressionPerShot);
            }
        }
    }
}

const DamageLedgerComponent* Runtime::damage_ledger_of(ecs::Entity entity) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return nullptr;
    }
    ecs::World& world = const_cast<ecs::World&>(m_scene_data_ptr->scene->world());
    return world.get<DamageLedgerComponent>(entity);
}

f32 Runtime::suppression_of(ecs::Entity entity) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0.0f;
    }
    ecs::World& world = const_cast<ecs::World&>(m_scene_data_ptr->scene->world());
    const CombatAgentComponent* agent = world.get<CombatAgentComponent>(entity);
    return agent != nullptr ? agent->suppression.level : 0.0f;
}

u32 Runtime::apply_explosion(ecs::Entity shooter, const WeaponProfile& profile,
                             const Vec3& center, ecs::Entity direct_victim) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0u;
    }
    const ExplosionFalloff blast{profile.blast_radius, profile.blast_damage,
                                 profile.blast_min_scale};
    if (!explosion_valid(blast) || !std::isfinite(center.x) || !std::isfinite(center.y) ||
        !std::isfinite(center.z)) {
        return 0u;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    // The direct victim already took the weapon's own hit. Excluding its box
    // from the LOS set matters as much as excluding its health: a blast centred
    // on its surface would otherwise be "blocked" by the victim itself for
    // every other entity in the scene.
    std::vector<HitscanTarget> targets;
    collect_targets_into(world, direct_victim, targets);

    m_explosions_total += 1u;
    u32 reached = 0u;

    for (ecs::Entity victim : world.all_entities()) {
        if (!world.is_alive(victim)) {
            continue;
        }
        if (direct_victim.valid() && victim.id == direct_victim.id &&
            victim.generation == direct_victim.generation) {
            continue;
        }
        HealthComponent* health = world.get<HealthComponent>(victim);
        const bool destructible = world.get<DestructibleComponent>(victim) != nullptr;
        if (health == nullptr && !destructible) {
            continue;
        }
        const scene::Transform* transform = world.get<scene::Transform>(victim);
        if (transform == nullptr) {
            continue;
        }

        physics::Aabb bounds{};
        const bool bounded = entity_collider_bounds(world, victim, bounds);
        Vec3 closest{transform->world_x, transform->world_y, transform->world_z};
        f32 distance = 0.0f;
        if (bounded) {
            closest = Vec3{std::clamp(center.x, bounds.min.x, bounds.max.x),
                           std::clamp(center.y, bounds.min.y, bounds.max.y),
                           std::clamp(center.z, bounds.min.z, bounds.max.z)};
            distance = distance_point_to_aabb(center, bounds.min, bounds.max);
        } else {
            distance = (closest - center).length();
        }
        if (!(distance <= blast.radius)) {
            continue;
        }

        // A wall stops a blast. The centre of the blast is on the direct
        // victim's surface, which is why that box was removed from `targets`.
        if (bounded && distance > 1e-4f) {
            const Vec3 delta = closest - center;
            const f32 length = delta.length();
            if (length > 1e-4f) {
                const HitscanHit sight =
                    hitscan_nearest(targets, center, delta, length + 0.05f, ecs::kInvalidEntity);
                if (!sight.hit || sight.entity != victim) {
                    continue;
                }
            }
        }

        const f32 damage = explosion_damage_at(blast, distance);
        if (!(damage > 0.0f)) {
            continue;
        }

        const bool allowed = team_allows_damage(world, shooter, victim);
        f32 applied = 0.0f;
        f32 armor_absorbed = 0.0f;
        bool killed = false;
        if (allowed) {
            if (health != nullptr) {
                const DamageResult result = health_apply_hit(*health, damage, HitZone::Body,
                                                              profile.armor_penetration,
                                                              profile.damage_type);
                applied = result.health_damage;
                armor_absorbed = result.armor_absorbed;
                killed = result.killed;
                if (result.accepted) {
                    apply_weapon_status(world, shooter, victim, profile);
                    record_damage(victim, shooter, result.health_damage,
                                  result.armor_absorbed, killed);
                }
            }
            if (m_destruction && m_destruction_enabled && destructible) {
                destruction::DamageEvent shards;
                shards.world_point = center;
                shards.radius = profile.blast_radius;
                shards.impulse = damage * 2.0f;
                apply_damage(victim, shards);
            }
        }

        ImpactEvent event;
        event.shooter = shooter;
        event.victim = victim;
        const scene::NameComponent* name = world.get<scene::NameComponent>(victim);
        if (name != nullptr) {
            event.victim_name = name->name;
        }
        event.position = center;
        event.normal = distance > 1e-4f ? (closest - center) / distance : Vec3{0.0f, 1.0f, 0.0f};
        event.distance = distance;
        event.damage = damage;
        event.applied = applied;
        event.armor_absorbed = armor_absorbed;
        event.hit_zone = HitZone::Body;
        event.splash = true;
        event.damage_type = profile.damage_type;
        event.blocked_by_faction = !allowed;
        event.killed = killed;
        m_impacts.push(std::move(event));

        if (allowed) {
            m_damage_dealt_total += applied;
            m_armor_damage_total += armor_absorbed;
            m_splash_hits_total += 1u;
            ++reached;
        }
    }
    return reached;
}

void Runtime::step_combat_ai(f32 dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    // The player is not an AI, even if a scene puts an agent on the same
    // entity: the trigger belongs to the person holding the mouse.
    const ecs::Entity player = player_entity(world);

    for (ecs::Entity e : world.query<CombatAgentComponent>()) {
        CombatAgentComponent* agent = world.get<CombatAgentComponent>(e);
        if (agent == nullptr || !agent->enabled) {
            continue;
        }
        if (player.valid() && e.id == player.id && e.generation == player.generation) {
            continue;
        }
        // No gun, no fight: an agent without a WeaponComponent has nothing to
        // aim, and stepping its aim would only produce numbers nobody reads.
        WeaponComponent* weapon = world.get<WeaponComponent>(e);
        if (weapon == nullptr) {
            continue;
        }
        const scene::Transform* self = world.get<scene::Transform>(e);
        if (self == nullptr) {
            continue;
        }

        // While idle, the agent faces wherever its Transform faces. Reading the
        // transform every idle frame (rather than once) means dragging the
        // entity in the editor turns it, which is what an author expects.
        if (!agent->state.engaged) {
            agent->state.yaw_deg = self->rot_y;
            agent->state.pitch_deg = 0.0f;
        }

        const Vec3 eye{self->world_x, self->world_y + agent->eye_height, self->world_z};

        CombatAimInput in;
        in.eye = eye;
        in.dt = dt;
        in.suppression = agent->suppression.level;

        // Every candidate is checked against the same world boxes. Rebuilding
        // once per agent (rather than once per candidate) keeps auto-targeting
        // a linear scan, not a scene walk per enemy.
        collect_hitscan_targets(world, e);

        // A target is engageable when it is alive, inside range, allowed by the
        // social rules and actually visible. `auto_mode` is the difference
        // between "the name the author wrote" (neutral entities allowed) and
        // "someone on an opposing team" (only real hostile factions).
        const auto evaluate_target = [&](ecs::Entity candidate, bool auto_mode, Vec3& out_aim,
                                         f32& out_distance) {
            if (!candidate.valid() || candidate == e) {
                return false;
            }
            const scene::Transform* transform = world.get<scene::Transform>(candidate);
            const HealthComponent* health = world.get<HealthComponent>(candidate);
            if (transform == nullptr || health == nullptr || !health_alive(*health)) {
                return false;
            }
            if (auto_mode) {
                if (!hostile_faction_pair(world, e, candidate)) {
                    return false;
                }
            } else if (!team_allows_damage(world, e, candidate)) {
                return false;
            }
            const Vec3 aim{transform->world_x, transform->world_y + agent->target_height,
                           transform->world_z};
            const Vec3 delta = aim - eye;
            const f32 distance = delta.length();
            if (!(distance > 1e-4f) || distance > agent->config.max_range) {
                return false;
            }
            // Visibility is a ray from the eye to the aim point: the first
            // thing it hits must be the target. A ray that hits nothing means
            // the target has no collider, and an agent that shoots through
            // walls is worse than one that never fires.
            const HitscanHit sight =
                hitscan_nearest(m_hitscan_targets, eye, delta, distance + 0.1f, e);
            if (!sight.hit || sight.entity != candidate) {
                return false;
            }
            out_aim = aim;
            out_distance = distance;
            return true;
        };

        ecs::Entity target = ecs::kInvalidEntity;
        Vec3 target_aim{0.0f, 0.0f, 0.0f};

        // Retaliation (Phase 55): if this agent opted in and its ledger names a
        // still-valid attacker, that attacker is the first candidate. The
        // normal authored/auto target remains the fallback when the attacker
        // is dead, hidden or out of range.
        if (agent->retaliate) {
            const DamageLedgerComponent* ledger = world.get<DamageLedgerComponent>(e);
            if (ledger != nullptr && ledger->ledger.has_damage &&
                ledger->ledger.last_attacker.valid()) {
                f32 distance = 0.0f;
                Vec3 aim{0.0f, 0.0f, 0.0f};
                if (evaluate_target(ledger->ledger.last_attacker, agent->auto_target, aim,
                                    distance)) {
                    target = ledger->ledger.last_attacker;
                    target_aim = aim;
                }
            }
        }

        if (!target.valid()) {
            if (agent->auto_target) {
                f32 held_distance = 0.0f;
                // Keep the current target while it remains valid. Without the
                // lock, two enemies at almost the same distance make the agent
                // twitch between them every frame and never finish a burst.
                if (evaluate_target(agent->locked_target, true, target_aim, held_distance)) {
                    target = agent->locked_target;
                } else {
                    f32 best_distance = std::numeric_limits<f32>::infinity();
                    Vec3 best_aim{0.0f, 0.0f, 0.0f};
                    for (ecs::Entity candidate : world.all_entities()) {
                        Vec3 aim{0.0f, 0.0f, 0.0f};
                        f32 distance = 0.0f;
                        if (!evaluate_target(candidate, true, aim, distance)) {
                            continue;
                        }
                        const bool closer = distance < best_distance;
                        const bool tie = distance == best_distance &&
                                         (!target.valid() || candidate.id < target.id);
                        if (closer || tie) {
                            best_distance = distance;
                            best_aim = aim;
                            target = candidate;
                        }
                    }
                    target_aim = best_aim;
                }
            } else {
                f32 distance = 0.0f;
                const ecs::Entity named = entity_by_name(world, agent->target);
                if (evaluate_target(named, false, target_aim, distance)) {
                    target = named;
                }
            }
        }

        // A changed target is a new notice for both modes: reset the reaction
        // clock, the burst and the motion history. The aim itself is kept
        // (setting engaged=false would snap the agent back to its Transform's
        // facing for one frame).
        if (target != agent->locked_target) {
            agent->locked_target = target;
            agent->state.reaction_timer = 0.0f;
            agent->state.burst_timer = 0.0f;
            agent->state.trigger = false;
            agent->has_target_history = false;
            agent->target_velocity = Vec3{0.0f, 0.0f, 0.0f};
        }

        if (target.valid()) {
            // Projectile lead (Phase 48): a slow round aimed at where a moving
            // target IS arrives behind it. The velocity is measured from the
            // previous aim point; the first frame after acquiring a target has
            // no history, so it aims at the target and starts learning.
            Vec3 aim = target_aim;
            if (agent->has_target_history && dt > 0.0f) {
                const Vec3 raw_velocity = (target_aim - agent->last_target_position) / dt;
                const f32 speed_of_target = raw_velocity.length();
                // A teleport or a scene edit is not a velocity. Ignore an
                // implausible spike rather than leading a target into orbit.
                if (std::isfinite(speed_of_target) && speed_of_target <= 250.0f) {
                    agent->target_velocity = raw_velocity;
                } else {
                    agent->target_velocity = Vec3{0.0f, 0.0f, 0.0f};
                }
            }
            if (weapon->profile.delivery == WeaponDelivery::Projectile) {
                const ProjectileIntercept intercept =
                    projectile_intercept(eye, target_aim, agent->target_velocity,
                                         weapon->profile.projectile_speed);
                if (intercept.valid) {
                    aim = intercept.point;
                }
            }
            agent->last_target_position = target_aim;
            agent->has_target_history = true;

            in.has_target = true;
            in.target = aim;
            in.visible = true;
        } else {
            agent->has_target_history = false;
            agent->target_velocity = Vec3{0.0f, 0.0f, 0.0f};
        }

        const CombatAimOutput out = combat_aim_step(agent->state, agent->config, in);
        (void)out;
    }
}

Runtime::WeaponSnapshot Runtime::weapon_snapshot() const {
    WeaponSnapshot out;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return out;
    }
    ecs::World& world = const_cast<ecs::World&>(m_scene_data_ptr->scene->world());
    const ecs::Entity shooter = player_entity(world);
    if (!shooter.valid()) {
        return out;
    }
    const WeaponComponent* w = world.get<WeaponComponent>(shooter);
    if (w == nullptr) {
        return out;
    }
    out.has_weapon = true;
    out.enabled = w->profile.enabled;
    out.mode = w->profile.mode;
    out.ammo = w->state.in_magazine;
    out.magazine = w->profile.magazine;
    out.reserve = w->state.reserve;
    out.reloading = w->state.reloading;
    out.reload_progress =
        (w->state.reloading && w->profile.reload_seconds > 0.0f)
            ? std::clamp(1.0f - w->state.reload_timer / w->profile.reload_seconds, 0.0f, 1.0f)
            : 0.0f;
    out.spread_deg = weapon_spread_deg(w->state, w->profile);
    out.shots_fired = m_shots_fired_total;
    out.hits_landed = m_hits_landed_total;
    out.active_projectiles = static_cast<u32>(m_projectiles.size());
    out.damage_dealt = m_damage_dealt_total;
    out.armor_damage_dealt = m_armor_damage_total;
    out.explosions = m_explosions_total;
    out.splash_hits = m_splash_hits_total;
    out.status_ticks = m_status_ticks_total;
    out.status_damage_dealt = m_status_damage_total;
    return out;
}

bool Runtime::weapon_reload() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    const ecs::Entity shooter = player_entity(world);
    if (!shooter.valid()) {
        return false;
    }
    WeaponComponent* w = world.get<WeaponComponent>(shooter);
    if (w == nullptr) {
        return false;
    }
    WeaponStep step;
    return weapon_begin_reload(w->state, w->profile, step);
}

u32 Runtime::fire_weapon() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0u;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();
    CameraComponent* cam = active_camera(world);
    if (cam == nullptr) {
        return 0u;
    }
    const ecs::Entity shooter = player_entity(world);
    if (!shooter.valid()) {
        return 0u;
    }
    WeaponComponent* w = world.get<WeaponComponent>(shooter);
    const scene::Transform* t = world.get<scene::Transform>(shooter);
    if (w == nullptr || t == nullptr) {
        return 0u;
    }
    const Vec3 offset{cam->pivot_x, cam->pivot_y, cam->pivot_z};
    const Vec3 rotated = t->world_rot.rotate(offset);
    const Vec3 pivot{t->world_x + rotated.x, t->world_y + rotated.y, t->world_z + rotated.z};

    WeaponStep step;
    if (!weapon_fire_now(w->state, w->profile, step)) {
        return 0u;
    }
    return fire_volley(shooter, *w, *cam, pivot, step);
}

f32 Runtime::apply_health_damage(ecs::Entity entity, f32 amount) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0.0f;
    }
    HealthComponent* h = m_scene_data_ptr->scene->world().get<HealthComponent>(entity);
    if (h == nullptr) {
        return 0.0f;
    }
    return health_apply_damage(*h, amount);
}

const HealthComponent* Runtime::health_of(ecs::Entity entity) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return nullptr;
    }
    ecs::World& world = const_cast<ecs::World&>(m_scene_data_ptr->scene->world());
    return world.get<HealthComponent>(entity);
}

} // namespace nf::runtime
