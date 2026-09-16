// NF/Scripting/ScriptEngine.cpp — LuaVM implementation (Lua 5.4 C API).

#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Core/Logger.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
} // extern "C"

namespace nf::scripting {

namespace {

// Instruction-budget hook: counts in 1024-instruction slices, aborts past
// the limit. luaL_error longjmps out of the hook — the pcall below turns it
// into a normal error string.
void budget_hook(lua_State* L, lua_Debug*) {
    // Counters live in the registry so the hook needs no VM pointer.
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_budget_used");
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_budget_limit");
    const auto used = static_cast<u64>(lua_tointeger(L, -2)) + 1024;
    const auto limit = static_cast<u64>(lua_tointeger(L, -1));
    lua_pop(L, 2);
    lua_pushinteger(L, static_cast<lua_Integer>(used));
    lua_setfield(L, LUA_REGISTRYINDEX, "nf_budget_used");
    if (limit > 0 && used > limit) {
        // NOTE: lua_pushfstring understands %d (lua_Integer) but not %llu.
        luaL_error(L, "instruction limit exceeded");
    }
}

LogCallback* log_callback_of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_log_cb");
    auto* cb = static_cast<LogCallback*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return cb;
}

void emit_log(lua_State* L, const char* level) {
    const int n = lua_gettop(L);
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addstring(&buf, "\t");
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        luaL_addlstring(&buf, s, len);
        lua_pop(L, 1); // pop tolstring result
    }
    luaL_pushresult(&buf);
    size_t len = 0;
    const char* msg = lua_tolstring(L, -1, &len);
    std::string text(msg ? msg : "", msg ? len : 0);
    lua_pop(L, 1);
    if (LogCallback* cb = log_callback_of(L)) {
        (*cb)(level, text);
    }
}

int nf_log_info(lua_State* L) {
    emit_log(L, "info");
    return 0;
}
int nf_log_warn(lua_State* L) {
    emit_log(L, "warn");
    return 0;
}
int nf_log_error(lua_State* L) {
    emit_log(L, "error");
    return 0;
}

int nf_clamp(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const double lo = luaL_checknumber(L, 2);
    const double hi = luaL_checknumber(L, 3);
    lua_pushnumber(L, v < lo ? lo : (v > hi ? hi : v));
    return 1;
}

int nf_lerp(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double t = luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * t);
    return 1;
}

void default_log(const std::string& level, const std::string& message) {
    if (level == "warn") {
        NF_LOG_WARN(LogCategory::Script, "[lua] {}", message);
    } else if (level == "error") {
        NF_LOG_ERROR(LogCategory::Script, "[lua] {}", message);
    } else {
        NF_LOG_INFO(LogCategory::Script, "[lua] {}", message);
    }
}

} // namespace

LuaVM::LuaVM() {
    m_state = luaL_newstate();
    if (!m_state) return;
    // Restricted stdlib: no io / os / debug / package (no require, no shell,
    // no bytecode-file loading, no debugger introspection).
    luaL_requiref(m_state, "_G", luaopen_base, 1);
    lua_pop(m_state, 1);
    luaL_requiref(m_state, "table", luaopen_table, 1);
    lua_pop(m_state, 1);
    luaL_requiref(m_state, "string", luaopen_string, 1);
    lua_pop(m_state, 1);
    luaL_requiref(m_state, "math", luaopen_math, 1);
    lua_pop(m_state, 1);
    luaL_requiref(m_state, "utf8", luaopen_utf8, 1);
    lua_pop(m_state, 1);
    luaL_requiref(m_state, "coroutine", luaopen_coroutine, 1);
    lua_pop(m_state, 1);
    // File loading from inside the sandbox stays out.
    lua_pushnil(m_state);
    lua_setglobal(m_state, "dofile");
    lua_pushnil(m_state);
    lua_setglobal(m_state, "loadfile");

    // print() -> engine log (keeps scripts from writing raw stdout).
    lua_pushcfunction(m_state, nf_log_info);
    lua_setglobal(m_state, "print");

    // nf.* host library.
    lua_newtable(m_state);
    lua_pushcfunction(m_state, nf_log_info);
    lua_setfield(m_state, -2, "log_info");
    lua_pushcfunction(m_state, nf_log_warn);
    lua_setfield(m_state, -2, "log_warn");
    lua_pushcfunction(m_state, nf_log_error);
    lua_setfield(m_state, -2, "log_error");
    lua_pushcfunction(m_state, nf_clamp);
    lua_setfield(m_state, -2, "clamp");
    lua_pushcfunction(m_state, nf_lerp);
    lua_setfield(m_state, -2, "lerp");
    lua_pushstring(m_state, "1.0");
    lua_setfield(m_state, -2, "version");
    lua_setglobal(m_state, "nf");

    // Per-VM log routing (registry lightuserdata, owned by this).
    m_log_callback = std::make_unique<LogCallback>(default_log);
    lua_pushlightuserdata(m_state, m_log_callback.get());
    lua_setfield(m_state, LUA_REGISTRYINDEX, "nf_log_cb");

    // Instruction budget counters (plain integers in the registry).
    lua_pushinteger(m_state, 0);
    lua_setfield(m_state, LUA_REGISTRYINDEX, "nf_budget_used");
    lua_pushinteger(m_state, 0);
    lua_setfield(m_state, LUA_REGISTRYINDEX, "nf_budget_limit");
    lua_sethook(m_state, budget_hook, LUA_MASKCOUNT, 1024);

    NF_LOG_INFO(LogCategory::Script, "LuaVM opened ({})", LUA_RELEASE);
}

