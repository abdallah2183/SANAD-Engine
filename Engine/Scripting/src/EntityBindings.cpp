// NF/Scripting/EntityBindings.cpp — nf.entity_* against an ecs::World.

#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

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
    lua_pushcfunction(L, l_self);
    lua_setfield(L, -2, "self");
    lua_pop(L, 1);
}

} // namespace nf::scripting
