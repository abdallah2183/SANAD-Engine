#include <NF/Scripting/SceneFlow.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <cmath>
#include <iterator>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace nf::scripting {
bool SceneFlow::valid_scene_path(std::string_view path) {
    constexpr std::string_view prefix = "content://Scenes/";
    if (path.size() > 512 || !path.starts_with(prefix) || !path.ends_with(".nfscene")) return false;
    path.remove_prefix(prefix.size());
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part.empty() || part == "." || part == "..") return false;
        for (const unsigned char c : part) {
            if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '?' || c == '#' || c == '*') return false;
        }
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return true;
}
bool SceneFlow::request_scene(std::string_view path, std::string& error) {
    error.clear();
    if (pending() || m_quit) { error = "another scene or quit request is pending"; return false; }
    if (!valid_scene_path(path)) { error = "expected content://Scenes/<name>.nfscene without traversal"; return false; }
    m_pending = path; m_target = path; m_status = "queued"; m_error.clear();
    return true;
}
bool SceneFlow::request_restart(std::string& error) { return request_scene(m_current, error); }
bool SceneFlow::request_quit(std::string& error) {
    error.clear();
    if (pending() || m_quit) { error = "another scene or quit request is pending"; return false; }
    m_quit = true; m_status = "quitting"; return true;
}
void SceneFlow::scene_adopted(const std::string& path) {
    m_current = path; m_pending.clear(); m_target = path; m_error.clear(); m_status = "idle"; m_quit = false;
}
void SceneFlow::complete(bool success, std::string error) {
    m_pending.clear(); m_status = success ? "ready" : "failed"; m_error = std::move(error);
}
void SceneFlow::reset_session() {
    m_values.clear(); m_pending.clear(); m_target.clear(); m_error.clear(); m_status = "idle"; m_quit = false;
}
namespace {
bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > 64) return false;
    for (const unsigned char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}
