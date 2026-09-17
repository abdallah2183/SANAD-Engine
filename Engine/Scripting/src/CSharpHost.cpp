// NF/Scripting/CSharpHost.cpp — .NET hosting via nethost/hostfxr.
//
// Enabled only when CMake found nethost (NF_CSHARP_HOSTING=1); otherwise this
// TU is a stub that fails loudly with "not configured", so the engine still
// builds on machines without .NET and the hosting tests NF_SKIP.

#include <NF/Scripting/CSharpHost.hpp>

#include <NF/Core/Logger.hpp>

#include <string>

#if NF_CSHARP_HOSTING
#include <windows.h>

#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

// Global scope on purpose: the hosting headers spell the wide-char type as
// plain `char_t`, which would resolve to nf::char_t inside nf::scripting.
// nethost.h declares get_hostfxr_path as a function (no _fn typedef), so the
// dynamic-load signature is spelled out here once.
using NfGetHostfxrPathFn =
    int(NETHOST_CALLTYPE*)(::char_t*, size_t*, const get_hostfxr_parameters*);
const ::char_t* kNfUnmanagedCallersOnly = UNMANAGEDCALLERSONLY_METHOD;
#endif

namespace nf::scripting {

namespace {

#if NF_CSHARP_HOSTING
//get_hostfxr_path and GetProcAddress hand back raw function addresses; the
//reinterpret_cast to a function type is the documented pattern (C4191).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4191)
#endif

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<usize>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                        needed);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                            nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<usize>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                        needed, nullptr, nullptr);
    return out;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Loads nethost.dll from the configure-time packs dir and asks it where the
// newest compatible hostfxr lives. Every handle is caller-owned.
bool locate_hostfxr(std::wstring& nethost_path, std::wstring& hostfxr_path,
                    std::string* out_error) {
    nethost_path = widen(NF_NETHOST_DIR) + L"\\nethost.dll";
    const HMODULE nethost =
        LoadLibraryW(nethost_path.c_str()); // freed by the caller (host owns it)
    if (nethost == nullptr) {
        if (out_error != nullptr) *out_error = "CSharpHost: nethost.dll not found";
        return false;
    }
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4191)
#endif
    auto get_path =
        reinterpret_cast<NfGetHostfxrPathFn>(GetProcAddress(nethost, "get_hostfxr_path"));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (get_path == nullptr) {
        FreeLibrary(nethost);
        if (out_error != nullptr) *out_error = "CSharpHost: get_hostfxr_path missing";
        return false;
    }
    ::char_t buffer[MAX_PATH];
    size_t buffer_size = MAX_PATH;
    const int rc = get_path(buffer, &buffer_size, nullptr);
    FreeLibrary(nethost);
    if (rc != 0) {
        if (out_error != nullptr) {
            *out_error = "CSharpHost: no .NET runtime found (rc=" + std::to_string(rc) +
                         "); install the .NET 10 runtime";
        }
        return false;
    }
    hostfxr_path = buffer;
    return true;
}

#endif // NF_CSHARP_HOSTING

} // namespace

CSharpHost::CSharpHost() = default;

CSharpHost::~CSharpHost() {
    shutdown();
}

