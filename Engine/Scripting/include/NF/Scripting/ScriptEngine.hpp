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
#include <string_view>
#include <vector>

struct lua_State;

namespace nf::ecs {
class World;
} // namespace nf::ecs

namespace nf::scripting {

/// Log routing for nf.log_*: defaults to the engine logger; tests override.
using LogCallback = std::function<void(const std::string& level, const std::string& message)>;

/// The small input surface scripts may read. It is a pair of callbacks rather
/// than a device/InputSystem type so NF/Scripting keeps its layering (no
/// dependency on NFPlatform or NFGameplay): the Runtime owns the real source and
/// adapts it here. Both callbacks may be empty (headless, or a paused game) and
/// then every query is false/zero rather than an error.
struct ScriptImpact {
    u32 shooter_id = 0u;
    u32 shooter_generation = 0u;
    u32 target_id = 0u;
    u32 target_generation = 0u;
    std::string target_name;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 nx = 0.0f;
    f32 ny = 0.0f;
    f32 nz = 0.0f;
    f32 distance = 0.0f;
    f32 damage = 0.0f;
    f32 applied = 0.0f;
    /// Phase 50: where the hit landed and how much armor stopped. The zone is
    /// a stable scene-format name ("body", "head", "limbs"), not an enum value,
    /// so a Lua script does not depend on C++ declaration order.
    std::string hit_zone = "body";
    f32 armor_absorbed = 0.0f;
    /// "ballistic", "explosive" or "fire" (Phase 52).
    std::string damage_type = "ballistic";
    bool blocked_by_faction = false;
    bool killed = false;
    /// True when the event came from a radial blast rather than the direct hit.
    bool splash = false;
    /// True when the event came from a damage-over-time tick (Phase 54).
    bool status = false;
};

/// One shot from the previous completed frame (Phase 53). A game uses this for
/// a muzzle flash, a tracer, a casing sound or a recoil overlay; it is the
/// event for the shot itself, not for what it hit.
struct ScriptShot {
    u32 shooter_id = 0u;
    u32 shooter_generation = 0u;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 dx = 0.0f;
    f32 dy = 0.0f;
    f32 dz = -1.0f;
    f32 damage = 0.0f;
    u32 pellet = 1u;
    u32 pellets = 1u;
    u32 seed = 0u;
    bool projectile = false;
};

struct ScriptInput {
    std::function<bool(std::string_view action)> pressed;
    std::function<f32(std::string_view action)> axis;

    /// Where the camera is looking, in degrees. Empty (headless, or a scene
    /// with no camera) reports 0, which is the direction a script that never
    /// asked about the camera already assumed.
    std::function<f32()> camera_yaw;

    /// The movement axes rotated by the camera's yaw: (x, z) in world space.
    ///
    /// Before the camera could be turned, "forward" was a world axis and a
    /// script could add `move_z * speed * dt` to z. Once the player turns the
    /// camera, forward is wherever they are looking — so the rotation has to
    /// happen somewhere, and it happens here, once, instead of in every
    /// script's trigonometry.
    std::function<void(f32&, f32&)> camera_move;

    /// Add camera shake, as trauma in [0, 1]. A script that fires an
    /// explosion or lands a hit calls `nf.shake(0.6)`; the runtime owns the
    /// clock and the camera owns the trim, so a script needs neither.
    ///
    /// Empty means the shake is dropped — a headless run has no camera to
    /// shake, and that is not an error.
    std::function<void(f32)> shake;

    /// Add recoil, in degrees: positive pitch raises the view, positive yaw
    /// turns it left. A weapon script calls `nf.recoil(1.2, 0.3)` per shot;
    /// the camera recovers on its own and the view returns to the player's
    /// aim, so the script never has to undo anything.
    std::function<void(f32, f32)> recoil;

    // --- The gun and hit points (Phase 44) ---------------------------------
    //
    // A script reads the weapon the runtime already stepped — it does not own
    // the fire-rate clock, the magazine or the spread. That distinction is the
    // whole design: a Lua script that fired on its own timer would drift from
    // the animation and from the sound, and two scripts would fight over the
    // ammo. `weapon_fire` therefore asks the weapon to fire, and the weapon's
    // cadence decides whether it may.
    //
    // Every hook is empty in a run with no runtime behind it, which is why each
    // has a documented "no weapon" answer instead of an error.

