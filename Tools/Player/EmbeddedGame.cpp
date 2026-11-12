#include "EmbeddedGame.hpp"

#include <NF/Project/EmbeddedGameResources.hpp>

#include <fstream>
#include <iterator>
#include <span>
#include <sstream>

namespace nf::player {
namespace {

namespace fs = std::filesystem;
namespace resources = project::embedded_game;

std::span<const char> resource_bytes(HMODULE module, const wchar_t* name) {
    const HRSRC resource = FindResourceW(module, name, MAKEINTRESOURCEW(10));
    if (!resource) return {};
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) return {};
    const auto* data = static_cast<const char*>(LockResource(loaded));
    if (!data) return {};
    return {data, SizeofResource(module, resource)};
}

bool extract_files(HMODULE module, std::wistream& index, const fs::path& staging,
                   std::string& error) {
    std::size_t file_index = 0;
    std::wstring line;
    while (std::getline(index, line)) {
        if (line.size() < 2 || (line[0] != L'0' && line[0] != L'1')) {
            error = "The embedded game index contains an invalid file entry.";
            return false;
        }
        const fs::path relative(line.substr(1));
        if (!resources::valid_file_path(relative)) {
            error = "The embedded game contains an invalid asset path.";
            return false;
        }
        const auto file = staging / relative;
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (ec || fs::exists(file, ec) || ec) {
            error = "Could not prepare a file in the game data directory.";
            return false;
        }
        std::ofstream out(file, std::ios::binary);
        if (line[0] == L'1') {
            const auto bytes = resource_bytes(module, resources::file_resource_name(file_index).c_str());
            if (bytes.empty()) {
                error = "The executable is missing an embedded game asset.";
                return false;
            }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        out.close();
        if (!out) {
            error = "Could not write a game asset to the local game data directory.";
            return false;
        }
        ++file_index;
    }
    return true;
}

LRESULT CALLBACK splash_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message != WM_PAINT) return DefWindowProcW(window, message, wparam, lparam);

    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    const HBRUSH background = CreateSolidBrush(RGB(15, 23, 42));
    FillRect(dc, &bounds, background);
    DeleteObject(background);

    constexpr int size = 96;
    const HICON logo = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_SHARED));
    if (logo) DrawIconEx(dc, (bounds.right - size) / 2, 24, logo, size, size, 0, nullptr, DI_NORMAL);

    const HFONT font = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    const HGDIOBJ previous_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(241, 245, 249));
    constexpr wchar_t label[] = L"Made with SANAD Engine";
    SetTextAlign(dc, TA_CENTER | TA_TOP);
    ExtTextOutW(dc, bounds.right / 2, 139, 0, nullptr, label,
                static_cast<UINT>(std::size(label) - 1), nullptr);
    SelectObject(dc, previous_font);
    DeleteObject(font);
    EndPaint(window, &paint);
    return 0;
}

} // namespace

bool prepare_embedded_game(EmbeddedGame& game, std::string& error) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const auto bytes = resource_bytes(module, resources::kIndex);
    if (bytes.empty() || bytes.size() % sizeof(wchar_t) != 0) {
        error = "The executable does not contain a SANAD game. Export a Windows EXE from the editor.";
        return false;
    }
    std::wistringstream index(std::wstring(reinterpret_cast<const wchar_t*>(bytes.data()),
                                           bytes.size() / sizeof(wchar_t)));
    std::wstring version, build_id, save_id, filename;
    constexpr std::wstring_view hex = L"0123456789abcdefABCDEF";
    if (!std::getline(index, version) || version != resources::kVersion ||
        !std::getline(index, build_id) || build_id.size() != 36 ||
        build_id.find_first_not_of(L"0123456789abcdefABCDEF-") != std::wstring::npos ||
        !std::getline(index, save_id) || save_id.size() != 16 ||
        save_id.find_first_not_of(hex) != std::wstring::npos ||
        !std::getline(index, filename) || !resources::valid_file_path(filename) ||
        fs::path(filename).has_parent_path() || fs::path(filename).extension() != L".nfproj") {
        error = "The embedded game index is invalid.";
        return false;
    }

    std::wstring local_app_data(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(),
                                                 static_cast<DWORD>(local_app_data.size()));
    if (length == 0 || length >= local_app_data.size()) {
        error = "Windows did not provide a local application data directory.";
        return false;
    }
    local_app_data.resize(length);
    game.user_directory = fs::path(local_app_data) / "SANAD" / "Games" / save_id;
    const auto builds = game.user_directory / "Builds";
    const auto data_directory = builds / build_id;
    game.project_file = data_directory / filename;

    std::error_code ec;
    // The descriptor is visible only after the whole directory is published.
    if (fs::is_regular_file(game.project_file, ec)) return true;
    fs::create_directories(builds, ec);
    const auto staging_id = resources::unique_id();
    if (ec || staging_id.empty()) {
        error = "Could not prepare the local game data directory.";
        return false;
    }
    const auto staging_path = builds / (L".staging-" + staging_id);
    if (!fs::create_directory(staging_path, ec)) {
        error = "Could not create a staging directory for game data.";
        return false;
    }
    const resources::StagingDirectory staging{staging_path};
    if (!extract_files(module, index, staging.path, error)) return false;
    if (!fs::is_regular_file(staging.path / filename, ec)) {
        error = "The executable is missing the embedded project descriptor.";
        return false;
    }
    if (MoveFileExW(staging.path.c_str(), data_directory.c_str(), 0)) return true;
    // Another copy of the game may have published the same build while we
    // extracted ours. In that case both can use the complete directory.
    if (fs::is_regular_file(game.project_file, ec)) return true;
    error = "Could not finish preparing the local game data directory.";
    return false;
}

void show_sanad_splash() {
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = splash_proc;
    window_class.lpszClassName = L"SANADGameSplash";
    if (!RegisterClassW(&window_class)) return;

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
    constexpr int width = 360;
    constexpr int height = 194;
    const auto& area = monitor.rcWork;
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class.lpszClassName,
        L"Made with SANAD Engine", WS_POPUP, (area.left + area.right - width) / 2,
        (area.top + area.bottom - height) / 2, width, height, nullptr, nullptr,
        window_class.hInstance, nullptr);
    if (!window) return;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    Sleep(1100);
    DestroyWindow(window);
}

} // namespace nf::player
