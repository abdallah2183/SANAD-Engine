// NF/Platform/Windows/GamepadWin.cpp — XInput poll → GamepadSample.
//
// Dynamically loads xinput1_4/1_3/9_1_0 so the binary runs on machines with
// no XInput redistributable; a missing DLL is a no-op pad, not a load failure.
// Polls user slots 0–3 and keeps the first connected controller (Xbox layout).

#include <NF/Platform/InputSystem.hpp>
#include <NF/Core/Logger.hpp>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

namespace nf {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct XInputBackend {
    HMODULE module = nullptr;
    XInputGetStateFn get_state = nullptr;
    bool tried = false;
    DWORD active_slot = 0;
    bool has_active = false;
};

XInputBackend& backend() {
    static XInputBackend b;
    return b;
}

void ensure_loaded() {
    XInputBackend& b = backend();
    if (b.tried) return;
    b.tried = true;

    static const char* kNames[] = {
        "xinput1_4.dll",   // Windows 8+
        "xinput1_3.dll",   // DirectX SDK
        "xinput9_1_0.dll", // Vista / Win7
    };
    for (const char* name : kNames) {
        if (HMODULE mod = LoadLibraryA(name)) {
            b.module = mod;
            b.get_state = reinterpret_cast<XInputGetStateFn>(
                GetProcAddress(mod, "XInputGetState"));
            if (b.get_state) break;
        }
    }
    if (!b.get_state) {
        NF_LOG_WARN(LogCategory::Platform,
                    "XInput unavailable — gamepad poll disabled");
    }
}

bool read_slot(DWORD slot, XINPUT_STATE& out) {
    XInputBackend& b = backend();
    if (!b.get_state) return false;
    return b.get_state(slot, &out) == ERROR_SUCCESS;
}

} // namespace

void InputSystem::poll_gamepad() {
    ensure_loaded();
    XInputBackend& b = backend();
    if (!b.get_state) return;

    XINPUT_STATE state{};
    bool connected = false;
    DWORD found = b.active_slot;

    if (b.has_active && read_slot(b.active_slot, state)) {
        found = b.active_slot;
        connected = true;
    } else {
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (read_slot(slot, state)) {
                found = slot;
                connected = true;
                break;
            }
        }
    }

    if (!connected) {
        if (b.has_active) {
            b.has_active = false;
            apply_gamepad_sample(GamepadSample{});
        }
        return;
    }

    b.active_slot = found;
    b.has_active = true;

    const XINPUT_GAMEPAD& pad = state.Gamepad;
    GamepadSample sample{};
    sample.connected = true;
    sample.buttons = pad.wButtons;
    sample.left_x = pad.sThumbLX;
    sample.left_y = pad.sThumbLY;
    sample.right_x = pad.sThumbRX;
    sample.right_y = pad.sThumbRY;
    sample.left_trigger = pad.bLeftTrigger;
    sample.right_trigger = pad.bRightTrigger;
    apply_gamepad_sample(sample);
}

} // namespace nf
