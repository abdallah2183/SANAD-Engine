#pragma once

// NF/Input/InputMapper.hpp — Gameplay-facing action layer.
// Design doc §50 (action-based input), §51 (cross-platform unification),
// §325 (`GetVector("Move")`) and §114 (determinism).
//
// Sits above the raw Platform InputSystem. Frame lifecycle:
//
//   1. update(state)   — snapshot the raw device state, resolve every action.
//   2. queries        — is_pressed / just_pressed / get_axis / get_vector.
//   3. end_frame()    — current values become the previous frame's reference.
//
// Contexts are evaluated in priority order; an active blocking context
// suppresses all actions in lower-priority contexts (they read as unpressed /
// zero). Edge detection runs on the post-suppression value, so a press that was
// blocked never surfaces as "just pressed" later — the edge the gameplay code
// sees always matches the value it reads.
//
// Determinism (§114): bindings are visited in insertion order and no RNG is
// used, so identical device states produce identical action values on every
// platform and every run. That is what makes recorded input replayable.
//
// Bindings round-trip through a text format (§138 configuration, §50 rebinding):
// the editor writes it, `load_string` reads it back.

#include <NF/Core/Containers.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Input/InputAction.hpp>
#include <NF/Input/InputContext.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace nf::input {

/// Resolved action value for one frame. Bool actions use `x` (0 or 1), Axis1D
/// uses `x`, Axis2D uses both components.
struct ActionValue {
    f32 x = 0.0f;
    f32 y = 0.0f;

    static constexpr f32 PressedThreshold = 0.5f;

    bool pressed() const { return x > PressedThreshold; }
};

/// Which transition a callback reports. Edges use the same post-suppression
/// values (and the same threshold) as just_pressed / just_released, so a
/// callback and the equivalent query can never disagree:
///   Pressed  — the action crossed the threshold upward this update
///   Held     — it stayed above it (every held update after the Pressed one)
///   Released — it fell back below it
enum class ActionPhase : u8 {
    Pressed = 0,
    Held,
    Released,
};

/// Callback payload: the action's resolved value this frame (Vec2 actions use
/// both components; Bool/Axis1D use `x`).
using ActionCallback = std::function<void(const ActionValue&)>;

class InputMapper {
public:
    InputMapper() = default;
    ~InputMapper() = default;

    InputMapper(const InputMapper&) = delete;
    InputMapper& operator=(const InputMapper&) = delete;
    InputMapper(InputMapper&&) = default;
    InputMapper& operator=(InputMapper&&) = default;

    void set_gamepad_settings(GamepadSettings settings) { m_pad = settings; }
    const GamepadSettings& gamepad_settings() const { return m_pad; }

    // --- contexts ---------------------------------------------------------

    /// Creates a context; higher `priority` is evaluated first. `blocking`
    /// contexts suppress every lower-priority context while active.
    /// Returns a stable reference (valid for the mapper's lifetime).
    InputContext& create_context(std::string name, i32 priority,
                                 bool blocking = false);
    InputContext* find_context(std::string_view name);
    const InputContext* find_context(std::string_view name) const;
    usize context_count() const { return m_contexts.size(); }

    void set_context_active(std::string_view name, bool active);
    void set_all_active(bool active);

    // --- frame lifecycle --------------------------------------------------

    /// Resolve every action against a raw device state. Must be called once per
    /// frame, before queries. Does not allocate after the first call (the
    /// evaluation cache is rebuilt only when contexts change).
    void update(const InputState& state);

    /// Advance the frame: current values become the previous reference used by
    /// edge detection on the next update.
    void end_frame();

    // --- queries ----------------------------------------------------------

    /// True while any bound source of a Bool action is held.
    bool is_pressed(std::string_view action) const;
    /// True only on the frame the action crossed the pressed threshold.
    bool just_pressed(std::string_view action) const;
    /// True only on the frame the action fell back below it.
    bool just_released(std::string_view action) const;
    /// Scalar value of an Axis1D action in [-1, 1] (Bool → [0,1]).
    f32 get_axis(std::string_view action) const;
    /// Direction of an Axis2D action, each component in [-1, 1].
    Vec2 get_vector(std::string_view action) const;

    // --- callbacks --------------------------------------------------------

    /// Registers a callback for one action phase. Multiple phases (or several
    /// callbacks on one phase — the newest replaces the older) are allowed;
    /// passing a null function clears that phase. Callbacks fire during
    /// update(), in evaluation order (context priority, then insertion), after
    /// every action value is resolved — a callback may safely query any action.
    /// Edge semantics match just_pressed / just_released exactly, including
    /// suppression: a held action blocked by a menu reads as Released.
    void set_action_callback(std::string_view action, ActionPhase phase,
                             ActionCallback callback);

    // --- serialization (rebinding persistence) ----------------------------

    /// Human-readable binding dump (§73): contexts, actions, bindings.
    std::string to_string() const;
    /// Rebuilds every context/action/binding from a dump. Unknown tokens are
    /// skipped with a warning rather than aborting the whole file (§148).
    bool load_string(std::string_view text);

    /// Bindings version tag, embedded in dumps and checked on load.
    static constexpr u32 BindingsVersion = 1;

private:
    /// Per-action callback slots, keyed by action name. `InputContext` refuses
    /// a duplicate name *within* one context but there is no mapper-wide
    /// uniqueness rule, so a name reused across two contexts would fire that
    /// action's callbacks once per context per update (and the value map would
    /// keep only the last context's value). Keep action names globally unique.
    struct ActionCallbacks {
        ActionCallback on_pressed;
        ActionCallback on_held;
        ActionCallback on_released;
    };

    /// Fires callbacks for the freshly resolved values, in evaluation order.
    /// No-op (and allocation-free) while no callback is registered.
    void fire_callbacks();

    void mark_dirty();
    /// Rebuild the flat evaluation order (contexts by priority, then their
    /// actions in insertion order) and pre-size the value maps so update()
    /// never allocates.
    void refresh_cache();

    /// Resolves suppression for every context: a context is suppressed if any
    /// higher-priority active blocking context is active.
    void compute_suppression();

    const ActionValue* current_value(std::string_view action) const;
    const ActionValue* previous_value(std::string_view action) const;

    // Contexts own actions; stable addresses are handed to callers, so the
    // container must not relocate them.
    DynamicArray<std::unique_ptr<InputContext>> m_contexts;

    // Evaluation structure, rebuilt only when contexts/actions change.
    DynamicArray<usize> m_context_order;   // context indices, priority order
    DynamicArray<bool> m_context_suppressed; // parallel to m_contexts

    // Double-buffered values; end_frame() swaps the pointers so no copy and no
    // allocation happens per frame.
    HashMap<std::string, ActionValue> m_values_a;
    HashMap<std::string, ActionValue> m_values_b;
    HashMap<std::string, ActionValue>* m_current = &m_values_a;
    HashMap<std::string, ActionValue>* m_previous = &m_values_b;

    GamepadSettings m_pad;
    HashMap<std::string, ActionCallbacks> m_callbacks;
    bool m_dirty = true;
};

} // namespace nf::input
