// NF/Input/InputAction.cpp

#include <NF/Input/InputAction.hpp>

#include <NF/Core/Math.hpp>

#include <cmath>

namespace nf::input {

// ---------------------------------------------------------------------------
// Binding registration
// ---------------------------------------------------------------------------

void InputAction::add_key(KeyCode key, AxisComponent component, f32 scale) {
    InputBinding b;
    b.source = BindingSource::Key;
    b.key = key;
    b.component = component;
    b.scale = scale;
    m_bindings.push_back(b);
}

void InputAction::add_mouse(MouseButton button, AxisComponent component, f32 scale) {
    InputBinding b;
    b.source = BindingSource::Mouse;
    b.mouse = button;
    b.component = component;
    b.scale = scale;
    m_bindings.push_back(b);
}

void InputAction::add_gamepad_button(GamepadButton button, AxisComponent component,
                                     f32 scale) {
    InputBinding b;
    b.source = BindingSource::GamepadButton;
    b.pad_button = button;
    b.component = component;
    b.scale = scale;
    m_bindings.push_back(b);
}

void InputAction::add_gamepad_axis(GamepadAxis axis, AxisComponent component, f32 scale) {
    InputBinding b;
    b.source = BindingSource::GamepadAxis;
    b.pad_axis = axis;
    b.component = component;
    b.scale = scale;
    m_bindings.push_back(b);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

void InputAction::accumulate_digital(const InputState& state, Vec2& out) const {
    for (const InputBinding& b : m_bindings) {
        bool held = false;
        switch (b.source) {
        case BindingSource::Key: {
            const auto idx = static_cast<usize>(b.key);
            held = idx < static_cast<usize>(KeyCode::Count) &&
                   state.keys[idx];
            break;
        }
        case BindingSource::Mouse: {
            const auto idx = static_cast<usize>(b.mouse);
            held = idx < static_cast<usize>(MouseButton::Count) &&
                   state.mouse_buttons[idx];
            break;
        }
        case BindingSource::GamepadButton: {
            // Buttons live in the key bitset of the platform state.
            const auto idx = static_cast<usize>(b.pad_button);
            held = idx < static_cast<usize>(GamepadButton::Count) &&
                   state.gamepad_buttons[idx];
            break;
        }
        case BindingSource::GamepadAxis:
            continue; // analog: handled separately
        }
        if (!held) continue;

        switch (b.component) {
        case AxisComponent::X: out.x += b.scale; break;
        case AxisComponent::Y: out.y += b.scale; break;
        case AxisComponent::None: out.x += b.scale; break;
        }
    }
}

void InputAction::accumulate_analog(const InputState& state, Vec2& out,
                                    const GamepadSettings& pad) const {
    for (const InputBinding& b : m_bindings) {
        if (b.source != BindingSource::GamepadAxis) continue;

        // Sticks are deadzoned as a pair: read the partner axis so a diagonal
        // stick movement is not clipped asymmetrically.
        f32 value = 0.0f;
        switch (b.pad_axis) {
        case GamepadAxis::LeftX:
            value = process_axis(b.pad_axis, state.axis(GamepadAxis::LeftX),
                                 state.axis(GamepadAxis::LeftY), pad);
            break;
        case GamepadAxis::LeftY:
            value = process_axis(b.pad_axis, state.axis(GamepadAxis::LeftY),
                                 state.axis(GamepadAxis::LeftX), pad);
            break;
        case GamepadAxis::RightX:
            value = process_axis(b.pad_axis, state.axis(GamepadAxis::RightX),
                                 state.axis(GamepadAxis::RightY), pad);
            break;
        case GamepadAxis::RightY:
            value = process_axis(b.pad_axis, state.axis(GamepadAxis::RightY),
                                 state.axis(GamepadAxis::RightX), pad);
            break;
        case GamepadAxis::LeftTrigger:
        case GamepadAxis::RightTrigger:
            value = process_axis(b.pad_axis, state.axis(b.pad_axis), 0.0f, pad);
            break;
        case GamepadAxis::Count:
            continue;
        }

        switch (b.component) {
        case AxisComponent::X: out.x += value * b.scale; break;
        case AxisComponent::Y: out.y += value * b.scale; break;
        case AxisComponent::None: out.x += value * b.scale; break;
        }
    }
}

f32 InputAction::evaluate_bool(const InputState& state,
                               const GamepadSettings& pad) const {
    Vec2 out{0.0f, 0.0f};
    accumulate_digital(state, out);
    accumulate_analog(state, out, pad);
    // Digital action: any contribution counts as fully pressed.
    return (out.x > 0.0f || out.y > 0.0f) ? 1.0f : 0.0f;
}

f32 InputAction::evaluate_axis(const InputState& state,
                               const GamepadSettings& pad) const {
    Vec2 out{0.0f, 0.0f};
    accumulate_digital(state, out);
    accumulate_analog(state, out, pad);
    return clamp(out.x, -1.0f, 1.0f);
}

Vec2 InputAction::evaluate_vector(const InputState& state,
                                  const GamepadSettings& pad) const {
    Vec2 out{0.0f, 0.0f};
    accumulate_digital(state, out);
    accumulate_analog(state, out, pad);
    out.x = clamp(out.x, -1.0f, 1.0f);
    out.y = clamp(out.y, -1.0f, 1.0f);
    return out;
}

f32 InputAction::process_axis(GamepadAxis axis, f32 raw_x, f32 raw_y,
                              const GamepadSettings& pad) {
    switch (axis) {
    case GamepadAxis::LeftTrigger:
    case GamepadAxis::RightTrigger: {
        // Linear deadzone, then rescale so a fully pulled trigger reaches 1.
        const f32 deadzone = clamp(pad.trigger_deadzone, 0.0f, 0.95f);
        if (raw_x <= deadzone) return 0.0f;
        return clamp((raw_x - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
    }
    case GamepadAxis::LeftX:
    case GamepadAxis::LeftY:
    case GamepadAxis::RightX:
    case GamepadAxis::RightY: {
        // Radial deadzone on the stick pair, then rescale the surviving vector
        // to full range. This keeps the direction intact and avoids the
        // "snap to axis" artifacts of independent per-axis deadzones.
        const f32 deadzone = clamp(pad.stick_deadzone, 0.0f, 0.95f);
        const f32 mag = std::sqrt(raw_x * raw_x + raw_y * raw_y);
        if (mag <= deadzone) return 0.0f;
        // mag > deadzone holds here, so the scale is always well defined.
        const f32 scale = (mag - deadzone) / mag;
        return clamp(raw_x * scale, -1.0f, 1.0f);
    }
    case GamepadAxis::Count:
        return 0.0f;
    }
    return 0.0f;
}

} // namespace nf::input
