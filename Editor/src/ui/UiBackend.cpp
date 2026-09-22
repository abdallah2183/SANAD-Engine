// UiBackend.cpp — ImGui context, docking, style, Win32 backend hookup.
//
// ImGui_ImplWin32_Init integrates HWND input; raw messages reach it through
// Window::set_message_hook (see ui_handle_win32_message). Drawing goes
// through UiRenderer (RHI-only); the stock ImGui_ImplVulkan backend stays
// compiled for reference but is not executed.

#include <NF/Editor/UiShell.hpp>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Declared this way on purpose: the backend header keeps it inside `#if 0`
// to avoid dragging <windows.h> into every includer.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);
#endif

namespace nf::editor {

namespace {

// Latin + Arabic blocks + presentation forms (the shaper emits FE70-FEFF).
// General Punctuation rides along: mixed EN/AR lines the editor draws (console
// messages, entity names, the About box) use dashes and curly quotes, and
// shaper pass-through keeps them as-is — without the range they rasterise as
// tofu inside an otherwise-Arabic line.
const ImWchar* arabic_glyph_ranges() {
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1
        0x0600, 0x06FF, // Arabic
        0x0750, 0x077F, // Arabic Supplement
        0x08A0, 0x08FF, // Arabic Extended-A
        0x2010, 0x202F, // General Punctuation (dashes, quotes, ellipsis)
        0xFB50, 0xFDFF, // Arabic Presentation Forms-A
        0xFE70, 0xFEFF, // Arabic Presentation Forms-B
        0,
    };
    return kRanges;
}

// Walks up from the working directory for Resources/fonts/<file> (the same
// root-finding the VFS uses for Content/). Returns empty when absent.
std::string find_bundled_font(const char* file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) return {};
    for (int i = 0; i < 6; ++i) {
        const fs::path cand = dir / "Resources" / "fonts" / file;
        if (fs::exists(cand, ec) && !ec) return cand.string();
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

ImFont* load_arabic_font(ImGuiIO& io) {
    const std::string path = find_bundled_font("Amiri-Regular.ttf");
    if (path.empty()) return nullptr;
    ImFontConfig cfg;
    cfg.MergeMode = true; // merge into the active (Latin) font
    cfg.PixelSnapH = true;
    return io.Fonts->AddFontFromFileTTF(path.c_str(), 16.0f, &cfg, arabic_glyph_ranges());
}

} // namespace

UiInitResult ui_init(void* hwnd) {
    UiInitResult out;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    out.context_ok = (ImGui::GetCurrentContext() != nullptr);
    if (!out.context_ok) {
        return out;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // deterministic layout, no imgui.ini writes

    // System UI font first, embedded default as fallback.
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f);
    if (font != nullptr) {
        out.font_used = "Segoe UI";
    } else {
        io.Fonts->AddFontDefault();
        out.font_used = "ImGui default";
    }
    // Arabic companion font (Phase 15): merged into the same atlas so Latin
    // and shaped Arabic (presentation forms, see NF/UI/ArabicShaper) render
    // in one draw. Resolved from Resources/fonts next to the project root;
    // absence only disables Arabic glyphs, never the editor.
    if (ImFont* arabic = load_arabic_font(io)) {
        (void)arabic;
        out.font_used += " + Amiri (AR)";
    }
    // The font atlas texture itself is uploaded by UiRenderer::init() (GPU
    // path, waited). Building here as well satisfies NewFrame()'s TexIsBuilt
    // check even if a frame is recorded before the renderer binds the font.
    io.Fonts->Build();

    // Dark graphite, single accent, flat (no gradients/glass/animation).
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.FrameRounding = 2.0f;
    st.PopupRounding = 2.0f;
    st.ScrollbarRounding = 2.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.149f, 0.153f, 0.169f, 1.0f); // graphite, not black
    c[ImGuiCol_ChildBg] = ImVec4(0.149f, 0.153f, 0.169f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.180f, 0.184f, 0.200f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.098f, 0.102f, 0.114f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.208f, 0.212f, 0.227f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.251f, 0.259f, 0.278f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.290f, 0.302f, 0.325f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.129f, 0.133f, 0.145f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.169f, 0.176f, 0.196f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.298f, 0.553f, 1.000f, 0.35f); // the one accent
    c[ImGuiCol_HeaderHovered] = ImVec4(0.298f, 0.553f, 1.000f, 0.55f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.298f, 0.553f, 1.000f, 0.75f);
    c[ImGuiCol_Button] = ImVec4(0.298f, 0.553f, 1.000f, 0.40f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.298f, 0.553f, 1.000f, 0.60f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.298f, 0.553f, 1.000f, 0.85f);
    c[ImGuiCol_CheckMark] = ImVec4(0.298f, 0.553f, 1.000f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.298f, 0.553f, 1.000f, 0.70f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.298f, 0.553f, 1.000f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.169f, 0.176f, 0.196f, 1.0f);
    c[ImGuiCol_TabActive] = ImVec4(0.208f, 0.216f, 0.235f, 1.0f);
    c[ImGuiCol_DockingPreview] = ImVec4(0.298f, 0.553f, 1.000f, 0.40f);

    // Win32 platform backend (HWND input). Raw messages arrive via
    // Window::set_message_hook -> ui_handle_win32_message (installed by the
    // shell); async-key shortcuts keep working regardless.
    if (hwnd != nullptr) {
        out.win32_ok = ImGui_ImplWin32_Init(hwnd);
    }
    return out;
}

bool ui_handle_win32_message(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam) {
#ifdef _WIN32
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), static_cast<UINT>(msg),
                                   static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
#else
    (void)hwnd;
    (void)msg;
    (void)wparam;
    (void)lparam;
#endif
    return false; // never consumes; engine input + DefWindowProc always run
}

void ui_end_frame() {
    ImGui::Render();
}

void ui_shutdown() {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

} // namespace nf::editor
