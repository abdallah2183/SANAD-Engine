// NF/Runtime/KeyboardInput.cpp — polling keyboard, pointer and pad source for
// the game window.

#include <NF/Runtime/KeyboardInput.hpp>

#include <NF/Platform/InputSystem.hpp>
#include <NF/Runtime/PlayerLook.hpp>

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// GetAsyncKeyState rather than window messages: the game window may not have
// focus the instant the user starts moving, and a polling source behaves the
// same whether it does or not — the same trade the editor's play input made.
[[nodiscard]] bool key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
#endif

/// The radius below which a stick is considered centred. XInput sticks rest
/// a little off zero on real hardware, and a pad that reports 0.03 forever
/// walks the player across the map while nobody is touching it.
constexpr float kStickDeadzone = 0.18f;

// --- the device, as the binding evaluator sees it ---------------------------
//
// Free functions with an opaque context, so the evaluator stays a pure
// function of (table, action, device) and a test can hand it a fake device.

bool read_key(const void* /*context*/, nf::u16 code) {
#ifdef _WIN32
    return key_down(static_cast<int>(code));
#else
    (void)code;
    return false;
#endif
}

bool read_pad_button(const void* /*context*/, nf::u16 code) {
    return nf::InputSystem::instance().is_gamepad_down(static_cast<nf::GamepadButton>(code));
}

// Mouse buttons are read through the SAME platform query the pointer capture
// already uses (VK_LBUTTON etc.), not through window messages: a message-based
// state needs a pump that runs before the frame, and the capture code has
// worked without one since 0.4.0. One query, one answer, whether the caller is
// asking "did the player click to take the pointer back" or "is attack held".
bool read_mouse_button(const void* /*context*/, nf::u16 code) {
#ifdef _WIN32
    return key_down(static_cast<int>(code));
#else
    (void)code;
    return false;
#endif
}

float read_pad_axis(const void* context, nf::u16 code) {
    if (context == nullptr) {
        return 0.0f;
    }
    // The deadzone was applied to the PAIR in begin_frame(), so this returns
    // an already-centred value: a per-axis deadzone here would let a resting
    // stick creep diagonally.
    return static_cast<const nf::runtime::KeyboardInputSource*>(context)->pad_axis_value(code);
}

} // namespace

