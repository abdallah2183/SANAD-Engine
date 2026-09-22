#pragma once

// NF/Core/CrashHandler.hpp — crash dumps and hard-error reporting
// (design doc Section 88).
//
// install_crash_handler() arms a last-resort filter: an unhandled Win32
// structured exception (access violation, stack overflow, ...) writes a
// timestamped .dmp next to the log directory via DbgHelp (loaded at runtime,
// so machines without it simply skip the dump) and flushes the logger before
// the process dies. Non-Windows builds get a safe no-op with the same API.
//
// The handler never allocates from the engine heap and never throws: it runs
// on a crashing process where the heap may already be corrupt.

#include <NF/Core/Types.hpp>

#include <string>

namespace nf {

struct CrashHandlerConfig {
    /// Directory for .dmp files (created when missing). Empty = disabled dumps
    /// (the logger still flushes).
    std::string dump_directory;
    /// Prefix for dump file names ("NOVAForge_20260916_221500.dmp").
    std::string dump_prefix = "NOVAForge";
    /// Version string stamped into the crash report ("MyGame 1.2.0 nf 0.1").
    /// A crash dump without a version is untriageable, so shipped builds set
    /// this; empty means the report omits the line.
    std::string app_version;
};

/// Arms the handler. Safe to call twice (re-arms with the new config).
/// Returns false only when the platform cannot arm one (never on Windows).
bool install_crash_handler(const CrashHandlerConfig& config);

/// Disarms. After this, crashes behave as if the handler was never installed.
void uninstall_crash_handler();

bool crash_handler_installed();

/// Writes a human-readable .txt crash report beside a minidump. Called by the
/// crash filter itself (so every dump ships with its readable twin) and exposed
/// so tools that recover a dump out-of-band can regenerate the report.
/// Never throws; on failure returns false and leaves out_error set.
bool write_crash_report(const std::string& report_path,
                        unsigned long exception_code,
                        unsigned long thread_id,
                        const std::string& minidump_path,
                        bool minidump_written,
                        const std::string& app_version);

} // namespace nf
