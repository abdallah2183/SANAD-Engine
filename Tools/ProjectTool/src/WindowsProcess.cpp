#include <NF/Project/WindowsProcess.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nf::project {

std::wstring quote_windows_argument(std::wstring_view argument) {
    // Always quote, even simple switches. This keeps the representation stable
    // and the parser below only has to preserve two special cases: embedded
    // quotes and runs of backslashes immediately before either quote.
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');

    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(ch);
        }
        backslashes = 0;
    }

    // Backslashes before the closing quote would escape it unless doubled.
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring build_windows_command_line(const std::vector<std::wstring>& arguments) {
    std::wstring command_line;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) command_line.push_back(L' ');
        command_line += quote_windows_argument(arguments[i]);
    }
    return command_line;
}

std::filesystem::path executable_directory() {
#if !defined(_WIN32)
    return {};
#else
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(buffer.data()).parent_path();
#endif
}

int run_windows_process_and_wait(const std::filesystem::path& executable,
                                 const std::filesystem::path& working_directory,
                                 const std::vector<std::string>& arguments,
                                 std::string& out_error) {
    out_error.clear();
#if !defined(_WIN32)
    (void)executable;
    (void)working_directory;
    (void)arguments;
    out_error = "direct Windows process launching is unavailable on this platform";
    return 1;
#else
    if (executable.empty()) {
        out_error = "process executable is empty";
        return 1;
    }

    std::vector<std::wstring> wide_arguments;
    wide_arguments.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        // The CLI receives narrow argv, whose paths use the active Windows code
        // page. filesystem performs the matching native conversion.
        wide_arguments.push_back(std::filesystem::path(argument).wstring());
    }
    // CreateProcess's command line still supplies the child's argv[0], even
    // though lpApplicationName makes the executable path unambiguous. Quoting
    // argv[0] directly avoids cmd.exe entirely while preserving normal CRT
    // argument parsing inside NFPlayer (and any tool that inspects argv[0]).
    std::wstring command_line = quote_windows_argument(executable.wstring());
    const std::wstring argument_line = build_windows_command_line(wide_arguments);
    if (!argument_line.empty()) {
        command_line.push_back(L' ');
        command_line += argument_line;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
        CREATE_DEFAULT_ERROR_MODE, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process);
    if (started == 0) {
        const DWORD error = GetLastError();
        out_error = "failed to start '" + executable.string() + "' (Windows error " +
                    std::to_string(error) + ")";
        return 1;
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    if (wait_result != WAIT_OBJECT_0 || GetExitCodeProcess(process.hProcess, &exit_code) == 0) {
        const DWORD error = GetLastError();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        out_error = "failed while waiting for '" + executable.string() +
                    "' (Windows error " + std::to_string(error) + ")";
        return 1;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
#endif
}

} // namespace nf::project