LuaVM::~LuaVM() {
    if (m_state) lua_close(m_state);
}

void LuaVM::set_log_callback(LogCallback cb) {
    if (!m_state) return;
    *m_log_callback = std::move(cb);
}

void LuaVM::set_instruction_limit(u64 limit) {
    m_instruction_limit = limit;
    apply_budget();
}

void LuaVM::apply_budget() {
    if (!m_state) return;
    lua_pushinteger(m_state, 0);
    lua_setfield(m_state, LUA_REGISTRYINDEX, "nf_budget_used");
    lua_pushinteger(m_state, static_cast<lua_Integer>(m_instruction_limit));
    lua_setfield(m_state, LUA_REGISTRYINDEX, "nf_budget_limit");
}

bool LuaVM::pcall_protected(int nargs, int nresults, std::string* out_error) {
    // Stack: [func, args...]. Prepend a traceback function below func.
    const int base = lua_gettop(m_state) - nargs;
    lua_pushcfunction(m_state, [](lua_State* L) -> int {
        luaL_traceback(L, L, lua_tostring(L, -1), 1);
        return 1;
    });
    lua_insert(m_state, base);
    const int status = lua_pcall(m_state, nargs, nresults, base);
    lua_remove(m_state, base); // drop the traceback function / result slot
    if (status != LUA_OK) {
        size_t len = 0;
        const char* msg = lua_tolstring(m_state, -1, &len);
        m_last_error.assign(msg ? msg : "unknown lua error", msg ? len : 17);
        lua_pop(m_state, 1);
        if (out_error) *out_error = m_last_error;
        return false;
    }
    return true;
}

bool LuaVM::run_string(const std::string& code, const std::string& chunk_name,
                       std::string* out_error) {
    if (!m_state) {
        if (out_error) *out_error = "VM is not open";
        return false;
    }
    apply_budget();
    if (luaL_loadbufferx(m_state, code.data(), code.size(), chunk_name.c_str(), "t") != LUA_OK) {
        size_t len = 0;
        const char* msg = lua_tolstring(m_state, -1, &len);
        m_last_error.assign(msg ? msg : "syntax error", msg ? len : 12);
        lua_pop(m_state, 1);
        if (out_error) *out_error = m_last_error;
        return false;
    }
    return pcall_protected(0, 0, out_error);
}

bool LuaVM::eval_number(const std::string& expr, double& out, std::string* out_error) {
    if (!m_state) {
        if (out_error) *out_error = "VM is not open";
        return false;
    }
    apply_budget();
    const std::string code = "return (" + expr + ")";
    if (luaL_loadbufferx(m_state, code.c_str(), code.size(), "eval", "t") != LUA_OK) {
        size_t len = 0;
        const char* msg = lua_tolstring(m_state, -1, &len);
        m_last_error.assign(msg ? msg : "eval compile failed", msg ? len : 19);
        lua_pop(m_state, 1);
        if (out_error) *out_error = m_last_error;
        return false;
    }
    if (!pcall_protected(0, 1, out_error)) return false;
    if (!lua_isnumber(m_state, -1)) {
        lua_pop(m_state, 1);
        if (out_error) *out_error = "expression did not return a number";
        return false;
    }
    out = lua_tonumber(m_state, -1);
    lua_pop(m_state, 1);
    return true;
}

bool LuaVM::call_numbers(const std::string& func, const std::vector<double>& args,
                         std::vector<double>& results, usize want_results,
                         std::string* out_error) {
    if (!m_state) {
        if (out_error) *out_error = "VM is not open";
        return false;
    }
    apply_budget();
    lua_getglobal(m_state, func.c_str());
    if (!lua_isfunction(m_state, -1)) {
        lua_pop(m_state, 1);
        if (out_error) *out_error = std::string("global '") + func + "' is not a function";
        return false;
    }
    for (double a : args) lua_pushnumber(m_state, a);
    if (!pcall_protected(static_cast<int>(args.size()), static_cast<int>(want_results),
                         out_error)) {
        return false;
    }
    results.clear();
    for (usize i = 0; i < want_results; ++i) {
        const int idx = static_cast<int>(want_results - i);
        if (!lua_isnumber(m_state, -idx)) {
            lua_pop(m_state, static_cast<int>(want_results));
            if (out_error) *out_error = "function returned a non-number";
            return false;
        }
        results.push_back(lua_tonumber(m_state, -idx));
    }
    lua_pop(m_state, static_cast<int>(want_results));
    return true;
}

void LuaVM::set_global_number(const char* name, double value) {
    if (!m_state) return;
    lua_pushnumber(m_state, value);
    lua_setglobal(m_state, name);
}

void LuaVM::set_global_string(const char* name, const std::string& value) {
    if (!m_state) return;
    lua_pushlstring(m_state, value.data(), value.size());
    lua_setglobal(m_state, name);
}

void LuaVM::set_global_bool(const char* name, bool value) {
    if (!m_state) return;
    lua_pushboolean(m_state, value ? 1 : 0);
    lua_setglobal(m_state, name);
}

bool LuaVM::get_global_number(const char* name, double& out) const {
    if (!m_state) return false;
    lua_getglobal(m_state, name);
    const bool ok = lua_isnumber(m_state, -1) != 0;
    if (ok) out = lua_tonumber(m_state, -1);
    lua_pop(m_state, 1);
    return ok;
}

} // namespace nf::scripting
