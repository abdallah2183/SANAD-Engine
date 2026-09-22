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

// XInput `wButtons` layout (xinput.h) duplicated so headless tests and the
// poll path share one mask without including Windows headers.
namespace xinput_buttons {
constexpr u16 DPadUp = 0x0001;
constexpr u16 DPadDown = 0x0002;
constexpr u16 DPadLeft = 0x0004;
constexpr u16 DPadRight = 0x0008;
constexpr u16 Start = 0x0010;
constexpr u16 Back = 0x0020;
constexpr u16 LeftThumb = 0x0040;
constexpr u16 RightThumb = 0x0080;
constexpr u16 LeftShoulder = 0x0100;
constexpr u16 RightShoulder = 0x0200;
constexpr u16 A = 0x1000;
constexpr u16 B = 0x2000;
constexpr u16 X = 0x4000;
constexpr u16 Y = 0x8000;
} // namespace xinput_buttons

/// One raw gamepad reading from the platform backend (XInput on Windows).
/// Tests build the same struct so the mapping is provable without hardware.
struct GamepadSample {
    bool connected = false;
    u16 buttons = 0; // XInput-style wButtons mask
    i16 left_x = 0;
    i16 left_y = 0;
    i16 right_x = 0;
    i16 right_y = 0; // stick axes in [-32768, 32767], Y up-positive
    u8 left_trigger = 0;
    u8 right_trigger = 0; // triggers in [0, 255]
};

// --- Input state ---
struct InputState {
    std::bitset<static_cast<usize>(KeyCode::Count)> keys;
    std::bitset<static_cast<usize>(KeyCode::Count)> keys_pressed_this_frame;
    std::bitset<static_cast<usize>(KeyCode::Count)> keys_released_this_frame;

    std::bitset<static_cast<usize>(MouseButton::Count)> mouse_buttons;
    std::bitset<static_cast<usize>(MouseButton::Count)> mouse_pressed_this_frame;
    std::bitset<static_cast<usize>(MouseButton::Count)> mouse_released_this_frame;

    std::bitset<static_cast<usize>(GamepadButton::Count)> gamepad_buttons;
    std::bitset<static_cast<usize>(GamepadButton::Count)> gamepad_pressed_this_frame;
    std::bitset<static_cast<usize>(GamepadButton::Count)> gamepad_released_this_frame;

    // Analog gamepad axes in [-1, 1]. Zero when no device is attached.
    // Triggers are reported in [0, 1].
    f32 gamepad_axes[static_cast<usize>(GamepadAxis::Count)] = {};

    f32 mouse_x = 0.0f;
    f32 mouse_y = 0.0f;
    f32 mouse_delta_x = 0.0f;
    f32 mouse_delta_y = 0.0f;
    f32 scroll_delta = 0.0f;

    // Live gamepad axis reading in [-1, 1] (no deadzone applied here; the
    // gameplay-facing InputMapper owns deadzone policy).
    f32 axis(GamepadAxis axis) const {
        const auto idx = static_cast<usize>(axis);
        return idx < static_cast<usize>(GamepadAxis::Count) ? gamepad_axes[idx] : 0.0f;
    }
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

    bool is_gamepad_down(GamepadButton btn) const;
    bool is_gamepad_pressed(GamepadButton btn) const;
    bool is_gamepad_released(GamepadButton btn) const;

    /// True while any XInput slot reported a connected pad last poll.
    bool gamepad_connected() const { return m_gamepad_connected; }

    f32 mouse_x() const { return m_state.mouse_x; }
    f32 mouse_y() const { return m_state.mouse_y; }
    f32 mouse_delta_x() const { return m_state.mouse_delta_x; }
    f32 mouse_delta_y() const { return m_state.mouse_delta_y; }
    f32 scroll_delta() const { return m_state.scroll_delta; }
    f32 gamepad_axis(GamepadAxis axis) const { return m_state.axis(axis); }

    /// Reads hardware (XInput slots 0–3 on Windows) and applies the sample.
    /// Called from Window::poll_events after the message pump. No-op off Windows.
    void poll_gamepad();

    /// Maps one raw sample onto the live state (edges, axes, disconnect).
    /// Public so headless tests can inject a fake device without a driver.
    void apply_gamepad_sample(const GamepadSample& sample);

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
    void on_gamepad_axis(GamepadAxis axis, f32 value);
    void on_gamepad_button(GamepadButton btn, bool pressed);

    const InputState& state() const { return m_state; }

private:
    InputSystem() = default;

    InputState m_state;
    InputState m_prev_state;
    HashMap<std::string, InputAction> m_actions;
    bool m_gamepad_connected = false;
};

} // namespace nf
