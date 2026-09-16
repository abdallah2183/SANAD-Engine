#pragma once

// NF/Scripting/ScriptEngine.hpp — Lua gameplay scripting (Phase 15).
//
// Three layers, bottom to top:
//   1. LuaVM: RAII Lua 5.4 state with a restricted standard library (base,
//      table, string, math, utf8, coroutine — NO io/os/debug/package), an
//      `nf.*` host library (logging, math), per-call instruction budgets,
//      and traceback-decorated errors. No engine types leak through.
//   2. Entity bindings: `nf.entity_pos / set_entity_pos / entity_alive /
//      self` against an ecs::World (install_ecs_bindings).
//   3. ScriptSystem: ScriptComponent{source} + per-source sandboxed
//      environments; calls update(dt) for every enabled script entity.
//
// Sandboxing notes (documented, not accidental):
//   - Each distinct source runs in its own _ENV table (globals don't leak
//     between scripts); _ENV falls back to the shared _G for nf.* and libs.
//   - dofile/loadfile are removed; load() from a string still works.
//   - A script that errors 5 times in a row is disabled until its source
//     changes (log spam is bounded); a source edit always recompiles.
//   - Instruction budgets abort runaway scripts (infinite loops) loudly.

#include <NF/Core/Types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace nf::ecs {
class World;
} // namespace nf::ecs

namespace nf::scripting {

/// Log routing for nf.log_*: defaults to the engine logger; tests override.
using LogCallback = std::function<void(const std::string& level, const std::string& message)>;

/// RAII Lua 5.4 VM. Not copyable. Single-threaded use only (like lua_State).
class LuaVM {
public:
    LuaVM();
    ~LuaVM();

    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;

    bool valid() const { return m_state != nullptr; }

    /// Runs a chunk for its side effects (defines functions/globals in _G).
    bool run_string(const std::string& code, const std::string& chunk_name = "chunk",
                    std::string* out_error = nullptr);

    /// Evaluates a numeric expression ("return (" + expr + ")").
    bool eval_number(const std::string& expr, double& out, std::string* out_error = nullptr);

    /// Calls a _G function with numeric args, collecting numeric results.
    bool call_numbers(const std::string& func, const std::vector<double>& args,
                      std::vector<double>& results, usize want_results,
                      std::string* out_error = nullptr);

    void set_global_number(const char* name, double value);
    void set_global_string(const char* name, const std::string& value);
    void set_global_bool(const char* name, bool value);
    bool get_global_number(const char* name, double& out) const;

    /// 0 = unlimited. Reset automatically at every run/call boundary.
    void set_instruction_limit(u64 limit);
    u64 instruction_limit() const { return m_instruction_limit; }

    /// Resets the per-call instruction counter and publishes the limit to
    /// the budget hook. Called automatically by run/call; hosts driving
    /// raw pcall sequences (ScriptSystem) call it themselves.
    void apply_budget();

    void set_log_callback(LogCallback cb);

    /// Escape hatch for the binding layer (same module). Not for games.
    lua_State* state() { return m_state; }

private:
    bool pcall_protected(int nargs, int nresults, std::string* out_error);

    lua_State* m_state = nullptr;
    u64 m_instruction_limit = 0;
    u64 m_instructions_used = 0;
    std::unique_ptr<LogCallback> m_log_callback;
    std::string m_last_error;
};

/// Installs nf.entity_pos / nf.set_entity_pos / nf.entity_alive / nf.self
/// bound to `world` (must outlive the VM). Re-installing re-targets.
void install_ecs_bindings(LuaVM& vm, ecs::World& world);

/// Per-entity game logic. `update(dt)` is looked up in the script's own
/// environment every tick; missing update() disables the script loudly.
struct ScriptComponent {
    std::string source;
    bool enabled = true;
};

/// Ticks every enabled ScriptComponent. Owns one LuaVM (with ECS bindings
/// for `world`) plus the per-source environment cache.
class ScriptSystem {
public:
    ScriptSystem();
    ~ScriptSystem();

    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    void update(ecs::World& world, float dt);

    LuaVM& vm() { return *m_vm; }
    void set_instruction_limit(u64 limit);

    /// Forgets cached environments (sources recompile lazily). Broken-source
    /// records are cleared too.
    void clear_cache();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    LuaVM* m_vm = nullptr; // owned by m_impl; cached for vm()
};

} // namespace nf::scripting
