#pragma once

// NF/Scripting/CSharpMarshal.hpp — blittable interop vocabulary for C# scripting.
//
// Everything crossing the native/managed boundary is a C-compatible pod:
// fixed-size integers, floats, raw pointers, and the structs below. No GC
// handles, no strings, no exceptions cross — strings travel as (pointer,
// byte-length) UTF-8 spans with caller-owned buffers, and every managed entry
// point answers corruption with a sentinel instead of throwing. The layouts
// here are asserted byte-exact so a C# LayoutKind.Sequential mirror matches.

#include <NF/Core/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace nf::scripting {

// Mirror of the managed Vec3f (3 x float, sequential, 12 bytes).
struct NetVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

static_assert(sizeof(NetVec3) == 12, "NetVec3 must be 12 bytes for the C# mirror");
static_assert(alignof(NetVec3) == 4, "NetVec3 must be 4-aligned for the C# mirror");
static_assert(offsetof(NetVec3, x) == 0, "NetVec3.x offset");
static_assert(offsetof(NetVec3, y) == 4, "NetVec3.y offset");
static_assert(offsetof(NetVec3, z) == 8, "NetVec3.z offset");

// Host API table handed to managed code (first field read by NFSandbox.CallLog).
// LogFn is a Cdecl void(int level, const u8* msg_bytes, int msg_len); the
// message is UTF-8, NOT null-terminated — use the length.
struct HostApi {
    using LogFn = void (*)(int level, const u8* msg_bytes, int msg_len);
    LogFn log = nullptr;
};

static_assert(sizeof(HostApi) == sizeof(void*), "HostApi must be one pointer");

// UI host API table handed to managed code (Game-Ready G3) so a C# game script
// can drive the runtime game UI through the same nf::ui::GameFlow the C++ and
// Lua paths use. `user` is opaque to managed code and echoed back verbatim on
// every call (the bound ui::GameFlow). All function pointers are Cdecl.
//
// String arguments travel as (UTF-8 bytes, length) spans — NOT null-terminated,
// exactly like HostApi's log callback. `action` is a nf::ui::Action value and
// `screen` a nf::ui::Screen value (both fit in int). Every entry point may be
// null; managed callers must null-check before calling (the sandbox does).
//
// Built by nf::scripting::make_ui_host_api (UiBindings.hpp) so the table's
// layout lives next to its only producer; mirrored by NFSandbox's UiHostApi
// struct in the managed sandbox.
struct UiHostApi {
    void* user = nullptr; // opaque owner (ui::GameFlow*)

    using HandleFn = void (*)(void* user, int action);       // ui::GameFlow::handle
    using ScreenFn = int (*)(void* user);                    // ui::GameFlow::screen
    using SetHealthFn = void (*)(void* user, float current, float max);
    using SetAmmoFn = void (*)(void* user, int magazine, int reserve);
    using MessageFn = void (*)(void* user, const u8* text, int len, float seconds);
    using GameOverFn = void (*)(void* user);                 // notify_game_over
    using UpdateFn = void (*)(void* user, float dt);         // Hud message timer
    using SetVolumeFn = int (*)(void* user, const u8* bus, int len, float value); // 1/0
    using VolumeFn = float (*)(void* user, const u8* bus, int len);               // -1 unknown

    HandleFn handle = nullptr;
    ScreenFn screen = nullptr;
    SetHealthFn set_health = nullptr;
    SetAmmoFn set_ammo = nullptr;
    MessageFn show_message = nullptr;
    GameOverFn notify_game_over = nullptr;
    UpdateFn update = nullptr;
    SetVolumeFn set_volume = nullptr;
    VolumeFn volume = nullptr;
};

static_assert(std::is_standard_layout_v<UiHostApi>,
              "UiHostApi crosses to managed code as a plain sequential struct");
static_assert(offsetof(UiHostApi, user) == 0, "UiHostApi.user must be first");

// Entity ids cross as signed 64-bit (C# long). Negative ids are rejected by
// convention: the engine never issues them, so one is corruption.
using CSharpEntity = long long;

// Copies UTF-8 bytes into a caller-owned buffer (the Echo protocol both sides
// implement). Returns bytes written, or -1 when the text does not fit / the
// buffer is null (capacity 0 with empty text writes nothing and returns 0).
long long write_string_to_buffer(std::string_view text, u8* out, usize capacity);

// Dotted managed type name: "Namespace.Type, Assembly" (the exact string
// load_assembly_and_get_function_pointer expects).
std::string csharp_type_name(std::string_view assembly, std::string_view dotted_type);

} // namespace nf::scripting
