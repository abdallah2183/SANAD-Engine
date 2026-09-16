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
};

/// Arms the handler. Safe to call twice (re-arms with the new config).
/// Returns false only when the platform cannot arm one (never on Windows).
bool install_crash_handler(const CrashHandlerConfig& config);

/// Disarms. After this, crashes behave as if the handler was never installed.
void uninstall_crash_handler();

bool crash_handler_installed();

} // namespace nf
