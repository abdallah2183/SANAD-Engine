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
