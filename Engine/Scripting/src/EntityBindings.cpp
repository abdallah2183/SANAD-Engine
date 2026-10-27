// NF/Scripting/EntityBindings.cpp — nf.entity_* against an ecs::World.

#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>

#include <string_view>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
} // extern "C"

namespace nf::scripting {

namespace {

ecs::World* bound_world(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_world");
    auto* w = static_cast<ecs::World*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return w;
}

bool check_entity(lua_State* L, int base, ecs::Entity& out) {
    const lua_Integer id = luaL_checkinteger(L, base);
    const lua_Integer gen = luaL_checkinteger(L, base + 1);
    if (id < 0 || gen < 0) {
        luaL_error(L, "entity id/generation must be non-negative");
        return false;
    }
    out.id = static_cast<u32>(id);
    out.generation = static_cast<u32>(gen);
    return true;
}

int l_entity_alive(lua_State* L) {
    ecs::World* w = bound_world(L);
    ecs::Entity e;
    if (!check_entity(L, 1, e) || !w) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, (w->is_alive(e) ? 1 : 0));
    return 1;
}

int l_entity_pos(lua_State* L) {
    ecs::World* w = bound_world(L);
    ecs::Entity e;
    if (!check_entity(L, 1, e) || !w) return 0;
    const scene::Transform* t = w->get<scene::Transform>(e);
    if (!t) return 0; // nil: no transform (scripts test for nil)
    lua_pushnumber(L, t->local_x);
    lua_pushnumber(L, t->local_y);
    lua_pushnumber(L, t->local_z);
    return 3;
}

int l_set_entity_pos(lua_State* L) {
    ecs::World* w = bound_world(L);
    ecs::Entity e;
    if (!check_entity(L, 1, e) || !w) {
        lua_pushboolean(L, 0);
        return 1;
    }
    scene::Transform* t = w->get<scene::Transform>(e);
    if (!t) {
        lua_pushboolean(L, 0);
        return 1;
    }
    t->local_x = static_cast<float>(luaL_checknumber(L, 3));
    t->local_y = static_cast<float>(luaL_checknumber(L, 4));
    t->local_z = static_cast<float>(luaL_checknumber(L, 5));
    t->dirty = true;
    lua_pushboolean(L, 1);
    return 1;
}

int l_set_entity_rot(lua_State* L) {
    ecs::World* w = bound_world(L);
    ecs::Entity e;
    if (!check_entity(L, 1, e) || !w) {
        lua_pushboolean(L, 0);
        return 1;
    }
    scene::Transform* t = w->get<scene::Transform>(e);
    if (!t) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // The yaw, in degrees. A character that walks sideways because nothing
    // told it which way it was going is the most visible thing a third-person
    // game can get wrong, and the script is the only thing that knows which
    // way the player pressed.
    t->rot_y = static_cast<float>(luaL_checknumber(L, 3));
    t->dirty = true;
    lua_pushboolean(L, 1);
    return 1;
}

int l_find_entity(lua_State* L) {
    ecs::World* w = bound_world(L);
    const char* name = luaL_checkstring(L, 1);
    if (!w || name == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    for (ecs::Entity e : w->all_entities()) {
        if (!w->is_alive(e)) {
            continue;
        }
        const scene::NameComponent* n = w->get<scene::NameComponent>(e);
        if (n != nullptr && n->name == name) {
            lua_pushinteger(L, static_cast<lua_Integer>(e.id));
            lua_pushinteger(L, static_cast<lua_Integer>(e.generation));
            return 2;
        }
    }
    lua_pushnil(L);
    return 1;
}

int l_entity_name(lua_State* L) {
    ecs::World* w = bound_world(L);
    ecs::Entity e;
    if (!check_entity(L, 1, e) || !w || !w->is_alive(e)) {
        lua_pushnil(L);
        return 1;
    }
    const scene::NameComponent* n = w->get<scene::NameComponent>(e);
    if (n == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, n->name.data(), n->name.size());
    return 1;
}

int l_entity_count(lua_State* L) {
    ecs::World* w = bound_world(L);
    lua_pushinteger(L, w ? static_cast<lua_Integer>(w->alive_entity_count()) : 0);
    return 1;
}

const ScriptInput* bound_input(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_input");
    auto* input = static_cast<const ScriptInput*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return input;
}

int l_action(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const char* name = luaL_checkstring(L, 1);
    const bool down = (input != nullptr && input->pressed && name != nullptr)
                          ? input->pressed(std::string_view(name))
                          : false;
    lua_pushboolean(L, down ? 1 : 0);
    return 1;
}

int l_axis(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const char* name = luaL_checkstring(L, 1);
    const f32 value = (input != nullptr && input->axis && name != nullptr)
                          ? input->axis(std::string_view(name))
                          : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return 1;
}

// nf.camera_yaw() -> degrees, or 0 when the scene has no camera.
//
// A script that turns the character to face where it walks needs the same
// yaw the camera is using, or the character walks backwards into the camera
// while the player looks the other way.
int l_camera_yaw(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const f32 yaw = (input != nullptr && input->camera_yaw) ? input->camera_yaw() : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(yaw));
    return 1;
}

// nf.camera_move() -> x, z in world space.
//
// The two axes the player pressed, rotated by the camera's yaw: pressing W
// moves away from the camera wherever the camera is pointing. Without a
// camera the raw axes ARE the world axes, which is what every script did
// before this existed — so a script can call this unconditionally.
int l_camera_move(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    f32 x = 0.0f;
    f32 z = 0.0f;
    if (input != nullptr && input->camera_move) {
        input->camera_move(x, z);
    } else if (input != nullptr && input->axis) {
        x = input->axis("move_x");
        z = input->axis("move_z");
    }
    lua_pushnumber(L, static_cast<lua_Number>(x));
    lua_pushnumber(L, static_cast<lua_Number>(z));
    return 2;
}

// nf.shake(amount) -> nothing.
//
// Trauma in [0, 1]: 0.2 is a footstep, 0.6 a landed hit, 1.0 an explosion.
// The camera's own trim (shake_strength / shake_decay) decides what it looks
// like, so the same number works for a handheld camera and a tank.
int l_shake(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    if (input != nullptr && input->shake) {
        const f32 amount = static_cast<f32>(luaL_checknumber(L, 1));
        input->shake(amount);
    }
    return 0;
}

// nf.recoil(pitch, yaw) -> nothing.
//
// Degrees of kick: 1.2 pitch and 0.3 yaw is a rifle shot. The camera recovers
// on its own and the view returns to where the player was aiming, so a weapon
// script adds a kick per shot and never has to undo it.
int l_recoil(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    if (input != nullptr && input->recoil) {
        const f32 pitch = static_cast<f32>(luaL_checknumber(L, 1));
        const f32 yaw = static_cast<f32>(luaL_optnumber(L, 2, 0.0));
        input->recoil(pitch, yaw);
    }
    return 0;
}

// --- the gun and hit points (Phase 44) ---------------------------------------
//
// These are READS of a weapon the runtime already stepped, plus two verbs
// (reload, fire) that the weapon itself may refuse. A script never owns the
// fire-rate clock: it asks, and the same cadence that governs the trigger
// governs the script, so a Lua turret and a player's rifle cannot drift apart.

int l_weapon_ammo(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const f32 ammo = (input != nullptr && input->weapon_ammo) ? input->weapon_ammo() : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(ammo));
    return 1;
}

