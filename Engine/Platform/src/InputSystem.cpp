// NF/Platform/InputSystem.cpp

#include <NF/Platform/InputSystem.hpp>
#include <NF/Core/Logger.hpp>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace nf {

// --- InputAction ---

bool InputAction::is_pressed(const InputState& state) const {
    for (KeyCode k : keys) {
        if (state.keys[static_cast<usize>(k)]) return true;
    }
    for (MouseButton m : mouse_buttons) {
        if (state.mouse_buttons[static_cast<usize>(m)]) return true;
    }
    for (GamepadButton g : gamepad_buttons) {
        if (state.gamepad_buttons[static_cast<usize>(g)]) return true;
    }
    return false;
}

bool InputAction::is_just_pressed(const InputState& state) const {
    for (KeyCode k : keys) {
        if (state.keys_pressed_this_frame[static_cast<usize>(k)]) return true;
    }
    for (MouseButton m : mouse_buttons) {
        if (state.mouse_pressed_this_frame[static_cast<usize>(m)]) return true;
    }
    for (GamepadButton g : gamepad_buttons) {
        if (state.gamepad_pressed_this_frame[static_cast<usize>(g)]) return true;
    }
    return false;
}

bool InputAction::is_just_released(const InputState& state) const {
    for (KeyCode k : keys) {
        if (state.keys_released_this_frame[static_cast<usize>(k)]) return true;
    }
    for (GamepadButton g : gamepad_buttons) {
        if (state.gamepad_released_this_frame[static_cast<usize>(g)]) return true;
    }
    return false;
}

// --- InputSystem ---

InputSystem& InputSystem::instance() {
    static InputSystem s;
    return s;
}

void InputSystem::init() {
    NF_LOG_INFO(LogCategory::Platform, "InputSystem initialized");
}

void InputSystem::shutdown() {
    NF_LOG_INFO(LogCategory::Platform, "InputSystem shut down");
}

void InputSystem::begin_frame() {
    // Clear per-frame state
    m_state.keys_pressed_this_frame.reset();
    m_state.keys_released_this_frame.reset();
    m_state.mouse_pressed_this_frame.reset();
    m_state.mouse_released_this_frame.reset();
    m_state.gamepad_pressed_this_frame.reset();
    m_state.gamepad_released_this_frame.reset();
    m_state.mouse_delta_x = 0.0f;
    m_state.mouse_delta_y = 0.0f;
    m_state.scroll_delta = 0.0f;
}

void InputSystem::end_frame() {
    m_prev_state = m_state;
}

bool InputSystem::is_key_down(KeyCode key) const {
    return m_state.keys[static_cast<usize>(key)];
}

bool InputSystem::is_key_pressed(KeyCode key) const {
    return m_state.keys_pressed_this_frame[static_cast<usize>(key)];
}

bool InputSystem::is_key_released(KeyCode key) const {
    return m_state.keys_released_this_frame[static_cast<usize>(key)];
}

bool InputSystem::is_mouse_down(MouseButton btn) const {
    return m_state.mouse_buttons[static_cast<usize>(btn)];
}

bool InputSystem::is_mouse_pressed(MouseButton btn) const {
    return m_state.mouse_pressed_this_frame[static_cast<usize>(btn)];
}

bool InputSystem::is_mouse_released(MouseButton btn) const {
    return m_state.mouse_released_this_frame[static_cast<usize>(btn)];
}

void InputSystem::register_action(const InputAction& action) {
    m_actions.put(action.name, action);
}

bool InputSystem::is_action_pressed(std::string_view name) const {
    const auto* action = m_actions.get(std::string(name));
    return action ? action->is_pressed(m_state) : false;
}

bool InputSystem::is_action_just_pressed(std::string_view name) const {
    const auto* action = m_actions.get(std::string(name));
    return action ? action->is_just_pressed(m_state) : false;
}

void InputSystem::on_key_down(KeyCode key) {
    auto idx = static_cast<usize>(key);
    if (idx < static_cast<usize>(KeyCode::Count)) {
        if (!m_state.keys[idx]) {
            m_state.keys_pressed_this_frame[idx] = true;
        }
        m_state.keys[idx] = true;
    }
}

void InputSystem::on_key_up(KeyCode key) {
    auto idx = static_cast<usize>(key);
    if (idx < static_cast<usize>(KeyCode::Count)) {
        m_state.keys[idx] = false;
        m_state.keys_released_this_frame[idx] = true;
    }
}

void InputSystem::on_mouse_button(MouseButton btn, bool pressed) {
    auto idx = static_cast<usize>(btn);
    if (idx < static_cast<usize>(MouseButton::Count)) {
        if (pressed) {
            if (!m_state.mouse_buttons[idx]) {
                m_state.mouse_pressed_this_frame[idx] = true;
            }
            m_state.mouse_buttons[idx] = true;
        } else {
            m_state.mouse_buttons[idx] = false;
            m_state.mouse_released_this_frame[idx] = true;
        }
    }
}