namespace nf::runtime {

float KeyboardInputSource::pad_axis_value(u16 index) const {
    return index < 6 ? m_pad_axes[index] : 0.0f;
}

DeviceState KeyboardInputSource::device_state() const {
    DeviceState state;
    state.context = this;
    state.key_down = &read_key;
    state.pad_down = &read_pad_button;
    state.pad_axis = &read_pad_axis;
    state.mouse_down = &read_mouse_button;
    return state;
}

KeyboardInputSource::~KeyboardInputSource() {
    // A destroyed source must not leave the pointer hidden and clipped: the
    // window is about to belong to somebody else, and an invisible cursor is
    // the hardest bug there is to explain to a user.
    set_pointer_capture(false);
}

void KeyboardInputSource::set_window(void* native_window) {
    if (native_window == m_window) {
        return;
    }
    set_pointer_capture(false);
    m_window = native_window;
    m_have_last = false;
}

void KeyboardInputSource::set_bindings(InputBindings bindings) {
    m_bindings = std::move(bindings);
}

void KeyboardInputSource::begin_frame() {
    m_dx = 0.0f;
    m_dy = 0.0f;
    m_wheel = 0.0f;
    for (float& axis : m_pad_axes) {
        axis = 0.0f;
    }

#ifdef _WIN32
    if (m_window != nullptr) {
        auto* hwnd = static_cast<HWND>(m_window);
        // The pointer is only worth capturing for a window the player is
        // actually in. A background window that clips the cursor makes the
        // whole desktop feel broken.
        const bool foreground = GetForegroundWindow() == hwnd;

        // Esc gives the pointer back. A captured cursor cannot reach the
        // close button, so without this the only way out is Alt+F4 — which
        // a player experiences as a frozen machine, not as a game.
        if (m_captured && key_down(VK_ESCAPE)) {
            set_pointer_capture(false);
            m_capture_suppressed = true;
        }
        // ...and a click inside the window takes it back, which is the
        // gesture every game uses for the same moment.
        const bool left_down = key_down(VK_LBUTTON);
        if (m_capture_suppressed && foreground && left_down && !m_left_was_down) {
            m_capture_suppressed = false;
        }
        m_left_was_down = left_down;

        POINT cursor{};
        if (GetCursorPos(&cursor) != 0) {
            if (m_captured) {
                // A captured cursor is held at the centre of the window, so
                // the offset from the centre IS this frame's movement. The
                // cursor is put back immediately: the next frame measures
                // from the same place, which is what makes a small desk
                // enough to turn through a full circle.
                m_dx = static_cast<float>(cursor.x - m_center_x);
                m_dy = static_cast<float>(cursor.y - m_center_y);
                if (m_dx != 0.0f || m_dy != 0.0f) {
                    SetCursorPos(m_center_x, m_center_y);
                }
            } else if (m_have_last) {
                // Not captured: the delta is still real, so a game that only
                // wants "drag to look" needs no capture at all.
                m_dx = static_cast<float>(cursor.x - m_last_x);
                m_dy = static_cast<float>(cursor.y - m_last_y);
            }
            m_last_x = cursor.x;
            m_last_y = cursor.y;
            m_have_last = true;
        }
    }
#endif

    // The wheel is a window message, not a pollable state, so it comes from
    // the platform input the window pump already filled. Reading it here and
    // then beginning the platform's frame is what makes it an EDGE: one
    // notch turns into one zoom step, instead of the wheel being "still
    // turned" for as long as the value is not cleared.
    auto& platform_input = InputSystem::instance();
    m_wheel = platform_input.scroll_delta();
    m_pad_connected = platform_input.gamepad_connected();
    if (m_pad_connected) {
        // One deadzone per stick, on the pair: an axis-wise test would let a
        // resting stick creep diagonally. Triggers are not sticks and are
        // left alone — a trigger's rest position IS zero.
        f32 left_x = platform_input.gamepad_axis(GamepadAxis::LeftX);
        f32 left_y = platform_input.gamepad_axis(GamepadAxis::LeftY);
        f32 right_x = platform_input.gamepad_axis(GamepadAxis::RightX);
        f32 right_y = platform_input.gamepad_axis(GamepadAxis::RightY);
        stick_deadzone(left_x, left_y, kStickDeadzone);
        stick_deadzone(right_x, right_y, kStickDeadzone);
        m_pad_axes[0] = left_x;
        m_pad_axes[1] = left_y;
        m_pad_axes[2] = right_x;
        m_pad_axes[3] = right_y;
        m_pad_axes[4] = platform_input.gamepad_axis(GamepadAxis::LeftTrigger);
        m_pad_axes[5] = platform_input.gamepad_axis(GamepadAxis::RightTrigger);
    }
    platform_input.begin_frame();
}

bool KeyboardInputSource::look_delta(float& dx, float& dy) const {
    if (m_window == nullptr) {
        dx = 0.0f;
        dy = 0.0f;
        return false;
    }
    dx = m_dx;
    dy = m_dy;
    return true;
}

float KeyboardInputSource::wheel_delta() const {
    return m_wheel;
}

void KeyboardInputSource::set_pointer_capture(bool captured) {
#ifdef _WIN32
    if (m_window == nullptr) {
        m_captured = false;
        return;
    }
    if (captured == m_captured) {
        return;
    }
    auto* hwnd = static_cast<HWND>(m_window);
    if (captured) {
        // The player gave the pointer back, or the window is not the one in
        // front: either way, no capture.
        if (m_capture_suppressed || GetForegroundWindow() != hwnd) {
            return;
        }
        RECT client{};
        if (GetClientRect(hwnd, &client) == 0) {
            return; // no client area, no capture: leave the pointer alone
        }
        POINT top_left{client.left, client.top};
        POINT bottom_right{client.right, client.bottom};
        ClientToScreen(hwnd, &top_left);
        ClientToScreen(hwnd, &bottom_right);
        RECT clip{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
        m_center_x = (top_left.x + bottom_right.x) / 2;
        m_center_y = (top_left.y + bottom_right.y) / 2;
        // Clip first, then hide, then centre: centring before the clip would
        // let the cursor land outside the window and generate one huge
        // delta on the first frame of play.
        if (ClipCursor(&clip) == 0) {
            return;
        }
        while (ShowCursor(FALSE) >= 0) {
        }
        SetCursorPos(m_center_x, m_center_y);
        m_have_last = false;
        m_captured = true;
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {
        }
        m_have_last = false;
        m_captured = false;
    }
#else
    (void)captured;
#endif
}

bool KeyboardInputSource::action_pressed(const std::string_view action) const {
    return binding_pressed(m_bindings, action, device_state());
}

float KeyboardInputSource::action_axis(const std::string_view action) const {
    return binding_axis(m_bindings, action, device_state());
}

} // namespace nf::runtime