size_t value_bytes(const SceneFlow::Value& value) {
    if (const auto* s = std::get_if<std::string>(&value)) return s->size();
    return sizeof(double);
}
}
bool SceneFlow::set(std::string_view key, Value value, std::string& error) {
    error.clear();
    if (!valid_key(key)) { error = "session key must use 1..64 ASCII letters, digits, _, - or ."; return false; }
    if (const auto* d = std::get_if<double>(&value); d && !std::isfinite(*d)) {
        error = "session numbers must be finite"; return false;
    }
    if (value_bytes(value) > 4096) { error = "session string exceeds 4096 bytes"; return false; }
    if (!m_values.contains(key) && m_values.size() >= 256) { error = "session exceeds 256 keys"; return false; }
    size_t bytes = key.size() + value_bytes(value);
    for (const auto& [k, v] : m_values) if (k != key) bytes += k.size() + value_bytes(v);
    if (bytes > 65536) { error = "session exceeds 64 KiB"; return false; }
    m_values.insert_or_assign(std::string(key), std::move(value)); return true;
}
bool SceneFlow::erase(std::string_view key) {
    const auto it = m_values.find(key);
    if (it == m_values.end()) return false;
    m_values.erase(it); return true;
}
const SceneFlow::Value* SceneFlow::get(std::string_view key) const {
    const auto it = m_values.find(key); return it == m_values.end() ? nullptr : &it->second;
}
namespace {
SceneFlow* host(lua_State* L) { return static_cast<SceneFlow*>(lua_touserdata(L, lua_upvalueindex(1))); }
std::string_view string_arg(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TSTRING);
    size_t count = 0; const char* text = lua_tolstring(L, index, &count); return {text, count};
}
int result(lua_State* L, bool ok, const std::string& error) {
    lua_pushboolean(L, ok);
    if (error.empty()) lua_pushnil(L); else lua_pushlstring(L, error.data(), error.size());
    return 2;
}
int load_fn(lua_State* L) {
    const auto path = string_arg(L, 1);
    std::string error = "scene host unavailable";
    auto* h = host(L); const bool ok = h && h->request_scene(path, error); return result(L, ok, error);
}
int restart_fn(lua_State* L) {
    std::string error = "scene host unavailable";
    auto* h = host(L); const bool ok = h && h->request_restart(error); return result(L, ok, error);
}
int quit_fn(lua_State* L) {
    std::string error = "scene host unavailable";
    auto* h = host(L); const bool ok = h && h->request_quit(error); return result(L, ok, error);
}
int current_fn(lua_State* L) {
    const auto* h = host(L); if (h) lua_pushlstring(L, h->current().data(), h->current().size()); else lua_pushnil(L);
    return 1;
}
int status_fn(lua_State* L) {
    const auto* h = host(L);
    lua_pushstring(L, h ? h->status().c_str() : "unavailable");
    lua_pushstring(L, h ? h->target().c_str() : "");
    lua_pushstring(L, h ? h->error().c_str() : "scene host unavailable");
    return 3;
}
int set_fn(lua_State* L) {
    const auto key = string_arg(L, 1);
    const auto type = lua_type(L, 2);
    if (type != LUA_TNIL && type != LUA_TBOOLEAN && type != LUA_TNUMBER && type != LUA_TSTRING)
        return luaL_error(L, "session values must be boolean, number, string or nil");
    auto* h = host(L);
    if (!h) return result(L, false, "session host unavailable");
    if (!valid_key(key)) return result(L, false, "invalid session key");
    if (type == LUA_TSTRING) {
        size_t bytes = 0; (void)lua_tolstring(L, 2, &bytes);
        if (bytes > 4096) return result(L, false, "session string exceeds 4096 bytes");
    }
    if (type == LUA_TNIL) { h->erase(key); return result(L, true, ""); }
    SceneFlow::Value value;
    switch (type) {
        case LUA_TBOOLEAN: value = lua_toboolean(L, 2) != 0; break;
        case LUA_TNUMBER: value = static_cast<double>(lua_tonumber(L, 2)); break;
        default: { size_t n = 0; const char* p = lua_tolstring(L, 2, &n); value = std::string(p, n); break; }
    }
    std::string error; const bool ok = h->set(key, std::move(value), error); return result(L, ok, error);
}
int get_fn(lua_State* L) {
    const auto key = string_arg(L, 1); const auto* h = host(L); const auto* value = h ? h->get(key) : nullptr;
    if (!value) { if (lua_gettop(L) >= 2) lua_pushvalue(L, 2); else lua_pushnil(L); }
    else if (const auto* b = std::get_if<bool>(value)) lua_pushboolean(L, *b);
    else if (const auto* d = std::get_if<double>(value)) lua_pushnumber(L, *d);
    else { const auto& s = std::get<std::string>(*value); lua_pushlstring(L, s.data(), s.size()); }
    return 1;
}
int clear_fn(lua_State* L) { if (auto* h = host(L)) h->clear_values(); return 0; }
struct Entry { const char* name; lua_CFunction fn; };
void table(lua_State* L, const char* name, const Entry* entries, size_t count, SceneFlow* flow) {
    lua_getglobal(L, "nf"); lua_newtable(L);
    for (size_t i = 0; i < count; ++i) {
        lua_pushlightuserdata(L, flow); lua_pushcclosure(L, entries[i].fn, 1); lua_setfield(L, -2, entries[i].name);
    }
    lua_setfield(L, -2, name); lua_pop(L, 1);
}
}
void install_game_flow_bindings(LuaVM& vm, SceneFlow* flow) {
    auto* L = vm.state(); if (!L) return;
    const Entry scene[] = {{"load", load_fn}, {"restart", restart_fn}, {"quit", quit_fn}, {"current", current_fn}, {"status", status_fn}};
    const Entry session[] = {{"set", set_fn}, {"get", get_fn}, {"clear", clear_fn}};
    table(L, "scene", scene, std::size(scene), flow); table(L, "session", session, std::size(session), flow);
}
} // namespace nf::scripting
