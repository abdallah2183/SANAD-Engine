#pragma once

// NF/Platform/Window.hpp — Window abstraction

#include <NF/Core/Types.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace nf {

/// Bridge that lets a platform window implementation drive a Window's private
/// state (message pump callbacks, resize bookkeeping) without exposing those
/// internals on the public API. Each platform defines it in its own .cpp.
struct WindowPlatformAccess;

struct WindowDesc {
    u32 width = 1280;
    u32 height = 720;
    std::string title = "NOVAForge Engine";
    bool fullscreen = false;
    bool resizable = true;
    bool vsync = true;
    // Open maximized (interactive sessions; scripted runs keep the exact
    // width/height above for determinism).
    bool maximized = false;
};

struct WindowSize {
    u32 width;
    u32 height;
};

enum class WindowEvent {
    Resize,
    Close,
    FocusGained,
    FocusLost,
    Minimized,
    Maximized,
    Restored
};

using WindowEventCallback = std::function<void(WindowEvent, u32, u32)>;

/// First-chance hook for raw native messages (HWND/MSG on Windows). Installed
/// by layers that own their own platform backend (e.g. the editor forwarding
/// to ImGui_ImplWin32_WndProcHandler). Types stay backend-neutral (void* /
/// integers); returning true consumes the message and skips default handling.
/// Default: no hook, behavior unchanged.
using NativeMessageHook = std::function<bool(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam)>;

class Window {
    friend struct WindowPlatformAccess;

public:
    Window() = default;
    ~Window();

    bool create(const WindowDesc& desc);
    void destroy();

    bool should_close() const { return m_should_close; }

    void poll_events();
    void swap_buffers();

    // Set callback for window events
    void set_event_callback(WindowEventCallback cb) { m_callback = std::move(cb); }

    // Install/clear the raw native-message hook (see NativeMessageHook).
    void set_message_hook(NativeMessageHook hook) { m_message_hook = std::move(hook); }

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }

    void set_title(std::string_view title);
    std::string_view title() const { return m_title; }

    void set_vsync(bool enabled) { m_vsync = enabled; }
    bool vsync() const { return m_vsync; }

    void resize(u32 width, u32 height);

    // Platform-specific: get native handle (HWND, XWindow, etc.)
    void* native_handle() const { return m_native_handle; }

    bool is_valid() const { return m_native_handle != nullptr; }

private:
    void* m_native_handle = nullptr;
    void* m_instance = nullptr; // HINSTANCE on Windows
    std::string m_title;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_should_close = false;
    bool m_vsync = true;
    WindowEventCallback m_callback;
    NativeMessageHook m_message_hook;

    // Internal window proc dispatch
    void process_event(WindowEvent event, u32 param1, u32 param2);
};

} // namespace nf
