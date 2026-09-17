#pragma once

// NF/Input/InputAction.hpp — Gameplay-facing input action (design doc §50–§51).
//
// An action is a named gameplay intent ("Move", "Jump", "Fire") that players
// express through arbitrary physical sources. Actions are typed by the value
// they produce:
//
//   Bool   — pressed / just-pressed / just-released   (Jump, Fire)
//   Axis1D — a scalar in [-1, 1]                       (Throttle, Strafe)
//   Axis2D — a direction vector in [-1, 1]^2           (Move, Look)
//
// Keyboard keys and mouse/gamepad buttons contribute a fixed +/- amount to one
// component of the action value while held; gamepad axes contribute their live
// (deadzoned, normalized) reading. Composing both lets a single action serve
// keyboard and gamepad identically — gameplay code never inspects the device,
// exactly what §50 asks for ("actions, not keys").

#include <NF/Core/Containers.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Platform/InputSystem.hpp>

#include <string>
#include <string_view>

namespace nf::input {

enum class ActionType : u8 {
    Bool,
    Axis1D,
    Axis2D,
};

/// Which component of an Axis2D action a binding drives. `None` marks bindings
/// that feed a Bool or Axis1D action.
enum class AxisComponent : u8 {
    None = 0,
    X = 1,
    Y = 2,
};

/// Physical sources a binding can read from. Only the matching field of
/// `InputBinding` is consulted during evaluation.
enum class BindingSource : u8 {
    Key,
    Mouse,
    GamepadButton,
    GamepadAxis,
};

/// Gamepad analog processing. The mapper owns one set and applies it to every
/// action so a stick behaves identically across the whole game.
struct GamepadSettings {
    /// Radial deadzone for the four stick axes: the (x,y) pair is discarded
    /// below this magnitude, and rescaled to full range above it.
    f32 stick_deadzone = 0.15f;
    /// Linear deadzone for the two triggers.
    f32 trigger_deadzone = 0.05f;
};

/// A single physical source contributing to an action's value.
struct InputBinding {
    BindingSource source = BindingSource::Key;
    KeyCode key = KeyCode::Unknown;                 // source == Key
    MouseButton mouse = MouseButton::Left;          // source == Mouse
    GamepadButton pad_button = GamepadButton::A;    // source == GamepadButton
    GamepadAxis pad_axis = GamepadAxis::LeftX;      // source == GamepadAxis
    AxisComponent component = AxisComponent::None;
    f32 scale = 1.0f; // direction/magnitude of the contribution
};

class InputAction {
public:
    InputAction() = default;
    InputAction(std::string name, ActionType type)
        : m_name(std::move(name)), m_type(type) {}

    const std::string& name() const { return m_name; }
    ActionType type() const { return m_type; }

    // --- binding registration (setup time only) --------------------------

    /// Digital binding: contributes `scale` to `component` while the key is held.
    /// For Bool actions `component`/`scale` are ignored (any held key = pressed).
    void add_key(KeyCode key, AxisComponent component = AxisComponent::None,
                 f32 scale = 1.0f);
    void add_mouse(MouseButton button, AxisComponent component = AxisComponent::None,
                   f32 scale = 1.0f);
    void add_gamepad_button(GamepadButton button,
                            AxisComponent component = AxisComponent::None,
                            f32 scale = 1.0f);
    /// Analog binding: contributes the live axis reading (deadzoned) scaled by
    /// `scale` to `component`.
    void add_gamepad_axis(GamepadAxis axis, AxisComponent component,
                          f32 scale = 1.0f);

    void clear_bindings() { m_bindings.clear(); }
    const DynamicArray<InputBinding>& bindings() const { return m_bindings; }

    // --- evaluation (frame time) -----------------------------------------

    /// Reads a raw device state and produces the action's value:
    ///   Bool  → [0,1]    (1.0 if any bound source is held)
    ///   Axis1D → [-1,1]
    ///   Axis2D → each component in [-1,1]
    f32 evaluate_bool(const InputState& state,
                      const GamepadSettings& pad) const;
    f32 evaluate_axis(const InputState& state,
                      const GamepadSettings& pad) const;
    Vec2 evaluate_vector(const InputState& state,
                         const GamepadSettings& pad) const;

    /// Deadzone + rescale for one raw axis reading. Sticks use a radial deadzone
    /// (the paired axis is needed), triggers a linear one. Exposed so the editor
    /// can preview exactly what gameplay will see.
    static f32 process_axis(GamepadAxis axis, f32 raw_x, f32 raw_y,
                            const GamepadSettings& pad);

private:
    /// Sum of every digital binding's contribution into a 2D accumulator.
    void accumulate_digital(const InputState& state, Vec2& out) const;
    /// Sum of every analog (gamepad axis) binding into a 2D accumulator.
    void accumulate_analog(const InputState& state, Vec2& out,
                           const GamepadSettings& pad) const;

    std::string m_name;
    ActionType m_type = ActionType::Bool;
    DynamicArray<InputBinding> m_bindings;
};

} // namespace nf::input
