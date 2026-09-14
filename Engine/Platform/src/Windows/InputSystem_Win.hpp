#pragma once

// NF/Platform/Windows/InputSystem_Win.hpp — Win32 → NOVAForge input translation.
// Internal to NFPlatform; not installed as a public header.

#include <NF/Platform/InputSystem.hpp>
#include <NF/Core/Types.hpp>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

namespace nf {

/// Translates a Win32 virtual-key into an engine key code.
KeyCode translate_key_code(WPARAM wparam, LPARAM lparam);

/// Feeds a raw Win32 message into the input system.
/// Returns true when the message was consumed as input.
bool input_handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

} // namespace nf
