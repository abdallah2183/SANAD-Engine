// NF/Scripting/ScriptSystem.cpp — per-entity sandboxed script ticking.

#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/ECS/ECS.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
} // extern "C"

#include <unordered_map>

namespace nf::scripting {

namespace {

// Consecutive runtime failures before a script is parked (source edits
// always clear the record). Bounds log spam from a permanently broken tick.
constexpr int kMaxConsecutiveErrors = 5;
// Environment cache cap: beyond this the whole cache drops and sources
// recompile lazily (keeps a pathological scene from pinning memory).
constexpr usize kMaxCachedEnvs = 64;

usize hash_source(const std::string& s) {
    return std::hash<std::string>{}(s);
}

} // namespace

struct ScriptSystem::Impl {
    LuaVM vm;
    u64 instruction_limit = 0;
    // source hash -> registry ref of the _ENV table (LUA_NOREF = broken).
    std::unordered_map<usize, int> envs;
    std::unordered_map<usize, int> consecutive_errors;

    ~Impl() {
        for (const auto& [key, ref] : envs) {
            if (ref != LUA_NOREF) luaL_unref(vm.state(), LUA_REGISTRYINDEX, ref);
        }
    }

    void clear() {
        for (const auto& [key, ref] : envs) {
            if (ref != LUA_NOREF) luaL_unref(vm.state(), LUA_REGISTRYINDEX, ref);
        }
        envs.clear();
        consecutive_errors.clear();
    }

    // Compiles source into a fresh _ENV; returns the env registry ref, or
    // LUA_NOREF on error (already logged). Stack discipline: every path
    // below leaves the stack exactly as it found it.
    int compile(const std::string& source, usize key) {
        lua_State* L = vm.state();
        if (luaL_loadbufferx(L, source.data(), source.size(), "script", "t") != LUA_OK) {
            size_t len = 0;
            const char* msg = lua_tolstring(L, -1, &len);
            NF_LOG_ERROR(LogCategory::Script, "script compile failed: {}",
                         msg ? std::string(msg, len) : "?");
            lua_pop(L, 1);
            envs[key] = LUA_NOREF;
            return LUA_NOREF;
        }
        // Stack: [chunk]. Build the sandbox env, stash it in the registry,
        // and point the chunk's _ENV at it — before running anything.
        lua_newtable(L); // [chunk, env]
        lua_pushvalue(L, -1); // [chunk, env, env] (one copy is stashed)
        lua_newtable(L); // [chunk, env, env, mt]
        lua_pushglobaltable(L); // [chunk, env, env, mt, _G]
        lua_setfield(L, -2, "__index"); // mt.__index = _G (pops _G)
        lua_setmetatable(L, -2); // env copy gets the mt (pops mt)
        lua_setupvalue(L, -3, 1); // chunk._ENV = env (pops one env)
        // Stack: [chunk, env]. Stash env, leaving [chunk].
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        // Run the chunk to define update() inside env: [handler, chunk].
        const int base = lua_gettop(L);
        lua_pushcfunction(L, [](lua_State* SL) -> int {
            luaL_traceback(SL, SL, lua_tostring(SL, -1), 1);
            return 1;
        });
        lua_insert(L, base);
        vm.apply_budget();
        const int status = lua_pcall(L, 0, 0, base);
        if (status != LUA_OK) {
            // Stack: [handler, errmsg].
            size_t len = 0;
            const char* msg = lua_tolstring(L, -1, &len);
            NF_LOG_ERROR(LogCategory::Script, "script init failed: {}",
                         msg ? std::string(msg, len) : "?");
            lua_pop(L, 2); // errmsg + handler
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            envs[key] = LUA_NOREF;
            return LUA_NOREF;
        }
        lua_remove(L, base); // drop handler; stack is balanced again
        // Require update(dt) to be a function in env.
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref); // [env]
        lua_getfield(L, -1, "update"); // [env, update]
        const bool has_update = lua_isfunction(L, -1) != 0;
        lua_pop(L, 2);
        if (!has_update) {
            NF_LOG_ERROR(LogCategory::Script, "script must define update(dt)");
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            envs[key] = LUA_NOREF;
            return LUA_NOREF;
        }
        envs[key] = ref;
        consecutive_errors[key] = 0;
        return ref;
    }
};

