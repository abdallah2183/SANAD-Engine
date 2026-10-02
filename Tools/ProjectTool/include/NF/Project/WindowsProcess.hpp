#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nf::project {

/// Quotes one argument for the Windows C runtime command-line parser.
/// Backslashes are doubled where an embedded quote or the final quote would
/// otherwise escape them.
std::wstring quote_windows_argument(std::wstring_view argument);

/// Builds only the argument portion of a Windows command line. The executable
/// is passed separately to CreateProcessW as lpApplicationName, so paths and
/// spaces never pass through cmd.exe quote parsing.
std::wstring build_windows_command_line(const std::vector<std::wstring>& arguments);

/// Returns the directory containing the running Windows executable. This is
/// deliberately based on the loaded module rather than argv[0], because a tool
/// launched through PATH may receive only its command name.
std::filesystem::path executable_directory();

/// Starts `executable` directly with CreateProcessW, waits for it, and returns
/// its exit code. `arguments` excludes argv[0]. On failure, returns 1 and fills
/// `out_error`; there is no shell fallback.
int run_windows_process_and_wait(const std::filesystem::path& executable,
                                 const std::filesystem::path& working_directory,
                                 const std::vector<std::string>& arguments,
                                 std::string& out_error);

} // namespace nf::project
