// UiBackend.cpp — ImGui context, docking, style, Win32 backend hookup.
//
// ImGui_ImplWin32_Init integrates HWND input; raw messages reach it through
// Window::set_message_hook (see ui_handle_win32_message). Drawing goes
// through UiRenderer (RHI-only); the stock ImGui_ImplVulkan backend stays
// compiled for reference but is not executed.

#include <NF/Editor/UiShell.hpp>
#include <NF/Editor/UiTheme.hpp>

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

// One size for the whole atlas. The Latin face and the merged Arabic face MUST
// agree: while they disagreed (15 vs 16) every Arabic label sat a pixel taller
// than the Latin text next to it on the same line, which reads as a broken
// baseline rather than as a localisation.
constexpr float kUiFontSize = 16.0f;

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

// Walks up from `start` for Resources/fonts/<file>. Returns empty when absent.
std::string walk_up_for_font(const std::filesystem::path& start, const char* file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = start;
    for (int i = 0; i < 6; ++i) {
        const fs::path cand = dir / "Resources" / "fonts" / file;
        if (fs::exists(cand, ec) && !ec) return cand.string();
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

// Where Resources/fonts lives, most specific first.
//
// `NF_FONT_DIR` exists because walking up cannot work for a PACKAGED build. The
// walk assumes the engine is inside a source tree that happens to contain
// Resources/fonts/ at its root, which is true of a checkout and false of a
// shipped folder — the beta on the Desktop has no parent that looks like the
// engine, so every Arabic label rendered as tofu with nothing in the log. The
// packaging script now copies the font in, and the build passes the absolute
// path so no walk is needed at all in the common case. The walk stays as the
// fallback for a source-tree run, which is what the automation uses.
#ifndef NF_FONT_DIR
    #define NF_FONT_DIR ""
#endif
const char* kFontDirOverride = NF_FONT_DIR;

// Where the dock layout is remembered between sessions.
//
// The editor used to run with IniFilename = nullptr so the layout was always
// the built-in default. That is right for a scripted run and wrong for a human:
// every engine this one is compared against remembers where you put the panels,
// and a splitter dragged into place and lost on the next launch reads as the
// editor forgetting your work. So a human session persists the layout and a
// scripted one still rebuilds the default (see ui_init's `persist_layout`).
//
// The string must outlive ImGui's use of it — ImGui keeps the pointer.
const char* layout_ini_path() {
    static const std::string path = [] {
        std::string dir = ".";
#ifdef _WIN32
        char buf[MAX_PATH * 4] = {};
        if (::GetEnvironmentVariableA("APPDATA", buf, sizeof(buf)) != 0) {
            dir = buf;
            dir += "\\SANAD";
        }
#else
        dir = ".";
#endif
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir + "\\editor_layout.ini";
    }();
    return path.c_str();
}

// Where the Arabic companion font lives.
//
// Two roots, in order:
//   1. the working directory, walked up — the engine checkout's own layout,
//      which is what the original implementation assumed;
//   2. the EDITOR EXECUTABLE's directory, walked up.
//
// The second one is not belt-and-braces, it is the fix for a real defect. The
// engine ships the font, not the game project, and `nf new` tells a developer to
// put their project in Documents — so the moment anyone opens a project from
// outside the engine tree, the working-directory walk finds nothing, the editor
// silently falls back to a Latin-only system font, and every Arabic label in
// the UI rasterises as tofu. Nothing logs a warning, because "no Arabic font" is
// a legal state for an English session; it just looks like the Arabic
// localisation is broken.
//
// Note this is the *engine's* resource, unlike Content/ and Cache/, which a
// project declares through its own mounts — which is exactly why it must be
// resolved against the executable and not against the project.
std::string find_bundled_font(const char* file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // 1. The build's own answer, absolute. A packaged build's Resources/fonts is
    //    beside the exe, which no upward walk can reach.
    if (kFontDirOverride != nullptr && kFontDirOverride[0] != '\0') {
        const fs::path cand = fs::path(kFontDirOverride) / file;
        if (fs::exists(cand, ec) && !ec) return cand.string();
    }
    // 2. Beside the executable — a package layout, before any walking.
#ifdef _WIN32
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) != 0) {
        const fs::path exedir = fs::path(exe_path).parent_path();
        const fs::path direct = exedir / "Resources" / "fonts" / file;
        if (fs::exists(direct, ec) && !ec) return direct.string();
        const std::string walked = walk_up_for_font(exedir, file);
        if (!walked.empty()) return walked;
    }
#endif
    // 3. The working directory, walked up: a source-tree run.
    const std::string from_cwd = walk_up_for_font(fs::current_path(ec), file);
    if (!from_cwd.empty()) {
        return from_cwd;
    }
    return {};
}

ImFont* load_arabic_font(ImGuiIO& io) {
    const std::string path = find_bundled_font("Amiri-Regular.ttf");
    if (path.empty()) return nullptr;
    ImFontConfig cfg;
    cfg.MergeMode = true; // merge into the active (Latin) font
    cfg.PixelSnapH = true;
    return io.Fonts->AddFontFromFileTTF(path.c_str(), kUiFontSize, &cfg, arabic_glyph_ranges());
}

