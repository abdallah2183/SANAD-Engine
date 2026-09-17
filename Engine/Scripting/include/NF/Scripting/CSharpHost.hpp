#pragma once

// NF/Scripting/CSharpHost.hpp — .NET hosting for C# gameplay scripting.
//
// Loads the shared .NET runtime through nethost/hostfxr (no static runtime
// dependency: machines without .NET fail loudly at initialize() instead of at
// link time) and resolves UnmanagedCallersOnly static methods out of a game
// assembly. Mirrors the Lua layer's shape on purpose: CSharpScript has the
// same Start/Update-per-entity lifecycle as ScriptComponent's update(dt), so
// a future ScriptSystem can tick both backends through one scheduler.
//
// Threading and lifetime rules (read before touching):
//   - One host per process, driven on the game thread. hostfxr entry points
//     are resolved once at initialize(); every managed call below is a plain
//     Cdecl function pointer, safe to invoke on the initializing thread.
//   - The .NET runtime boots ONCE per process: later hosts reuse it
//     (init answers Success_HostAlreadyInitialized with a valid context).
//     shutdown() closes the init context but never unloads hostfxr —
//     unloading poisons re-initialization process-wide (0x80008081), so the
//     module is intentionally leaked to OS-at-exit.
//   - Managed assemblies stay loaded for the host's lifetime (.NET cannot
//     unload the default context): loading a rebuilt game DLL needs a fresh
//     host, exactly like restarting the Lua VM.
//   - Only blittable types cross (see CSharpMarshal.hpp). Managed methods
//     must never throw across the boundary; the sandbox answers with
//     sentinels (-1 / no-op) instead.
//   - Sandboxing gap vs Lua (documented): v1 runs one shared assembly load
//     context with no instruction budgets — per-script isolation is by
//     entity-keyed state (see NFSandbox), not by VM. Do not run untrusted
//     C# through this host.
//
// Without .NET (or without the CMake-detected nethost): initialize() returns
// false with an explanatory error, every call helper returns false, and the
// hosting tests NF_SKIP. Pure-logic code (CSharpMarshal) always builds.

#include <NF/Core/Types.hpp>
#include <NF/Scripting/CSharpMarshal.hpp>

#include <string>

namespace nf::scripting {

// Cdecl entry-point signatures shared with the managed sandbox.
using CSAddFn = int (*)(int a, int b);
using CSScaleFn = float (*)(float x, float factor);
using CSEchoFn = int (*)(const u8* input, int input_len, u8* output, int output_cap);
using CSStepVec3Fn = void (*)(NetVec3* pos, NetVec3* vel, float dt);
using CSCallLogFn = void (*)(const HostApi* api, int level, const u8* message, int message_len);
using CSScriptStartFn = void (*)(CSharpEntity entity);
using CSScriptUpdateFn = void (*)(CSharpEntity entity, float dt);
using CSScriptCounterFn = int (*)(CSharpEntity entity);

/// RAII .NET host. Not copyable. initialize()/shutdown() pair like LuaVM's
/// ctor/dtor split, because hosting can legitimately fail on machines
/// without the runtime (a throwing ctor would turn "no .NET" into a crash).
class CSharpHost {
public:
    CSharpHost();
    ~CSharpHost();

    CSharpHost(const CSharpHost&) = delete;
    CSharpHost& operator=(const CSharpHost&) = delete;

    /// Boots the runtime from a .runtimeconfig.json and keeps hostfxr alive.
    /// False + out_error when nethost/hostfxr/the runtime is missing or the
    /// config is unreadable. Never throws.
    bool initialize(const std::string& runtime_config_path, std::string* out_error = nullptr);
    void shutdown();
    bool valid() const { return m_host_context != nullptr; }

    /// Resolves one UnmanagedCallersOnly static method. Loads the assembly on
    /// first use (later calls reuse it). Null + out_error when the file, the
    /// type, or the method is missing. Never throws.
    void* get_method(const std::string& assembly_path, const std::string& type_name,
                     const std::string& method_name, std::string* out_error = nullptr);

    /// True when this build can host (nethost present at configure time) AND
    /// a hostfxr resolves on this machine right now. The hosting tests skip
    /// on false; games show "install .NET 10" instead of crashing.
    static bool runtime_available();

private:
    void* m_nethost = nullptr;      // HMODULE, freed at shutdown
    void* m_hostfxr = nullptr;      // HMODULE, freed at shutdown
    void* m_host_context = nullptr; // hostfxr_handle, closed at shutdown
    void* m_load_fn = nullptr;      // load_assembly_and_get_function_pointer
};

/// One script class out of a game assembly: Start(entity) once, then
/// Update(entity, dt) per tick — the C# twin of ScriptComponent::update(dt).
/// Counter(entity) exposes the sandbox tick count (games read real state
/// through their own bindings instead).
class CSharpScript {
public:
    CSharpScript(CSharpHost& host, std::string assembly_path, std::string type_name);
    ~CSharpScript() = default;

    CSharpScript(const CSharpScript&) = delete;
    CSharpScript& operator=(const CSharpScript&) = delete;

    bool valid() const { return m_start != nullptr && m_update != nullptr; }
    const std::string& error() const { return m_error; }

    /// All three never throw; false means the host died or (counter) the
    /// method is absent. Calling start/update on an invalid script is a
    /// no-op returning false.
    bool start(CSharpEntity entity);
    bool update(CSharpEntity entity, float dt);
    bool counter(CSharpEntity entity, int& out);

private:
    CSScriptStartFn m_start = nullptr;
    CSScriptUpdateFn m_update = nullptr;
    CSScriptCounterFn m_counter = nullptr;
    std::string m_error;
};

} // namespace nf::scripting