int l_weapon_reserve(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const f32 reserve = (input != nullptr && input->weapon_reserve) ? input->weapon_reserve() : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(reserve));
    return 1;
}

int l_weapon_reloading(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const bool reloading =
        (input != nullptr && input->weapon_reloading) ? input->weapon_reloading() : false;
    lua_pushboolean(L, reloading ? 1 : 0);
    return 1;
}

int l_weapon_reload(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const bool started = (input != nullptr && input->weapon_reload) ? input->weapon_reload() : false;
    lua_pushboolean(L, started ? 1 : 0);
    return 1;
}

int l_weapon_fire(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const f32 traced = (input != nullptr && input->weapon_fire) ? input->weapon_fire() : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(traced));
    return 1;
}

// nf.last_hit() -> hit, x, y, z, distance.
int l_last_hit(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    bool hit = false;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 distance = 0.0f;
    if (input != nullptr && input->last_hit) {
        input->last_hit(hit, x, y, z, distance);
    }
    lua_pushboolean(L, hit ? 1 : 0);
    lua_pushnumber(L, static_cast<lua_Number>(x));
    lua_pushnumber(L, static_cast<lua_Number>(y));
    lua_pushnumber(L, static_cast<lua_Number>(z));
    lua_pushnumber(L, static_cast<lua_Number>(distance));
    return 5;
}

