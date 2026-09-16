// NF/Core/CrashHandler.cpp — SEH filter + minidump writer (Windows).

#include <NF/Core/CrashHandler.hpp>
#include <NF/Core/Logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h> // types only; the DLL is loaded at runtime (see below)
#endif

namespace nf {

namespace {

std::mutex& crash_mutex() {
    static std::mutex m;
    return m;
}

CrashHandlerConfig& crash_config() {
    static CrashHandlerConfig cfg;
    return cfg;
}

std::atomic<bool>& crash_installed() {
    static std::atomic<bool> installed{false};
    return installed;
}

#if defined(_WIN32)

// DbgHelp is loaded at runtime (no link dependency, graceful degradation).
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          const PMINIDUMP_EXCEPTION_INFORMATION,
                                          const PMINIDUMP_USER_STREAM_INFORMATION,
                                          const PMINIDUMP_CALLBACK_INFORMATION);

std::string dump_file_path(EXCEPTION_POINTERS* info) {
    (void)info;
    const CrashHandlerConfig cfg = crash_config();
    if (cfg.dump_directory.empty()) return {};
    // Timestamped name: prefix_YYYYMMDD_HHMMSS.dmp (local time).
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_value{};
    localtime_s(&tm_value, &t);
    char stamp[32]{};
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", tm_value.tm_year + 1900,
                  tm_value.tm_mon + 1, tm_value.tm_mday, tm_value.tm_hour, tm_value.tm_min,
                  tm_value.tm_sec);
    std::string dir = cfg.dump_directory;
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    CreateDirectoryA(dir.c_str(), nullptr); // best effort; failure is fine
    return dir + "\\" + cfg.dump_prefix + "_" + stamp + ".dmp";
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    // Re-entrancy guard: a crash inside the handler must not recurse.
    static std::atomic<bool> in_handler{false};
    if (in_handler.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;

    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    // NOTE: Logger::log dispatches to sinks synchronously, so this line is
    // already on its way out; there is no cross-sink flush to call (file
    // sinks buffer in the CRT — the dump is the durable artifact).
    NF_LOG_FATAL(LogCategory::Core, "Fatal exception 0x{:08X}; writing minidump", code);

    const std::string path = dump_file_path(info);
    if (!path.empty()) {
        if (HMODULE dbg = LoadLibraryA("Dbghelp.dll")) {
            if (auto writer = reinterpret_cast<MiniDumpWriteDumpFn>(
                    GetProcAddress(dbg, "MiniDumpWriteDump"))) {
                HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    MINIDUMP_EXCEPTION_INFORMATION ex{};
                    ex.ThreadId = GetCurrentThreadId();
                    ex.ExceptionPointers = info;
                    ex.ClientPointers = FALSE;
                    const BOOL ok = writer(GetCurrentProcess(), GetCurrentProcessId(), file,
                                           MiniDumpNormal, &ex, nullptr, nullptr);
                    CloseHandle(file);
                    NF_LOG_FATAL(LogCategory::Core, "Minidump {} ({})", path,
                                 ok ? "written" : "FAILED");
                }
            }
            FreeLibrary(dbg);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER; // terminate (a dump is not a recovery)
}

#endif // _WIN32

} // namespace

bool install_crash_handler(const CrashHandlerConfig& config) {
    std::lock_guard lock(crash_mutex());
    crash_config() = config;
#if defined(_WIN32)
    SetUnhandledExceptionFilter(crash_filter);
#endif
    crash_installed().store(true);
    NF_LOG_INFO(LogCategory::Core, "Crash handler armed (dumps: {})",
                config.dump_directory.empty() ? "<disabled>" : config.dump_directory);
    return true;
}

void uninstall_crash_handler() {
    std::lock_guard lock(crash_mutex());
#if defined(_WIN32)
    SetUnhandledExceptionFilter(nullptr);
#endif
    crash_installed().store(false);
}

bool crash_handler_installed() {
    return crash_installed().load();
}

} // namespace nf