void InputSystem::on_mouse_move(f32 x, f32 y) {
    m_state.mouse_delta_x = x - m_state.mouse_x;
    m_state.mouse_delta_y = y - m_state.mouse_y;
    m_state.mouse_x = x;
    m_state.mouse_y = y;
}

void InputSystem::on_mouse_scroll(f32 delta) {
    m_state.scroll_delta = delta;
}

void InputSystem::on_gamepad_axis(GamepadAxis axis, f32 value) {
    const auto idx = static_cast<usize>(axis);
    if (idx < static_cast<usize>(GamepadAxis::Count)) {
        m_state.gamepad_axes[idx] = value;
    }
}

bool InputSystem::is_gamepad_down(GamepadButton btn) const {
    return m_state.gamepad_buttons[static_cast<usize>(btn)];
}

bool InputSystem::is_gamepad_pressed(GamepadButton btn) const {
    return m_state.gamepad_pressed_this_frame[static_cast<usize>(btn)];
}

bool InputSystem::is_gamepad_released(GamepadButton btn) const {
    return m_state.gamepad_released_this_frame[static_cast<usize>(btn)];
}

void InputSystem::on_gamepad_button(GamepadButton btn, bool pressed) {
    const auto idx = static_cast<usize>(btn);
    if (idx >= static_cast<usize>(GamepadButton::Count)) return;
    if (pressed) {
        if (!m_state.gamepad_buttons[idx]) {
            m_state.gamepad_pressed_this_frame[idx] = true;
        }
        m_state.gamepad_buttons[idx] = true;
    } else {
        if (m_state.gamepad_buttons[idx]) {
            m_state.gamepad_released_this_frame[idx] = true;
        }
        m_state.gamepad_buttons[idx] = false;
    }
}

// XInput wButtons → engine GamepadButton. Guide has no XInputGetState bit.
static GamepadButton map_xinput_button(u16 mask_bit) {
    using namespace xinput_buttons;
    switch (mask_bit) {
        case A: return GamepadButton::A;
        case B: return GamepadButton::B;
        case X: return GamepadButton::X;
        case Y: return GamepadButton::Y;
        case LeftShoulder: return GamepadButton::LeftBumper;
        case RightShoulder: return GamepadButton::RightBumper;
        case Back: return GamepadButton::Back;
        case Start: return GamepadButton::Start;
        case LeftThumb: return GamepadButton::LeftThumb;
        case RightThumb: return GamepadButton::RightThumb;
        case DPadUp: return GamepadButton::DPadUp;
        case DPadDown: return GamepadButton::DPadDown;
        case DPadLeft: return GamepadButton::DPadLeft;
        case DPadRight: return GamepadButton::DPadRight;
        default: return GamepadButton::Count;
    }
}

static f32 normalize_stick(i16 raw) {
    // Full deflection 32767 → 1.0; -32768 clamps to -1.0.
    constexpr f32 kScale = 1.0f / 32767.0f;
    f32 v = static_cast<f32>(raw) * kScale;
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

void InputSystem::apply_gamepad_sample(const GamepadSample& sample) {
    if (!sample.connected) {
        if (m_gamepad_connected) {
            for (usize i = 0; i < static_cast<usize>(GamepadButton::Count); ++i) {
                on_gamepad_button(static_cast<GamepadButton>(i), false);
            }
            for (usize i = 0; i < static_cast<usize>(GamepadAxis::Count); ++i) {
                m_state.gamepad_axes[i] = 0.0f;
            }
            m_gamepad_connected = false;
        }
        return;
    }

    m_gamepad_connected = true;

    static constexpr u16 kBits[] = {
        xinput_buttons::A,           xinput_buttons::B,
        xinput_buttons::X,           xinput_buttons::Y,
        xinput_buttons::LeftShoulder, xinput_buttons::RightShoulder,
        xinput_buttons::Back,        xinput_buttons::Start,
        xinput_buttons::LeftThumb,   xinput_buttons::RightThumb,
        xinput_buttons::DPadUp,      xinput_buttons::DPadDown,
        xinput_buttons::DPadLeft,    xinput_buttons::DPadRight,
    };
    for (u16 bit : kBits) {
        const GamepadButton btn = map_xinput_button(bit);
        if (btn == GamepadButton::Count) continue;
        on_gamepad_button(btn, (sample.buttons & bit) != 0);
    }

    m_state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftX)] = normalize_stick(sample.left_x);
    m_state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftY)] = normalize_stick(sample.left_y);
    m_state.gamepad_axes[static_cast<usize>(GamepadAxis::RightX)] = normalize_stick(sample.right_x);
    m_state.gamepad_axes[static_cast<usize>(GamepadAxis::RightY)] = normalize_stick(sample.right_y);
    m_state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftTrigger)] =
        static_cast<f32>(sample.left_trigger) / 255.0f;
    m_state.gamepad_axes[static_cast<usize>(GamepadAxis::RightTrigger)] =
        static_cast<f32>(sample.right_trigger) / 255.0f;
}

#ifndef _WIN32
void InputSystem::poll_gamepad() {
    // Non-Windows: no XInput backend; sample injection still works for tests.
}
#endif

} // namespace nf
