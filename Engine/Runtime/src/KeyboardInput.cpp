// NF/Runtime/KeyboardInput.cpp — polling keyboard source for the game window.

#include <NF/Runtime/KeyboardInput.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// GetAsyncKeyState rather than window messages: the game window may not have
// focus the instant the user starts moving, and a polling source behaves the
// same whether it does or not — the same trade the editor's play input made.
[[nodiscard]] bool key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

} // namespace
#endif

namespace nf::runtime {

bool KeyboardInputSource::action_pressed(const std::string_view action) const {
#ifdef _WIN32
    if (action == "move_forward" || action == "forward" || action == "up") {
        return key_down(VK_UP) || key_down('W');
    }
    if (action == "move_back" || action == "back" || action == "down") {
        return key_down(VK_DOWN) || key_down('S');
    }
    if (action == "move_left" || action == "left") {
        return key_down(VK_LEFT) || key_down('A');
    }
    if (action == "move_right" || action == "right") {
        return key_down(VK_RIGHT) || key_down('D');
    }
    if (action == "jump") {
        return key_down(VK_SPACE);
    }
    if (action == "sprint") {
        return key_down(VK_SHIFT);
    }
    if (action == "interact") {
        return key_down('E');
    }
    // OrbitCamera legacy axes (keyboard fallback).
    if (action == "look") {
        return key_down(VK_LEFT) || key_down(VK_RIGHT);
    }
    if (action == "zoom") {
        return key_down(VK_UP) || key_down(VK_DOWN);
    }
#else
    (void)action;
#endif
    return false;
}

float KeyboardInputSource::action_axis(const std::string_view action) const {
#ifdef _WIN32
    if (action == "move_x") {
        float v = 0.0f;
        if (key_down(VK_LEFT) || key_down('A')) {
            v -= 1.0f;
        }
        if (key_down(VK_RIGHT) || key_down('D')) {
            v += 1.0f;
        }
        return v;
    }
    if (action == "move_z") {
        float v = 0.0f;
        if (key_down(VK_UP) || key_down('W')) {
            v -= 1.0f;
        }
        if (key_down(VK_DOWN) || key_down('S')) {
            v += 1.0f;
        }
        return v;
    }
    if (action == "look") {
        float v = 0.0f;
        if (key_down(VK_LEFT)) {
            v -= 1.0f;
        }
        if (key_down(VK_RIGHT)) {
            v += 1.0f;
        }
        return v;
    }
    if (action == "zoom") {
        float v = 0.0f;
        if (key_down(VK_UP)) {
            v -= 1.0f;
        }
        if (key_down(VK_DOWN)) {
            v += 1.0f;
        }
        return v;
    }
#else
    (void)action;
#endif
    return 0.0f;
}

} // namespace nf::runtime
