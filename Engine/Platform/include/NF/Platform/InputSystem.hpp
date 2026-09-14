#pragma once

// NF/Platform/InputSystem.hpp — Input abstraction
// Design doc Section 50: Action-based input (Move, Jump, Fire, etc.)
// mapped to devices (Keyboard, Mouse, Controller, Touch)

#include <NF/Core/Containers.hpp>
#include <NF/Core/Types.hpp>

#include <bitset>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nf {

// --- Key codes ---
enum class KeyCode : u16 {
    Unknown = 0,

    // Letters
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Numbers
    Num0, Num1, Num2, Num3, Num4,
    Num5, Num6, Num7, Num8, Num9,

    // Function keys
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    // Navigation
    Up, Down, Left, Right,
    Home, End, PageUp, PageDown,
    Insert, Delete,

    // Modifiers
    LeftShift, RightShift,
    LeftCtrl, RightCtrl,
    LeftAlt, RightAlt,
    LeftSuper, RightSuper,

    // Special
    Space, Enter, Escape, Tab, Backspace,
    CapsLock, ScrollLock, PrintScreen, Pause,

    // Brackets
    LeftBracket, RightBracket,

    // Punctuation
    Semicolon, Apostrophe, Comma, Period, Slash, Backslash,
    Grave,

    // Numpad
    Numpad0, Numpad1, Numpad2, Numpad3, Numpad4,
    Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
    NumpadEnter, NumpadAdd, NumpadSubtract,
    NumpadMultiply, NumpadDivide, NumpadDecimal,

    Count
};

// --- Mouse buttons ---
enum class MouseButton : u8 {
    Left = 0,
    Right = 1,
    Middle = 2,
    X1 = 3,
    X2 = 4,
    Count
};

// --- Gamepad buttons ---
enum class GamepadButton : u8 {
    A = 0, B, X, Y,
    LeftBumper, RightBumper,
    Back, Start, Guide,
    LeftThumb, RightThumb,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    Count
};

// --- Gamepad axes ---
enum class GamepadAxis : u8 {
    LeftX = 0, LeftY,
    RightX, RightY,
    LeftTrigger, RightTrigger,
    Count
};

// --- Input state ---
struct InputState {
    std::bitset<static_cast<usize>(KeyCode::Count)> keys;
    std::bitset<static_cast<usize>(KeyCode::Count)> keys_pressed_this_frame;
    std::bitset<static_cast<usize>(KeyCode::Count)> keys_released_this_frame;

    std::bitset<static_cast<usize>(MouseButton::Count)> mouse_buttons;
    std::bitset<static_cast<usize>(MouseButton::Count)> mouse_pressed_this_frame;
    std::bitset<static_cast<usize>(MouseButton::Count)> mouse_released_this_frame;

    f32 mouse_x = 0.0f;
    f32 mouse_y = 0.0f;
    f32 mouse_delta_x = 0.0f;
    f32 mouse_delta_y = 0.0f;
    f32 scroll_delta = 0.0f;
};

// --- Input action mapping ---
struct InputAction {
    std::string name;
    DynamicArray<KeyCode> keys;
    DynamicArray<MouseButton> mouse_buttons;
    DynamicArray<GamepadButton> gamepad_buttons;

    bool is_pressed(const InputState& state) const;
    bool is_just_pressed(const InputState& state) const;
    bool is_just_released(const InputState& state) const;
};

class InputSystem {
public:
    static InputSystem& instance();

    void init();
    void shutdown();

    void begin_frame();
    void end_frame();

    // Query
    bool is_key_down(KeyCode key) const;
    bool is_key_pressed(KeyCode key) const;
    bool is_key_released(KeyCode key) const;

    bool is_mouse_down(MouseButton btn) const;
    bool is_mouse_pressed(MouseButton btn) const;
    bool is_mouse_released(MouseButton btn) const;

    f32 mouse_x() const { return m_state.mouse_x; }
    f32 mouse_y() const { return m_state.mouse_y; }
    f32 mouse_delta_x() const { return m_state.mouse_delta_x; }
    f32 mouse_delta_y() const { return m_state.mouse_delta_y; }
    f32 scroll_delta() const { return m_state.scroll_delta; }

    // Actions
    void register_action(const InputAction& action);
    bool is_action_pressed(std::string_view name) const;
    bool is_action_just_pressed(std::string_view name) const;

    // Internal: called by platform layer
    void on_key_down(KeyCode key);
    void on_key_up(KeyCode key);
    void on_mouse_button(MouseButton btn, bool pressed);
    void on_mouse_move(f32 x, f32 y);
    void on_mouse_scroll(f32 delta);

    const InputState& state() const { return m_state; }

private:
    InputSystem() = default;

    InputState m_state;
    InputState m_prev_state;
    HashMap<std::string, InputAction> m_actions;
};

} // namespace nf
