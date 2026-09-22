// NF/Core/CrashHandler.cpp — SEH filter + minidump writer (Windows).

#include <NF/Core/CrashHandler.hpp>
#include <NF/Core/Logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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

// Human-readable twin of the minidump: the .txt a user can attach to a bug
// report without knowing what WinDbg is. snprintf only — this runs inside a
// crashing process where the heap may be corrupt, so no iostream formatting.
bool write_report_unlocked(const std::string& report_path,
                           DWORD exception_code,
                           DWORD thread_id,
                           const std::string& minidump_path,
                           bool minidump_written,
                           const std::string& app_version) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_s(&tm_utc, &t);
    char stamp[32]{};
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min,
                  tm_utc.tm_sec);
    char text[1024]{};
    std::snprintf(text, sizeof(text),
                  "NOVAForge crash report\n"
                  "======================\n"
                  "time:       %s\n"
                  "version:    %s\n"
                  "exception:  0x%08lX\n"
                  "thread:     %lu\n"
                  "minidump:   %s (%s)\n"
                  "\n"
                  "A minidump is a binary artifact; open it in WinDbg / Visual Studio\n"
                  "against the matching build to get a stack. This .txt is the quick,\n"
                  "human-readable summary users can send in with a bug report.\n",
                  stamp, app_version.empty() ? "<unknown>" : app_version.c_str(),
                  static_cast<unsigned long>(exception_code),
                  static_cast<unsigned long>(thread_id),
                  minidump_path.empty() ? "<none>" : minidump_path.c_str(),
                  minidump_written ? "written" : "FAILED");

    HANDLE file = CreateFileA(report_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok =
        WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE;
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
    std::string report_path;
    bool dump_written = false;
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
                    dump_written = ok != FALSE;
                    NF_LOG_FATAL(LogCategory::Core, "Minidump {} ({})", path,
                                 ok ? "written" : "FAILED");
                }
            }
            FreeLibrary(dbg);
        }
        // The readable twin lives beside the dump whatever happened to it — a
        // FAILED dump line in a report is itself worth receiving.
        report_path = path.substr(0, path.size() - 4) + ".txt";
        write_report_unlocked(report_path, code, GetCurrentThreadId(), path, dump_written,
                              crash_config().app_version);
    }
    return EXCEPTION_EXECUTE_HANDLER; // terminate (a dump is not a recovery)
}

#endif // _WIN32

} // namespace

bool write_crash_report(const std::string& report_path,
                        unsigned long exception_code,
                        unsigned long thread_id,
                        const std::string& minidump_path,
                        bool minidump_written,
                        const std::string& app_version) {
#if defined(_WIN32)
    return write_report_unlocked(report_path, static_cast<DWORD>(exception_code),
                                 static_cast<DWORD>(thread_id), minidump_path, minidump_written,
                                 app_version);
#else
    (void)report_path; (void)exception_code; (void)thread_id;
    (void)minidump_path; (void)minidump_written; (void)app_version;
    return false;
#endif
}

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
