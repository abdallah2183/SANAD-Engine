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
    return false;
}

bool InputAction::is_just_pressed(const InputState& state) const {
    for (KeyCode k : keys) {
        if (state.keys_pressed_this_frame[static_cast<usize>(k)]) return true;
    }
    for (MouseButton m : mouse_buttons) {
        if (state.mouse_pressed_this_frame[static_cast<usize>(m)]) return true;
    }
    return false;
}

bool InputAction::is_just_released(const InputState& state) const {
    for (KeyCode k : keys) {
        if (state.keys_released_this_frame[static_cast<usize>(k)]) return true;
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

void InputSystem::on_gamepad_button(GamepadButton btn, bool pressed) {
    m_state.gamepad_buttons[static_cast<usize>(btn)] = pressed;
}

} // namespace nf