// --- Forge Dark v2 -----------------------------------------------------------
//
// The palette is a ladder, and the ladder is the design. From the void behind
// the panels up to an active control there are five perceptual steps, each
// roughly 5-7% apart in lightness, so no two states of the same widget can be
// mistaken for one another at a glance — which is what makes a dense tool feel
// calm instead of noisy.
//
//   0  void      #14161B   dockspace, scrollbar trough, title bar
//   1  panel     #1B1E24   every docked panel
//   2  raised    #22262D   popups, menu bar, table headers, status bars
//   3  control   #2A2F37   input frames
//   4  hover     #343B45
//   5  active    #3E4652
//
// One accent (default #4C8DFF) is reserved for selection, focus and primary
// actions; everything semantic (danger / success / warning) is a fixed colour
// so a destructive button never inherits the user's accent. See UiTheme.hpp.
void apply_forge_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();

    // --- geometry ---------------------------------------------------------
    st.WindowRounding = 6.0f;
    st.ChildRounding = 5.0f;
    st.FrameRounding = 5.0f;
    st.PopupRounding = 8.0f;
    st.ScrollbarRounding = 8.0f;
    st.GrabRounding = 4.0f;
    st.TabRounding = 4.0f;
    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 1.0f;
    st.PopupBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.TabBarBorderSize = 1.0f;
    st.TabBarOverlineSize = 2.0f;
    st.SeparatorTextBorderSize = 2.0f;
    st.DockingSeparatorSize = 2.0f;
    // Roomier than ImGui's defaults (8,4 / 8,4): the toolbar and the dock
    // buttons used to stick together, and a 16px font needs the vertical air.
    st.WindowPadding = ImVec2(12.0f, 10.0f);
    st.FramePadding = ImVec2(10.0f, 6.0f);
    st.CellPadding = ImVec2(6.0f, 4.0f);
    st.ItemSpacing = ImVec2(9.0f, 6.0f);
    st.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    st.IndentSpacing = 22.0f;
    st.ScrollbarSize = 14.0f;
    st.GrabMinSize = 10.0f;
    st.DisabledAlpha = 0.45f;
    // Centred button text: the toolbar's Local/World toggle and the panel
    // action rows read as buttons rather than as left-aligned labels.
    st.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    // No collapse arrow in panel title bars — a docked panel is not a
    // collapsible section, and the arrow competed with the tab bar above it.
    st.WindowMenuButtonPosition = ImGuiDir_None;
    st.AntiAliasedLines = true;
    st.AntiAliasedFill = true;

    const ImVec4 accent = theme::accent();
    const auto tint = [&accent](float a) { return theme::alpha(accent, a); };

    ImVec4* c = st.Colors;
    // Surfaces (the ladder above).
    c[ImGuiCol_WindowBg] = theme::surface();
    c[ImGuiCol_ChildBg] = theme::rgb(0x1E, 0x22, 0x28);
    c[ImGuiCol_PopupBg] = theme::surface_raised();
    c[ImGuiCol_MenuBarBg] = theme::surface_raised();
    c[ImGuiCol_DockingEmptyBg] = theme::surface_sunken();
    c[ImGuiCol_ScrollbarBg] = theme::surface_sunken();
    c[ImGuiCol_Border] = theme::rgb(0x2B, 0x31, 0x3A);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    // Text.
    c[ImGuiCol_Text] = theme::text_primary();
    c[ImGuiCol_TextDisabled] = theme::text_faint();
    c[ImGuiCol_TextSelectedBg] = tint(0.35f);
    c[ImGuiCol_InputTextCursor] = accent;

    // Controls.
    c[ImGuiCol_FrameBg] = theme::rgb(0x2A, 0x2F, 0x37);
    c[ImGuiCol_FrameBgHovered] = theme::rgb(0x34, 0x3B, 0x45);
    c[ImGuiCol_FrameBgActive] = theme::rgb(0x3E, 0x46, 0x52);
    c[ImGuiCol_CheckboxSelectedBg] = tint(0.80f);

    // Scrollbars: the grab is the only part that needs to be visible.
    c[ImGuiCol_ScrollbarGrab] = theme::rgb(0x3A, 0x42, 0x4E);
    c[ImGuiCol_ScrollbarGrabHovered] = theme::rgb(0x47, 0x50, 0x5E);
    c[ImGuiCol_ScrollbarGrabActive] = theme::rgb(0x55, 0x60, 0x6F);

    // Accent family: selection, headers, primary buttons, sliders.
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = tint(0.75f);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = tint(0.35f);
    c[ImGuiCol_ButtonHovered] = tint(0.55f);
    c[ImGuiCol_ButtonActive] = tint(0.80f);
    c[ImGuiCol_Header] = tint(0.28f);
    c[ImGuiCol_HeaderHovered] = tint(0.48f);
    c[ImGuiCol_HeaderActive] = tint(0.68f);
    c[ImGuiCol_Separator] = theme::rgb(0x2B, 0x31, 0x3A);
    c[ImGuiCol_SeparatorHovered] = tint(0.55f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = tint(0.20f);
    c[ImGuiCol_ResizeGripHovered] = tint(0.45f);
    c[ImGuiCol_ResizeGripActive] = tint(0.70f);

    // Title bars: only the focused window lifts, so the active panel is
    // obvious in a four-way split without any extra chrome.
    c[ImGuiCol_TitleBg] = theme::surface_sunken();
    c[ImGuiCol_TitleBgActive] = theme::surface_raised();
    c[ImGuiCol_TitleBgCollapsed] = theme::surface_sunken();

    // Tabs: an unselected tab melts into the panel; the selected one lifts and
    // carries an accent overline (the 1.93 "selected tab" affordance).
    c[ImGuiCol_Tab] = theme::surface();
    c[ImGuiCol_TabHovered] = tint(0.35f);
    c[ImGuiCol_TabSelected] = theme::rgb(0x26, 0x2C, 0x35);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = theme::surface_sunken();
    c[ImGuiCol_TabDimmedSelected] = theme::rgb(0x20, 0x24, 0x2B);
    c[ImGuiCol_TabDimmedSelectedOverline] = tint(0.35f);

    // Docking feedback.
    c[ImGuiCol_DockingPreview] = tint(0.40f);

    // Tables.
    c[ImGuiCol_TableHeaderBg] = theme::surface_raised();
    c[ImGuiCol_TableBorderStrong] = theme::rgb(0x2B, 0x31, 0x3A);
    c[ImGuiCol_TableBorderLight] = theme::rgb(0x23, 0x27, 0x2E);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.015f);

    // Feedback.
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_DragDropTargetBg] = tint(0.15f);
    c[ImGuiCol_UnsavedMarker] = theme::warning();
    c[ImGuiCol_NavCursor] = tint(0.60f);
    c[ImGuiCol_TextLink] = accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.04f, 0.04f, 0.05f, 0.60f);
}

} // namespace