// nf.impact_count() -> number of impacts in the previous completed frame.
int l_impact_count(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const u32 count = (input != nullptr && input->impact_count) ? input->impact_count() : 0u;
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    return 1;
}

// nf.impact(i) -> table, or nil. Lua indices are 1-based; the runtime's vector
// is 0-based, so the conversion happens here and nowhere else.
int l_impact(lua_State* L) {
    const lua_Integer index = luaL_checkinteger(L, 1);
    const ScriptInput* input = bound_input(L);
    ScriptImpact impact;
    if (index < 1 || input == nullptr || !input->impact_at ||
        !input->impact_at(static_cast<u32>(index - 1), impact)) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 15);
    const auto set_int = [L](const char* key, lua_Integer value) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, key);
    };
    const auto set_num = [L](const char* key, lua_Number value) {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, key);
    };
    const auto set_bool = [L](const char* key, bool value) {
        lua_pushboolean(L, value ? 1 : 0);
        lua_setfield(L, -2, key);
    };
    set_int("shooter", static_cast<lua_Integer>(impact.shooter_id));
    set_int("shooter_gen", static_cast<lua_Integer>(impact.shooter_generation));
    set_int("target", static_cast<lua_Integer>(impact.target_id));
    set_int("target_gen", static_cast<lua_Integer>(impact.target_generation));
    lua_pushlstring(L, impact.target_name.data(),
                    static_cast<size_t>(impact.target_name.size()));
    lua_setfield(L, -2, "name");
    set_num("x", static_cast<lua_Number>(impact.x));
    set_num("y", static_cast<lua_Number>(impact.y));
    set_num("z", static_cast<lua_Number>(impact.z));
    set_num("nx", static_cast<lua_Number>(impact.nx));
    set_num("ny", static_cast<lua_Number>(impact.ny));
    set_num("nz", static_cast<lua_Number>(impact.nz));
    set_num("distance", static_cast<lua_Number>(impact.distance));
    set_num("damage", static_cast<lua_Number>(impact.damage));
    set_num("applied", static_cast<lua_Number>(impact.applied));
    lua_pushlstring(L, impact.hit_zone.data(), impact.hit_zone.size());
    lua_setfield(L, -2, "zone");
    set_num("armor_absorbed", static_cast<lua_Number>(impact.armor_absorbed));
    lua_pushlstring(L, impact.damage_type.data(), impact.damage_type.size());
    lua_setfield(L, -2, "damage_type");
    set_bool("blocked", impact.blocked_by_faction);
    set_bool("killed", impact.killed);
    set_bool("splash", impact.splash);
    set_bool("status", impact.status);
    return 1;
}

// nf.shot_count() -> number of shots in the previous completed frame.
int l_shot_count(lua_State* L) {
    const ScriptInput* input = bound_input(L);
    const u32 count = (input != nullptr && input->shot_count) ? input->shot_count() : 0u;
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    return 1;
}