    /// Rounds in the magazine / carried in reserve. 0 when there is no weapon.
    std::function<f32()> weapon_ammo;
    std::function<f32()> weapon_reserve;
    /// Whether a reload is in progress.
    std::function<bool()> weapon_reloading;
    /// Starts a reload. False when there is nothing to load.
    std::function<bool()> weapon_reload;
    /// Fires one volley if the weapon is ready; returns the projectiles traced.
    std::function<f32()> weapon_fire;

    /// The last projectile's result: hit, x, y, z, distance. A miss reports
    /// false and leaves the numbers at zero, so a script can branch on the
    /// first value alone.
    std::function<void(bool&, f32&, f32&, f32&, f32&)> last_hit;

    /// Every impact from the PREVIOUS completed frame (Phase 49). A script
    /// reads them with `nf.impact_count()` and `nf.impact(i)`; the runtime
    /// keeps the current frame separate so an impact fired by the script
    /// itself is not lost before the next update.
    std::function<u32()> impact_count;
    std::function<bool(u32, ScriptImpact&)> impact_at;

    /// Every shot from the previous completed frame (Phase 53). Same two-slot
    /// contract as impacts, so a script that fires sees its own shot next frame.
    std::function<u32()> shot_count;
    std::function<bool(u32, ScriptShot&)> shot_at;

    /// An entity's hit points by (id, generation): current, max. A dead entity
    /// reports 0 for both rather than failing — "how much health does this
    /// have" is a question with an answer even when the answer is none.
    std::function<void(u32, u32, f32&, f32&)> health_of;

    /// Applies damage by (id, generation, amount); returns what was taken.
    std::function<f32(u32, u32, f32)> damage;

    /// Status effects (Phase 54): apply/refresh, query remaining, remove.
    std::function<bool(u32, u32, std::string_view, f32, f32, f32)> apply_status;
    std::function<f32(u32, u32, std::string_view)> status_remaining;
    std::function<bool(u32, u32, std::string_view)> clear_status;

    /// Combat memory and awareness (Phase 55).
    std::function<bool(u32, u32, u32&, u32&)> last_attacker;
    std::function<bool(u32, u32, u32&, u32&)> killer;
    std::function<void(u32, u32, f32&, f32&, u32&, f32&)> damage_taken;
    std::function<f32(u32, u32)> suppression;

    /// Perception (Phase 60). `sensor` is the entity running the script, so a
    /// guard's own `update()` asks "do I know where the player is" rather than a
    /// game keeping a parallel board. All four are no-ops when either entity has
    /// no `Perception:` line, which is what keeps a script written before this
    /// existing unchanged.
    std::function<bool(u32, u32, u32, u32)> aware_of;
    std::function<bool(u32, u32, u32, u32)> sensing_of;
    /// Last known position of (target) as remembered by (sensor).
    std::function<bool(u32, u32, u32, u32, f32&, f32&, f32&)> last_known_of;
    std::function<bool(u32, u32, u32, u32)> forget;
};

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

/// Installs nf.action(name) / nf.axis(name) against `input` (may be null:
/// every query then reads false/0). Re-installing re-targets.
void install_input_bindings(LuaVM& vm, const ScriptInput* input);

/// Per-entity game logic. `update(dt)` is looked up in the script's own
/// environment every tick; missing update() disables the script loudly.
/// `path` is the authoring-time logical VFS path (e.g.
/// content://Scripts/spin.lua) the source was loaded from; empty for inline
/// or test-authored sources. It is what the scene format persists — the
/// source itself is reloaded from the path on load, never stored inline.
struct ScriptComponent {
    std::string source;
    std::string path;
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

    /// The live input view for the next update() calls. Copied, so the caller's
    /// adapter may die as soon as this returns. Pass a default ScriptInput to
    /// disable input queries.
    void set_input_source(const ScriptInput& input);

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