bool CSharpHost::initialize(const std::string& runtime_config_path, std::string* out_error) {
#if !NF_CSHARP_HOSTING
    (void)runtime_config_path;
    if (out_error != nullptr) {
        *out_error = "CSharpHost: C# hosting not configured (no nethost at build time)";
    }
    return false;
#else
    shutdown();
    if (runtime_config_path.empty()) {
        if (out_error != nullptr) *out_error = "CSharpHost: empty runtime config path";
        return false;
    }
    std::wstring nethost_path;
    std::wstring hostfxr_path;
    if (!locate_hostfxr(nethost_path, hostfxr_path, out_error)) return false;

    // Reuses the already-loaded module when present (shutdown never frees).
    HMODULE hostfxr = static_cast<HMODULE>(m_hostfxr);
    if (hostfxr == nullptr) {
        hostfxr = LoadLibraryW(hostfxr_path.c_str());
    }
    if (hostfxr == nullptr) {
        if (out_error != nullptr) *out_error = "CSharpHost: hostfxr.dll failed to load";
        return false;
    }
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4191)
#endif
    auto init_fn = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config"));
    auto delegate_fn = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate"));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (init_fn == nullptr || delegate_fn == nullptr) {
        FreeLibrary(hostfxr);
        if (out_error != nullptr) *out_error = "CSharpHost: hostfxr entry points missing";
        return false;
    }
    const std::wstring config_w = widen(runtime_config_path);
    // Explicit initialize parameters: a bare LoadLibrary(hostfxr) gives it no
    // process context (no apphost, no muxer). host_path is this executable
    // (we ARE the host); dotnet_root anchors framework resolution.
    wchar_t self[MAX_PATH];
    const DWORD self_len = GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring host_path(self_len > 0 ? self : L"NFScripting");
    // dotnet root = nearest ancestor containing dotnet.exe (pack layouts
    // vary; the exe is the stable anchor, max a few levels above nethost).
    std::wstring dotnet_root;
    std::wstring probe = nethost_path;
    for (int i = 0; i < 10; ++i) {
        const usize cut = probe.find_last_of(L"\\/");
        if (cut == std::wstring::npos) break;
        probe.resize(cut);
        if (GetFileAttributesW((probe + L"\\dotnet.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            dotnet_root = probe;
            break;
        }
    }
    // hostfxr does plain string math on this root: CMake hands us forward
    // slashes, which break its probing, so normalize to native separators.
    for (wchar_t& ch : dotnet_root) {
        if (ch == L'/') ch = L'\\';
    }
    hostfxr_initialize_parameters params{};
    params.size = sizeof(params);
    params.host_path = host_path.c_str();
    params.dotnet_root = dotnet_root.empty() ? nullptr : dotnet_root.c_str();
    hostfxr_handle context = nullptr;
    const int init_rc = init_fn(config_w.c_str(), &params, &context);
    // rc 1 = Success_HostAlreadyInitialized: the runtime lives once per
    // process, so a later host reuses it (context is still valid — verified).
    // Anything else is a real failure.
    if ((init_rc != 0 && init_rc != 1) || context == nullptr) {
        FreeLibrary(hostfxr);
        if (out_error != nullptr) {
            *out_error = "CSharpHost: runtime init failed (rc=" + std::to_string(init_rc) +
                         "); check the .runtimeconfig.json framework";
        }
        return false;
    }
    load_assembly_and_get_function_pointer_fn load_fn = nullptr;
    const int dlg_rc = delegate_fn(context, hdt_load_assembly_and_get_function_pointer,
                                   reinterpret_cast<void**>(&load_fn));
    if (dlg_rc != 0 || load_fn == nullptr) {
        auto close_fn = reinterpret_cast<hostfxr_close_fn>(
            GetProcAddress(hostfxr, "hostfxr_close"));
        if (close_fn != nullptr) close_fn(context);
        FreeLibrary(hostfxr);
        if (out_error != nullptr) {
            *out_error =
                "CSharpHost: load_assembly delegate unavailable (rc=" + std::to_string(dlg_rc) + ")";
        }
        return false;
    }
    // The HMODULEs stay loaded until shutdown(); the context until close().
    // (HMODULE is already void* compatible with the members.)
    m_nethost = nullptr; // nethost was only needed to locate hostfxr
    m_hostfxr = hostfxr;
    m_host_context = context;
    m_load_fn = reinterpret_cast<void*>(load_fn);
    NF_LOG_INFO(LogCategory::Core, "CSharpHost: .NET runtime ready ({})",
                narrow(hostfxr_path));
    return true;
#endif
}

void CSharpHost::shutdown() {
#if NF_CSHARP_HOSTING
    // Close the init context but NEVER FreeLibrary(hostfxr): unloading the
    // module poisons the process (CoreCLR global state outlives it) and every
    // later initialize fails with 0x80008081. The leaked module is by design —
    // one runtime per process, freed by the OS at exit.
    if (m_host_context != nullptr && m_hostfxr != nullptr) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4191)
#endif
        auto close_fn =
            reinterpret_cast<hostfxr_close_fn>(GetProcAddress(static_cast<HMODULE>(m_hostfxr),
                                                             "hostfxr_close"));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (close_fn != nullptr) {
            close_fn(static_cast<hostfxr_handle>(m_host_context));
        }
    }
    m_host_context = nullptr;
    m_load_fn = nullptr;
    // m_hostfxr intentionally stays loaded AND kept (see above): re-init
    // reuses it without another LoadLibrary, so the refcount never grows.
    m_nethost = nullptr;