// The banner flag. OUTSIDE the anonymous namespace on purpose: main.cpp sets it
// and Panels.cpp reads it, so it needs external linkage. Declared in UiShell.hpp.
bool g_arabic_font_missing = false;

bool ui_arabic_font_missing() { return g_arabic_font_missing; }
void set_ui_arabic_font_missing(bool on) { g_arabic_font_missing = on; }

UiInitResult ui_init(void* hwnd, bool persist_layout) {
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
    // Human session: remember the dock layout. Scripted run: deterministic
    // default every time, so automation screenshots are comparable.
    io.IniFilename = persist_layout ? layout_ini_path() : nullptr;

    // System UI font first, embedded default as fallback.
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", kUiFontSize);
    if (font != nullptr) {
        out.font_used = "Segoe UI";
    } else {
        io.Fonts->AddFontDefault();
        out.font_used = "ImGui default";
    }
    // Arabic companion font (Phase 15): merged into the same atlas so Latin
    // and shaped Arabic (presentation forms, see NF/UI/ArabicShaper) render
    // in one draw.
    //
    // The base font is loaded with NO glyph ranges, which means ImGui
    // rasterises Basic Latin only — so Arabic codepoints are absent from the
    // atlas whether or not the base FONT FILE happens to contain them. Segoe UI
    // does contain Arabic, but it is never asked for. That makes this merge the
    // ONLY source of Arabic glyphs, and its failure mode is the entire UI
    // rendering as tofu diamonds.
    //
    // It used to fail silently, with a comment arguing that silence is correct
    // because "no Arabic font" is a legal state for an English session. It is
    // legal; it is not a reason to be mute. An English editor is unaffected
    // either way, and an Arabic editor now gets a named console error AND an
    // on-screen banner instead of a screen full of diamonds and no clue.
    if (ImFont* arabic = load_arabic_font(io)) {
        (void)arabic;
        out.font_used += " + Amiri (AR)";
        out.arabic_font_ok = true;
    } else {
        out.arabic_font_ok = false;
        out.arabic_font_searched.clear();
        NF_LOG_ERROR(
            LogCategory::Editor,
            "Arabic UI font missing: Resources/fonts/Amiri-Regular.ttf not found (searched the "
            "build font dir, the executable directory, and the working directory, each walked up "
            "6 levels). Every Arabic label will render as tofu. Copy Resources/fonts/ next to "
            "the executable.");
    }
    // The font atlas texture itself is uploaded by UiRenderer::init() (GPU
    // path, waited). Building here as well satisfies NewFrame()'s TexIsBuilt
    // check even if a frame is recorded before the renderer binds the font.
    io.Fonts->Build();

    // Modern forge-dark theme: flat, calm, one accent. Rounded frames keep
    // the dense editor readable; slightly larger padding stops toolbar and
    // dock buttons from sticking together (a real report).
    apply_forge_theme();

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
