// NF/Platform/Windows/InputSystem_Win.cpp — Win32 input translation

#include "InputSystem_Win.hpp"

#include <NF/Core/Logger.hpp>

namespace nf {

KeyCode translate_key_code(WPARAM wparam, LPARAM lparam) {
    // Letters and digits map straight onto ASCII.
    if (wparam >= 'A' && wparam <= 'Z') {
        return static_cast<KeyCode>(static_cast<u16>(KeyCode::A) +
                                    (static_cast<u16>(wparam) - 'A'));
    }
    if (wparam >= '0' && wparam <= '9') {
        return static_cast<KeyCode>(static_cast<u16>(KeyCode::Num0) +
                                    (static_cast<u16>(wparam) - '0'));
    }

    // Function keys F1..F12
    if (wparam >= VK_F1 && wparam <= VK_F12) {
        return static_cast<KeyCode>(static_cast<u16>(KeyCode::F1) +
                                    (static_cast<u16>(wparam) - VK_F1));
    }

    // Distinguish left/right modifiers using the extended-key bit, exactly as
    // Win32 requires — VK_SHIFT alone cannot tell them apart.
    const bool is_extended = (lparam & 0x01000000) != 0;

    switch (wparam) {
        case VK_SHIFT:
            return is_extended ? KeyCode::RightShift : KeyCode::LeftShift;
        case VK_CONTROL:
            return is_extended ? KeyCode::RightCtrl : KeyCode::LeftCtrl;
        case VK_MENU:
            return is_extended ? KeyCode::RightAlt : KeyCode::LeftAlt;
        case VK_LWIN:  return KeyCode::LeftSuper;
        case VK_RWIN:  return KeyCode::RightSuper;

        case VK_UP:      return KeyCode::Up;
        case VK_DOWN:    return KeyCode::Down;
        case VK_LEFT:    return KeyCode::Left;
        case VK_RIGHT:   return KeyCode::Right;
        case VK_HOME:    return KeyCode::Home;
        case VK_END:     return KeyCode::End;
        case VK_PRIOR:   return KeyCode::PageUp;
        case VK_NEXT:    return KeyCode::PageDown;
        case VK_INSERT:  return KeyCode::Insert;
        case VK_DELETE:  return KeyCode::Delete;

        case VK_SPACE:      return KeyCode::Space;
        case VK_RETURN:     return KeyCode::Enter;
        case VK_ESCAPE:     return KeyCode::Escape;
        case VK_TAB:        return KeyCode::Tab;
        case VK_BACK:       return KeyCode::Backspace;
        case VK_CAPITAL:    return KeyCode::CapsLock;
        case VK_SCROLL:     return KeyCode::ScrollLock;
        case VK_SNAPSHOT:   return KeyCode::PrintScreen;
        case VK_PAUSE:      return KeyCode::Pause;

        case VK_OEM_4:      return KeyCode::LeftBracket;
        case VK_OEM_6:      return KeyCode::RightBracket;
        case VK_OEM_1:      return KeyCode::Semicolon;
        case VK_OEM_7:      return KeyCode::Apostrophe;
        case VK_OEM_COMMA:  return KeyCode::Comma;
        case VK_OEM_PERIOD: return KeyCode::Period;
        case VK_OEM_2:      return KeyCode::Slash;
        case VK_OEM_5:      return KeyCode::Backslash;
        case VK_OEM_3:      return KeyCode::Grave;

        case VK_NUMPAD0:    return KeyCode::Numpad0;
        case VK_NUMPAD1:    return KeyCode::Numpad1;
        case VK_NUMPAD2:    return KeyCode::Numpad2;
        case VK_NUMPAD3:    return KeyCode::Numpad3;
        case VK_NUMPAD4:    return KeyCode::Numpad4;
        case VK_NUMPAD5:    return KeyCode::Numpad5;
        case VK_NUMPAD6:    return KeyCode::Numpad6;
        case VK_NUMPAD7:    return KeyCode::Numpad7;
        case VK_NUMPAD8:    return KeyCode::Numpad8;
        case VK_NUMPAD9:    return KeyCode::Numpad9;
        case VK_ADD:        return KeyCode::NumpadAdd;
        case VK_SUBTRACT:   return KeyCode::NumpadSubtract;
        case VK_MULTIPLY:   return KeyCode::NumpadMultiply;
        case VK_DIVIDE:     return KeyCode::NumpadDivide;
        case VK_DECIMAL:    return KeyCode::NumpadDecimal;

        default:
            return KeyCode::Unknown;
    }
}

bool input_handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)hwnd;

    InputSystem& input = InputSystem::instance();

    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            // Ignore auto-repeat: wparam bit 30 is set for repeated keys, and
            // "is just pressed" must fire only on the initial press.
            const bool is_repeat = (lparam & (1 << 30)) != 0;
            if (!is_repeat) {
                const KeyCode key = translate_key_code(wparam, lparam);
                if (key != KeyCode::Unknown) {
                    input.on_key_down(key);
                }
            }
            return true;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const KeyCode key = translate_key_code(wparam, lparam);
            if (key != KeyCode::Unknown) {
                input.on_key_up(key);
            }
            return true;
        }

        case WM_MOUSEMOVE: {
            const f32 x = static_cast<f32>(GET_X_LPARAM(lparam));
            const f32 y = static_cast<f32>(GET_Y_LPARAM(lparam));
            input.on_mouse_move(x, y);
            return true;
        }

        case WM_LBUTTONDOWN: input.on_mouse_button(MouseButton::Left, true);   return true;
        case WM_LBUTTONUP:   input.on_mouse_button(MouseButton::Left, false);  return true;
        case WM_RBUTTONDOWN: input.on_mouse_button(MouseButton::Right, true);  return true;
        case WM_RBUTTONUP:   input.on_mouse_button(MouseButton::Right, false); return true;
        case WM_MBUTTONDOWN: input.on_mouse_button(MouseButton::Middle, true); return true;
        case WM_MBUTTONUP:   input.on_mouse_button(MouseButton::Middle, false);return true;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            const bool pressed = (msg == WM_XBUTTONDOWN);
            const MouseButton button = (GET_XBUTTON_WPARAM(wparam) == XBUTTON1)
                                           ? MouseButton::X1
                                           : MouseButton::X2;
            input.on_mouse_button(button, pressed);
            return true;
        }

        case WM_MOUSEWHEEL: {
            // 120 is one detent on a standard wheel; normalize to +/-1.
            const i32 delta = GET_WHEEL_DELTA_WPARAM(wparam);
            input.on_mouse_scroll(static_cast<f32>(delta) / static_cast<f32>(WHEEL_DELTA));
            return true;
        }

        default:
            return false;
    }
}

} // namespace nf