// nf.shot(i) -> table, or nil. 1-based, like nf.impact.
int l_shot(lua_State* L) {
    const lua_Integer index = luaL_checkinteger(L, 1);
    const ScriptInput* input = bound_input(L);
    ScriptShot shot;
    if (index < 1 || input == nullptr || !input->shot_at ||
        !input->shot_at(static_cast<u32>(index - 1), shot)) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 13);
    const auto set_int = [L](const char* key, lua_Integer value) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, key);
    };
    const auto set_num = [L](const char* key, lua_Number value) {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, key);
    };
    const auto set_bool = [L](const char* key, bool value) {
        lua_pushboolean(L, value ? 1 : 0);
        lua_setfield(L, -2, key);
    };
    set_int("shooter", static_cast<lua_Integer>(shot.shooter_id));
    set_int("shooter_gen", static_cast<lua_Integer>(shot.shooter_generation));
    set_num("x", static_cast<lua_Number>(shot.x));
    set_num("y", static_cast<lua_Number>(shot.y));
    set_num("z", static_cast<lua_Number>(shot.z));
    set_num("dx", static_cast<lua_Number>(shot.dx));
    set_num("dy", static_cast<lua_Number>(shot.dy));
    set_num("dz", static_cast<lua_Number>(shot.dz));
    set_num("damage", static_cast<lua_Number>(shot.damage));
    set_int("pellet", static_cast<lua_Integer>(shot.pellet));
    set_int("pellets", static_cast<lua_Integer>(shot.pellets));
    set_int("seed", static_cast<lua_Integer>(shot.seed));
    set_bool("projectile", shot.projectile);
    return 1;
}

// nf.apply_status(id, gen, kind, dps, seconds, tick=0.5) -> bool.
int l_apply_status(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const char* kind = luaL_checkstring(L, 3);
    const f32 dps = static_cast<f32>(luaL_checknumber(L, 4));
    const f32 seconds = static_cast<f32>(luaL_checknumber(L, 5));
    const f32 tick = static_cast<f32>(luaL_optnumber(L, 6, 0.5));
    const ScriptInput* input = bound_input(L);
    const bool ok = (input != nullptr && input->apply_status)
                        ? input->apply_status(e.id, e.generation, kind, dps, seconds, tick)
                        : false;
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// nf.status_remaining(id, gen, kind) -> seconds, 0 when absent.
int l_status_remaining(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    const char* kind = luaL_checkstring(L, 3);
    const ScriptInput* input = bound_input(L);
    const f32 seconds = (input != nullptr && input->status_remaining)
                            ? input->status_remaining(e.id, e.generation, kind)
                            : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(seconds));
    return 1;
}

