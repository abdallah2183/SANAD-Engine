#include <NF/Project/AndroidPreviewExport.hpp>
#include <NF/Project/WindowsProcess.hpp>

#include <array>
#include <chrono>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nf::project {

#ifdef _WIN32
AndroidPreviewResult export_android_preview(const std::filesystem::path& project,
                                           const std::filesystem::path& repo) {
    AndroidPreviewResult result;
    const auto script = repo / L"Tools" / L"AndroidPlayer" / L"build_project_preview.ps1";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(script, ec)) {
        result.error = L"Android preview tools are missing from the engine source folder.";
        return result;
    }
    const auto output = project.parent_path() / L"dist-android";
    std::filesystem::create_directories(output, ec);
    if (ec || !std::filesystem::is_directory(output, ec)) {
        result.error = L"Could not create the project's dist-android folder.";
        return result;
    }
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto base = project.stem().wstring() + L"-preview-" + std::to_wstring(stamp);
    result.apk = output / (base + L".apk");
    result.log = output / (base + L".log.txt");

    std::array<wchar_t, MAX_PATH + 1> system_dir{};
    const UINT length = GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
    if (length == 0 || length >= system_dir.size()) {
        result.error = L"Could not locate Windows PowerShell.";
        return result;
    }
    const auto powershell = std::filesystem::path(system_dir.data()) /
                            L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    const std::vector<std::wstring> args{L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy",
                                         L"Bypass", L"-File", script.wstring(), L"-Project",
                                         project.wstring(), L"-OutputApk", result.apk.wstring()};
    std::wstring command = nf::project::quote_windows_argument(powershell.wstring()) + L" " +
                           nf::project::build_windows_command_line(args);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(result.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        result.error = L"Could not create the Android preview log.";
        return result;
    }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        CloseHandle(log);
        result.error = L"Could not prepare the Android preview process.";
        return result;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE, nullptr,
                                        repo.c_str(), &startup, &process);
    const DWORD start_error = started ? 0 : GetLastError();
    CloseHandle(input);
    CloseHandle(log);
    if (!started) {
        result.error = L"Could not start the Android preview tools (Windows error " +
                       std::to_wstring(start_error) + L").";
        return result;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    const bool exited = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!exited || exit_code != 0 || !std::filesystem::is_regular_file(result.apk, ec)) {
        result.error = L"Android preview failed. Check the build log and Android SDK setup.";
    }
    return result;
}
#else
AndroidPreviewResult export_android_preview(const std::filesystem::path&, const std::filesystem::path&) {
    AndroidPreviewResult result;
    result.error = L"Android preview export is available on Windows only.";
    return result;
}
#endif

} // namespace nf::project
