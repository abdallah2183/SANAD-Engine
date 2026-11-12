#include <NF/Scripting/SettingsBindings.hpp>
#include <cmath>
#include <map>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace nf::scripting {
namespace {
assets::SettingsStore* store(lua_State* state) {
    lua_getfield(state, LUA_REGISTRYINDEX, "nf_settings_store");
    auto* result = static_cast<assets::SettingsStore*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return result;
}
int save_fn(lua_State* state) {
    auto* st = store(state);
    if (!st) {
        lua_pushboolean(state, 0);
        lua_pushstring(state, "settings: no store (no project)");
        return 2;
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    assets::SettingsStore::Map values;
    lua_pushnil(state);
    while (lua_next(state, 1) != 0) {
        if (!lua_isstring(state, -2)) {
            lua_pop(state, 1);
            luaL_error(state, "settings keys must be strings");
        }
        size_t klen = 0;
        const char* key = luaL_checklstring(state, -2, &klen);
        std::string k(key, klen);
        if (!assets::SettingsStore::valid_key(k)) {
            lua_pop(state, 1);
            luaL_error(state, "settings: invalid key (use [A-Za-z0-9_.-], max 64, not 'version')");
        }
        std::string v;
        if (lua_isstring(state, -1) || lua_isnumber(state, -1)) {
            size_t n = 0;
            const char* text = luaL_checklstring(state, -1, &n);
            v.assign(text, n);
        } else if (lua_isboolean(state, -1)) {
            v = lua_toboolean(state, -1) ? "1" : "0";
        } else {
            lua_pop(state, 1);
            luaL_error(state, "settings values must be strings, numbers or booleans");
        }
        if (v.size() > 4096) {
            lua_pop(state, 1);
            luaL_error(state, "settings value for key is too long (max 4096 bytes)");
        }
        values[std::move(k)] = std::move(v);
        lua_pop(state, 1);
    }
    std::string error;
    const bool ok = st->save(values, error);
    lua_pushboolean(state, ok ? 1 : 0);
    if (ok) lua_pushnil(state);
    else lua_pushstring(state, error.c_str());
    return 2;
}
int load_fn(lua_State* state) {
    auto* st = store(state);
    lua_newtable(state);
    if (!st) {
        lua_pushboolean(state, 0);
        lua_pushstring(state, "settings: no store (no project)");
        return 3;
    }
    assets::SettingsStore::Map values;
    std::string error;
    const bool ok = st->load(values, error);
    if (ok) {
        for (const auto& [k, v] : values) {
            lua_pushstring(state, k.c_str());
            lua_pushstring(state, v.c_str());
            lua_settable(state, -3);
        }
    }
    lua_pushboolean(state, ok ? 1 : 0);
    if (ok) lua_pushnil(state);
    else lua_pushstring(state, error.c_str());
    return 3;
}
int clear_fn(lua_State* state) {
    auto* st = store(state);
    if (!st) {
        lua_pushboolean(state, 0);
        lua_pushstring(state, "settings: no store (no project)");
        return 2;
    }
    std::string error;
    const bool ok = st->clear(error);
    lua_pushboolean(state, ok ? 1 : 0);
    if (ok) lua_pushnil(state);
    else lua_pushstring(state, error.c_str());
    return 2;
}
int path_fn(lua_State* state) {
    auto* st = store(state);
    if (!st) {
        lua_pushstring(state, "");
        return 1;
    }
    lua_pushstring(state, st->settings_file().string().c_str());
    return 1;
}
} // namespace
void install_settings_bindings(LuaVM& vm, assets::SettingsStore* target) {
    auto* state = vm.state();
    if (!state) return;
    lua_pushlightuserdata(state, target);
    lua_setfield(state, LUA_REGISTRYINDEX, "nf_settings_store");
    lua_getglobal(state, "nf");
    lua_getfield(state, -1, "settings");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    struct Entry { const char* name; lua_CFunction function; };
    const Entry entries[] = {{"save", save_fn}, {"load", load_fn}, {"clear", clear_fn}, {"path", path_fn}};
    for (const auto& entry : entries) {
        lua_pushcfunction(state, entry.function);
        lua_setfield(state, -2, entry.name);
    }
    lua_setfield(state, -2, "settings");
    lua_pop(state, 1);
}
} // namespace nf::scripting