ScriptSystem::ScriptSystem() : m_impl(std::make_unique<Impl>()) {
    m_vm = &m_impl->vm;
}

ScriptSystem::~ScriptSystem() = default;

void ScriptSystem::set_instruction_limit(u64 limit) {
    m_impl->instruction_limit = limit;
    m_impl->vm.set_instruction_limit(limit);
}

void ScriptSystem::clear_cache() {
    m_impl->clear();
}

void ScriptSystem::update(ecs::World& world, float dt) {
    Impl& impl = *m_impl;
    install_ecs_bindings(impl.vm, world);
    impl.vm.set_instruction_limit(impl.instruction_limit);
    lua_State* L = impl.vm.state();
    if (!L) return;

    for (ecs::Entity e : world.query<ScriptComponent>()) {
        const ScriptComponent* comp = world.get<ScriptComponent>(e);
        if (!comp || !comp->enabled || comp->source.empty()) continue;
        if (impl.envs.size() > kMaxCachedEnvs) impl.clear();

        const usize key = hash_source(comp->source);
        auto eit = impl.envs.find(key);
        int env_ref = (eit != impl.envs.end()) ? eit->second : LUA_NOREF;
        if (eit == impl.envs.end()) {
            env_ref = impl.compile(comp->source, key);
            if (env_ref == LUA_NOREF) continue; // logged at compile time
        } else if (env_ref == LUA_NOREF) {
            continue; // broken source: stays silent until the source changes
        }

        // Consecutive-error parking.
        int& errors = impl.consecutive_errors[key]; // default 0
        if (errors >= kMaxConsecutiveErrors) continue;

        // Publish self for nf.self() and as env fields for direct reads.
        lua_pushinteger(L, static_cast<lua_Integer>(e.id));
        lua_setfield(L, LUA_REGISTRYINDEX, "nf_self_id");
        lua_pushinteger(L, static_cast<lua_Integer>(e.generation));
        lua_setfield(L, LUA_REGISTRYINDEX, "nf_self_gen");

        lua_rawgeti(L, LUA_REGISTRYINDEX, env_ref); // [env]
        lua_pushinteger(L, static_cast<lua_Integer>(e.id));
        lua_setfield(L, -2, "self_id");
        lua_pushinteger(L, static_cast<lua_Integer>(e.generation));
        lua_setfield(L, -2, "self_gen");
        lua_getfield(L, -1, "update"); // [env, update]
        if (!lua_isfunction(L, -1)) {
            NF_LOG_ERROR(LogCategory::Script, "script lost update(dt); parking");
            lua_pop(L, 2); // update value + env
            errors = kMaxConsecutiveErrors;
            continue;
        }
        lua_pushnumber(L, static_cast<lua_Number>(dt)); // [env, update, dt]
        lua_remove(L, -3); // [update, dt]
        // Protected call with traceback: [handler, update, dt].
        // base = stack index of update (gettop is 2: [update, dt]).
        const int base = lua_gettop(L) - 1;
        lua_pushcfunction(L, [](lua_State* SL) -> int {
            luaL_traceback(SL, SL, lua_tostring(SL, -1), 1);
            return 1;
        });
        lua_insert(L, base);
        impl.vm.apply_budget();
        const int status = lua_pcall(L, 1, 0, base);
        if (status != LUA_OK) {
            // Stack: [handler, errmsg].
            size_t len = 0;
            const char* msg = lua_tolstring(L, -1, &len);
            ++errors;
            NF_LOG_WARN(LogCategory::Script, "script runtime error ({}/{}): {}",
                        errors, kMaxConsecutiveErrors,
                        msg ? std::string(msg, len) : "?");
            lua_pop(L, 2); // errmsg + handler
            continue;
        }
        lua_remove(L, base); // drop handler; stack balanced
        errors = 0;
    }
}

} // namespace nf::scripting