// nf.clear_status(id, gen, kind) -> bool.
int l_clear_status(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const char* kind = luaL_checkstring(L, 3);
    const ScriptInput* input = bound_input(L);
    const bool ok = (input != nullptr && input->clear_status)
                        ? input->clear_status(e.id, e.generation, kind)
                        : false;
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// nf.last_attacker(id, gen) -> id, gen, or nil when the ledger has no hit.
int l_last_attacker(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    u32 id = 0u;
    u32 generation = 0u;
    if (input == nullptr || !input->last_attacker ||
        !input->last_attacker(e.id, e.generation, id, generation)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    lua_pushinteger(L, static_cast<lua_Integer>(generation));
    return 2;
}

// nf.killer(id, gen) -> id, gen, or nil until a recorded hit killed it.
int l_killer(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    u32 id = 0u;
    u32 generation = 0u;
    if (input == nullptr || !input->killer ||
        !input->killer(e.id, e.generation, id, generation)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    lua_pushinteger(L, static_cast<lua_Integer>(generation));
    return 2;
}

// nf.damage_taken(id, gen) -> health, armor, hits, seconds_since_damage.
int l_damage_taken(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushnumber(L, 0.0);
        lua_pushnumber(L, 0.0);
        lua_pushinteger(L, 0);
        lua_pushnumber(L, 0.0);
        return 4;
    }
    const ScriptInput* input = bound_input(L);
    f32 health = 0.0f;
    f32 armor = 0.0f;
    u32 hits = 0u;
    f32 seconds = 0.0f;
    if (input != nullptr && input->damage_taken) {
        input->damage_taken(e.id, e.generation, health, armor, hits, seconds);
    }
    lua_pushnumber(L, static_cast<lua_Number>(health));
    lua_pushnumber(L, static_cast<lua_Number>(armor));
    lua_pushinteger(L, static_cast<lua_Integer>(hits));
    lua_pushnumber(L, static_cast<lua_Number>(seconds));
    return 4;
}

// nf.suppression(id, gen) -> level in [0, 1].
int l_suppression(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    const f32 level = (input != nullptr && input->suppression)
                          ? input->suppression(e.id, e.generation)
                          : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(level));
    return 1;
}

// The entity running this script, from the registry fields ScriptSystem sets
// before each update(). False when the script is running outside an entity
// (a test calling a chunk directly), which makes every perception query below
// answer "no" instead of reading whatever the last entity happened to be.
bool self_entity(lua_State* L, ecs::Entity& out) {
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_self_id");
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_self_gen");
    const lua_Integer id = lua_tointeger(L, -2);
    const lua_Integer gen = lua_tointeger(L, -1);
    lua_pop(L, 2);
    if (id <= 0) {
        return false;
    }
    out = ecs::Entity{static_cast<u32>(id), static_cast<u32>(gen)};
    return true;
}

// --- Perception (Phase 60) ---------------------------------------------------
//
// The SENSOR is always the entity running the script. A guard's update() asks
// "do I know where the player is", which is a question about its own memory and
// not about the world — so there is no sensor argument to get wrong.

// nf.aware(id, gen) -> bool. True once a sense has been sure of the target, and
// for the linger window after it went quiet.
int l_aware(lua_State* L) {
    ecs::Entity target;
    ecs::Entity self;
    if (!check_entity(L, 1, target) || !self_entity(L, self)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    const bool aware =
        input != nullptr && input->aware_of &&
        input->aware_of(self.id, self.generation, target.id, target.generation);
    lua_pushboolean(L, aware ? 1 : 0);
    return 1;
}

// nf.sensing(id, gen) -> bool. A stimulus from the target arrived THIS frame,
// which is strictly stronger than nf.aware: a half-seen or merely remembered
// target is aware but not sensed, and that is the difference between "shoot" and
// "go and look".
int l_sensing(lua_State* L) {
    ecs::Entity target;
    ecs::Entity self;
    if (!check_entity(L, 1, target) || !self_entity(L, self)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    const bool sensing =
        input != nullptr && input->sensing_of &&
        input->sensing_of(self.id, self.generation, target.id, target.generation);
    lua_pushboolean(L, sensing ? 1 : 0);
    return 1;
}

// nf.last_known(id, gen) -> x, y, z, or nil when this entity has no memory of
// it. This is the search target: where the target was when it was last seen,
// which is what a patrol walks to.
int l_last_known(lua_State* L) {
    ecs::Entity target;
    ecs::Entity self;
    if (!check_entity(L, 1, target) || !self_entity(L, self)) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    if (input == nullptr || !input->last_known_of ||
        !input->last_known_of(self.id, self.generation, target.id, target.generation, x, y, z)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(x));
    lua_pushnumber(L, static_cast<lua_Number>(y));
    lua_pushnumber(L, static_cast<lua_Number>(z));
    return 3;
}

// nf.forget(id, gen) -> bool. Drops one target's memory: a body that has been
// searched, a decoy that was seen through, a target the game knows is gone.
int l_forget(lua_State* L) {
    ecs::Entity target;
    ecs::Entity self;
    if (!check_entity(L, 1, target) || !self_entity(L, self)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    const bool forgotten =
        input != nullptr && input->forget &&
        input->forget(self.id, self.generation, target.id, target.generation);
    lua_pushboolean(L, forgotten ? 1 : 0);
    return 1;
}

// nf.decision() -> name, score, or nil when this entity has no brain (or has not
// decided anything yet). Phase 61: the engine publishes the choice and the game
// decides what "advance" means, which is why this returns a string and not an
// effect.
int l_decision(lua_State* L) {
    ecs::Entity self;
    if (!self_entity(L, self)) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    std::string name;
    f32 score = 0.0f;
    if (input == nullptr || !input->decision ||
        !input->decision(self.id, self.generation, name, score)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, name.c_str(), name.size());
    lua_pushnumber(L, static_cast<lua_Number>(score));
    return 2;
}

// nf.state() -> the name of the state this entity is in, or nil when it has no
// state machine (Phase 62). The machine's own shape is data; this is how a game
// reads which part of it is running.
int l_state(lua_State* L) {
    ecs::Entity self;
    if (!self_entity(L, self)) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    std::string name;
    if (input == nullptr || !input->state || !input->state(self.id, self.generation, name)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, name.c_str(), name.size());
    return 1;
}

// nf.route() -> index, count, wait, finished, or nil when this entity is not
// following an authored patrol route (Phase 64). The points are scene data; the
// cursor is session state the runtime owns, which is why a script reads it here
// instead of holding its own copy that a respawn would desynchronise.
int l_route(lua_State* L) {
    ecs::Entity self;
    if (!self_entity(L, self)) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptInput* input = bound_input(L);
    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    if (input == nullptr || !input->route ||
        !input->route(self.id, self.generation, index, count, wait, finished)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(index));
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    lua_pushnumber(L, static_cast<lua_Number>(wait));
    lua_pushboolean(L, finished ? 1 : 0);
    return 4;
}

// nf.health(id, gen) -> current, max. (0, 0) for an entity with no hit points.
int l_health(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushnumber(L, 0.0);
        lua_pushnumber(L, 0.0);
        return 2;
    }
    const ScriptInput* input = bound_input(L);
    f32 current = 0.0f;
    f32 maximum = 0.0f;
    if (input != nullptr && input->health_of) {
        input->health_of(e.id, e.generation, current, maximum);
    }
    lua_pushnumber(L, static_cast<lua_Number>(current));
    lua_pushnumber(L, static_cast<lua_Number>(maximum));
    return 2;
}

// nf.damage(id, gen, amount) -> applied.
int l_damage(lua_State* L) {
    ecs::Entity e;
    if (!check_entity(L, 1, e)) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    const f32 amount = static_cast<f32>(luaL_checknumber(L, 3));
    const ScriptInput* input = bound_input(L);
    const f32 applied =
        (input != nullptr && input->damage) ? input->damage(e.id, e.generation, amount) : 0.0f;
    lua_pushnumber(L, static_cast<lua_Number>(applied));
    return 1;
}

int l_set_entity_scale(lua_State* L) {    ecs::World* w = bound_world(L);    ecs::Entity e;    if (!check_entity(L, 1, e) || !w) {
        lua_pushboolean(L, 0);
        return 1;
    }
    scene::Transform* t = w->get<scene::Transform>(e);
    if (!t) {
        lua_pushboolean(L, 0);
        return 1;
    }
    t->scale_x = static_cast<float>(luaL_checknumber(L, 3));
    t->scale_y = static_cast<float>(luaL_checknumber(L, 4));
    t->scale_z = static_cast<float>(luaL_checknumber(L, 5));
    t->dirty = true;
    lua_pushboolean(L, 1);
    return 1;
}

int l_self(lua_State* L) {
    // Set by ScriptSystem before each update() call (registry singletons:
    // script execution is single-threaded, so no state leaks between calls).
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_self_id");
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_self_gen");
    return 2;
}

} // namespace

void install_ecs_bindings(LuaVM& vm, ecs::World& world) {
    lua_State* L = vm.state();
    if (!L) return;
    lua_pushlightuserdata(L, &world);
    lua_setfield(L, LUA_REGISTRYINDEX, "nf_world");

    lua_getglobal(L, "nf");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        NF_LOG_ERROR(LogCategory::Script, "install_ecs_bindings: nf table missing");
        return;
    }
    lua_pushcfunction(L, l_entity_alive);
    lua_setfield(L, -2, "entity_alive");
    lua_pushcfunction(L, l_entity_pos);
    lua_setfield(L, -2, "entity_pos");
    lua_pushcfunction(L, l_set_entity_pos);
    lua_setfield(L, -2, "set_entity_pos");
    lua_pushcfunction(L, l_set_entity_scale);
    lua_setfield(L, -2, "set_entity_scale");
    lua_pushcfunction(L, l_set_entity_rot);
    lua_setfield(L, -2, "set_entity_rot");
    lua_pushcfunction(L, l_find_entity);
    lua_setfield(L, -2, "find_entity");
    lua_pushcfunction(L, l_entity_name);
    lua_setfield(L, -2, "entity_name");
    lua_pushcfunction(L, l_entity_count);
    lua_setfield(L, -2, "entity_count");
    lua_pushcfunction(L, l_self);
    lua_setfield(L, -2, "self");
    lua_pop(L, 1);
}

void install_input_bindings(LuaVM& vm, const ScriptInput* input) {
    lua_State* L = vm.state();
    if (!L) return;
    lua_pushlightuserdata(L, const_cast<ScriptInput*>(input));
    lua_setfield(L, LUA_REGISTRYINDEX, "nf_input");

    lua_getglobal(L, "nf");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        NF_LOG_ERROR(LogCategory::Script, "install_input_bindings: nf table missing");
        return;
    }
    lua_pushcfunction(L, l_action);
    lua_setfield(L, -2, "action");
    lua_pushcfunction(L, l_axis);
    lua_setfield(L, -2, "axis");
    lua_pushcfunction(L, l_camera_yaw);
    lua_setfield(L, -2, "camera_yaw");
    lua_pushcfunction(L, l_camera_move);
    lua_setfield(L, -2, "camera_move");
    lua_pushcfunction(L, l_shake);
    lua_setfield(L, -2, "shake");
    lua_pushcfunction(L, l_recoil);
    lua_setfield(L, -2, "recoil");
    lua_pushcfunction(L, l_weapon_ammo);
    lua_setfield(L, -2, "weapon_ammo");
    lua_pushcfunction(L, l_weapon_reserve);
    lua_setfield(L, -2, "weapon_reserve");
    lua_pushcfunction(L, l_weapon_reloading);
    lua_setfield(L, -2, "weapon_reloading");
    lua_pushcfunction(L, l_weapon_reload);
    lua_setfield(L, -2, "weapon_reload");
    lua_pushcfunction(L, l_weapon_fire);
    lua_setfield(L, -2, "weapon_fire");
    lua_pushcfunction(L, l_last_hit);
    lua_setfield(L, -2, "last_hit");
    lua_pushcfunction(L, l_impact_count);
    lua_setfield(L, -2, "impact_count");
    lua_pushcfunction(L, l_impact);
    lua_setfield(L, -2, "impact");
    lua_pushcfunction(L, l_shot_count);
    lua_setfield(L, -2, "shot_count");
    lua_pushcfunction(L, l_shot);
    lua_setfield(L, -2, "shot");
    lua_pushcfunction(L, l_health);
    lua_setfield(L, -2, "health");
    lua_pushcfunction(L, l_damage);
    lua_setfield(L, -2, "damage");
    lua_pushcfunction(L, l_apply_status);
    lua_setfield(L, -2, "apply_status");
    lua_pushcfunction(L, l_status_remaining);
    lua_setfield(L, -2, "status_remaining");
    lua_pushcfunction(L, l_clear_status);
    lua_setfield(L, -2, "clear_status");
    lua_pushcfunction(L, l_last_attacker);
    lua_setfield(L, -2, "last_attacker");
    lua_pushcfunction(L, l_killer);
    lua_setfield(L, -2, "killer");
    lua_pushcfunction(L, l_damage_taken);
    lua_setfield(L, -2, "damage_taken");
    lua_pushcfunction(L, l_suppression);
    lua_setfield(L, -2, "suppression");
    lua_pushcfunction(L, l_aware);
    lua_setfield(L, -2, "aware");
    lua_pushcfunction(L, l_sensing);
    lua_setfield(L, -2, "sensing");
    lua_pushcfunction(L, l_last_known);
    lua_setfield(L, -2, "last_known");
    lua_pushcfunction(L, l_forget);
    lua_setfield(L, -2, "forget");
    lua_pushcfunction(L, l_decision);
    lua_setfield(L, -2, "decision");
    lua_pushcfunction(L, l_state);
    lua_setfield(L, -2, "state");
    lua_pushcfunction(L, l_route);
    lua_setfield(L, -2, "route");
    lua_pop(L, 1);
}

} // namespace nf::scripting