#endif
}

void* CSharpHost::get_method(const std::string& assembly_path, const std::string& type_name,
                             const std::string& method_name, std::string* out_error) {
#if !NF_CSHARP_HOSTING
    (void)assembly_path;
    (void)type_name;
    (void)method_name;
    if (out_error != nullptr) {
        *out_error = "CSharpHost: C# hosting not configured (no nethost at build time)";
    }
    return nullptr;
#else
    if (!valid()) {
        if (out_error != nullptr) *out_error = "CSharpHost: host not initialized";
        return nullptr;
    }
    if (assembly_path.empty() || type_name.empty() || method_name.empty()) {
        if (out_error != nullptr) *out_error = "CSharpHost: empty assembly/type/method";
        return nullptr;
    }
    const std::wstring asm_w = widen(assembly_path);
    const std::wstring type_w = widen(type_name);
    const std::wstring method_w = widen(method_name);
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4191)
#endif
    auto load_fn = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(m_load_fn);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    void* fn = nullptr;
    const int rc = load_fn(asm_w.c_str(), type_w.c_str(), method_w.c_str(),
                           kNfUnmanagedCallersOnly, nullptr, &fn);
    if (rc != 0 || fn == nullptr) {
        if (out_error != nullptr) {
            *out_error = "CSharpHost: method '" + method_name + "' not found in '" +
                         assembly_path + "' (rc=" + std::to_string(rc) + ")";
        }
        return nullptr;
    }
    return fn;
#endif
}

bool CSharpHost::runtime_available() {
#if !NF_CSHARP_HOSTING
    return false;
#else
    std::wstring nethost_path;
    std::wstring hostfxr_path;
    return locate_hostfxr(nethost_path, hostfxr_path, nullptr);
#endif
}

// --- CSharpScript ------------------------------------------------------------

CSharpScript::CSharpScript(CSharpHost& host, std::string assembly_path, std::string type_name) {
    std::string error;
    auto* start = host.get_method(assembly_path, type_name, "Start", &error);
    if (start == nullptr) {
        m_error = error;
        return;
    }
    auto* update = host.get_method(assembly_path, type_name, "Update", &error);
    if (update == nullptr) {
        m_error = error;
        return;
    }
    // Counter is optional telemetry: a script without one still ticks.
    std::string counter_error;
    auto* counter = host.get_method(assembly_path, type_name, "Counter", &counter_error);
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4191)
#endif
    m_start = reinterpret_cast<CSScriptStartFn>(start);
    m_update = reinterpret_cast<CSScriptUpdateFn>(update);
    m_counter = reinterpret_cast<CSScriptCounterFn>(counter);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

bool CSharpScript::start(CSharpEntity entity) {
    if (m_start == nullptr) return false;
    m_start(entity);
    return true;
}

bool CSharpScript::update(CSharpEntity entity, float dt) {
    if (m_update == nullptr) return false;
    m_update(entity, dt);
    return true;
}

bool CSharpScript::counter(CSharpEntity entity, int& out) {
    if (m_counter == nullptr) return false;
    out = m_counter(entity);
    return true;
}

} // namespace nf::scripting
