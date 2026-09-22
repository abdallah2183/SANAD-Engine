// NF/Platform/Windows/Window_Win.cpp — Win32 window implementation

#include <NF/Platform/Window.hpp>
#include <NF/Platform/InputSystem.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Assert.hpp>

#include "InputSystem_Win.hpp"

#include <cstring>

// InputSystem_Win.hpp already defines NOMINMAX + WIN32_LEAN_AND_MEAN before
// including windows.h, so the macros are active before any Win32 API call here.

namespace nf {

// Map our window pointer to the HWND for the window proc
// We store the Window* in the user data of the HWND

// Definition of the platform bridge declared in Window.hpp.
struct WindowPlatformAccess {
    static bool forward_message(Window* window, void* hwnd, uint32_t msg, uint64_t wparam,
                                int64_t lparam) {
        if (window && window->m_message_hook) {
            return window->m_message_hook(hwnd, msg, wparam, lparam);
        }
        return false;
    }
    static void set_close_requested(Window* window, bool value) {
        window->m_should_close = value;
    }
    static void set_size(Window* window, u32 width, u32 height) {
        window->m_width = width;
        window->m_height = height;
    }
    static void notify(Window* window, WindowEvent event, u32 a, u32 b) {
        window->process_event(event, a, b);
    }
};

namespace {

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Window* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // First-chance hook (e.g. ImGui's Win32 backend). A consumed message skips
    // everything below, including DefWindowProc.
    if (WindowPlatformAccess::forward_message(window, hwnd, msg, wparam, lparam)) {
        return 0;
    }

    // Input is pumped first: the window proc owns the raw message stream and
    // the input system is platform-agnostic.
    if (input_handle_message(hwnd, msg, wparam, lparam) && window) {
        // Consumed as input; fall through to DefWindowProc for the rest.
    }

    switch (msg) {
        case WM_CLOSE: {
            if (window) {
                WindowPlatformAccess::set_close_requested(window, true);
                WindowPlatformAccess::notify(window, WindowEvent::Close, 0, 0);
            }
            return 0;
        }

        case WM_SIZE: {
            if (window) {
                const u32 w = LOWORD(lparam);
                const u32 h = HIWORD(lparam);

                WindowPlatformAccess::set_size(window, w, h);

                if (w == 0 && h == 0) {
                    WindowPlatformAccess::notify(window, WindowEvent::Minimized, 0, 0);
                } else {
                    WindowPlatformAccess::notify(window, WindowEvent::Resize, w, h);
                }
            }
            return 0;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }

        case WM_SETFOCUS: {
            if (window) WindowPlatformAccess::notify(window, WindowEvent::FocusGained, 0, 0);
            return 0;
        }

        case WM_KILLFOCUS: {
            if (window) WindowPlatformAccess::notify(window, WindowEvent::FocusLost, 0, 0);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

bool Window::create(const WindowDesc& desc) {
    m_width = desc.width;
    m_height = desc.height;
    m_title = desc.title;
    m_vsync = desc.vsync;

    HINSTANCE hInstance = GetModuleHandleW(nullptr);

    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"NOVAForgeWindowClass";

    if (!RegisterClassExW(&wc)) {
        NF_LOG_ERROR(LogCategory::Platform, "Failed to register window class");
        return false;
    }

    // Adjust window rect for client area size
    RECT rc{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!desc.resizable) {
        style &= ~WS_THICKFRAME;
    }
    if (desc.fullscreen) {
        style = WS_POPUP;
    }
    AdjustWindowRect(&rc, style, FALSE);

    u32 window_width = rc.right - rc.left;
    u32 window_height = rc.bottom - rc.top;

    // Convert title to wide string
    int wlen = MultiByteToWideChar(CP_UTF8, 0, desc.title.c_str(), -1, nullptr, 0);
    std::wstring wtitle(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, desc.title.c_str(), -1, wtitle.data(), wlen);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        wtitle.c_str(),
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        window_width, window_height,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        NF_LOG_ERROR(LogCategory::Platform, "Failed to create window (error: {})", GetLastError());
        return false;
    }

    // Store pointer in user data for the window proc
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    m_native_handle = static_cast<void*>(hwnd);
    m_instance = static_cast<void*>(hInstance);

    // Maximized first (SW_SHOWMAXIMIZED posts WM_SIZE synchronously, so
    // width()/height() already report the maximized client area below).
    ShowWindow(hwnd, desc.maximized ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    NF_LOG_INFO(LogCategory::Platform, "Window created: {}x{} '{}'",
                desc.width, desc.height, desc.title);

    return true;
}

void Window::destroy() {
    if (m_native_handle) {
        HWND hwnd = static_cast<HWND>(m_native_handle);
        DestroyWindow(hwnd);
        m_native_handle = nullptr;
    }
}

Window::~Window() {
    destroy();
}

void Window::poll_events() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_should_close = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // XInput after the pump: pad state lands in the same frame as key events.
    InputSystem::instance().poll_gamepad();
}

void Window::swap_buffers() {
    // Swapchain is managed by the RHI, not the window.
    // This is a no-op here; the RHI backend handles present.
}

void Window::set_title(std::string_view title) {
    m_title = std::string(title);
    if (m_native_handle) {
        HWND hwnd = static_cast<HWND>(m_native_handle);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, m_title.c_str(), -1, nullptr, 0);
        std::wstring wtitle(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, m_title.c_str(), -1, wtitle.data(), wlen);
        SetWindowTextW(hwnd, wtitle.c_str());
    }
}

void Window::resize(u32 width, u32 height) {
    if (m_native_handle) {
        HWND hwnd = static_cast<HWND>(m_native_handle);
        RECT rc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
        AdjustWindowRect(&rc, style, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        m_width = width;
        m_height = height;
    }
}

void Window::process_event(WindowEvent event, u32 param1, u32 param2) {
    if (m_callback) {
        m_callback(event, param1, param2);
    }
}

} // namespace nf
