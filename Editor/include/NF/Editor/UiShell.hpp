#pragma once

// NF/Editor/UiShell.hpp — Dear ImGui shell for the native editor executable.
//
// The pure editor logic lives in NFEditorCore (window-free, fully tested).
// This header exposes the thin ImGui layer used only by NOVAForgeEditor:
// context/docking setup, per-frame panel recording, and input intents.
//
// Rendering note: ImGui frames are recorded every frame (real DockSpace
// layout, real panel widgets incl. a live viewport Image(), real drag & drop
// payloads) and submitted through UiRenderer — an RHI-only draw backend with
// no raw Vulkan outside the RHI. The resulting UI pass Loads the
// already-rendered scene image and draws over it, then presents.

#include <NF/Editor/EditorApp.hpp>

#include <cstdint>
#include <string>

namespace nf::editor {

struct UiInitResult {
    bool context_ok = false;
    bool win32_ok = false; // ImGui Win32 backend initialized for HWND input
    std::string font_used;
};

// Creates the ImGui context, enables docking, applies the graphite style and
// loads Segoe UI (falling back to the default font). Safe to call once.
UiInitResult ui_init(void* hwnd);

// Forwards one raw Win32 message to ImGui_ImplWin32_WndProcHandler. Always
// returns false (that handler claims nothing), so the engine's input system
// and DefWindowProc keep working untouched. Install as Window message hook.
bool ui_handle_win32_message(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam);

// Records one full ImGui frame: dockspace + toolbar/outliner/viewport/
// inspector/assets/console panels. Reads EditorApp, never mutates the scene
// except through explicit widget-triggered intents returned below.
struct UiIntents {
    bool open_scene_dialog_confirm = false;
    std::string open_scene_path;
    bool save_as_confirm = false;
    std::string save_as_path;
    // Mesh asset dropped onto the viewport panel (logical path).
    bool viewport_drop = false;
    std::string dropped_mesh_path;
    // Viewport pick request in panel NDC.
    bool viewport_pick = false;
    float pick_ndc_x = 0.0f;
    float pick_ndc_y = 0.0f;
};

struct UiFrameStats {
    bool validation_on = false;
    uint32_t validation_errors = 0;
    uint32_t alive_objects = 0;
    uint32_t viewport_lit = 0; // last readback proof (0 = not measured yet)
};

UiIntents ui_frame(EditorApp& app, const UiFrameStats& stats);

// Ends the frame (ImGui::Render). The caller submits GetDrawData() through
// UiRenderer inside its UI Load pass.
void ui_end_frame();

// Destroys the ImGui context (after UiRenderer::shutdown(), while the device
// still lives — the font atlas upload is long done, but order stays obvious).
void ui_shutdown();

} // namespace nf::editor
