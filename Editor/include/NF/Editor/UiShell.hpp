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
    // Viewport pointer gesture (left button): press hit-tests, selects and
    // arms a gizmo drag; drag moves the selection live; release folds the
    // whole gesture into one undoable command (a click without movement
    // folds into nothing, so plain selection never touches undo).
    // Panel NDC in all three. `press_additive` carries Shift (group select:
    // the hit entity toggles into the selection instead of replacing it).
    bool viewport_press = false;
    bool viewport_press_additive = false;
    float press_ndc_x = 0.0f;
    float press_ndc_y = 0.0f;
    bool viewport_drag = false;
    float drag_ndc_x = 0.0f;
    float drag_ndc_y = 0.0f;
    bool viewport_release = false;
    // Viewport navigation (right button held on the viewport image): orbit
    // look + WASD/QE fly + wheel zoom. Pixel deltas accumulate while held;
    // main.cpp drains them once per frame, so a slow frame never drops a
    // gesture the way per-event consumption would.
    bool nav_orbit = false;
    float nav_dx = 0.0f; // +x = pointer moved right, pixels
    float nav_dy = 0.0f; // +y = pointer moved down, pixels
    float nav_wheel = 0.0f; // + = wheel up (zoom in), notches
    bool nav_f = false; // W: dolly toward the pivot
    bool nav_b = false; // S: dolly away
    bool nav_l = false; // A: orbit left
    bool nav_r = false; // D: orbit right
    bool nav_u = false; // E: rise
    bool nav_d = false; // Q: sink

    // --- Project actions -----------------------------------------------------
    // Scaffolding and building happen in main.cpp, not here: this layer only
    // records what the user asked for, so the panels stay free of filesystem and
    // process work.
    bool new_project_confirm = false;
    std::string new_project_dir;
    std::string new_project_name;
    bool build_project = false;
};

struct UiFrameStats {
    bool validation_on = false;
    uint32_t validation_errors = 0;
    uint32_t alive_objects = 0;
    uint32_t viewport_lit = 0; // last readback proof (0 = not measured yet)
    // Frame delta in seconds. Carried here rather than read from ImGui so the
    // headless path drives the same autosave clock as the windowed one.
    float dt_seconds = 0.0f;
    // Profiler panel (P4). The RHI has no timestamp queries in v0.1, so the
    // GPU figure is the wall clock from submit of the viewport command buffer
    // to its fence signalling — a real measurement of GPU work, coarse but
    // honest, and it is the number a stalled frame inflates. cull_us and
    // draw_prep_us are the renderer's own CPU timings; draw_calls/visible are
    // its counts. scene_open_us is the most recent scene-or-asset load the
    // editor timed (the dominant load cost in v0.1), assets_cached how many
    // mesh handles the manager currently holds.
    uint64_t gpu_us = 0;
    uint64_t gpu_avg_us = 0;
    double cull_us = 0.0;
    double draw_prep_us = 0.0;
    uint32_t draw_calls = 0;
    uint32_t visible_objects = 0;
    uint64_t scene_open_us = 0;
    size_t assets_cached = 0;
};

UiIntents ui_frame(EditorApp& app, const UiFrameStats& stats);

// Ends the frame (ImGui::Render). The caller submits GetDrawData() through
// UiRenderer inside its UI Load pass.
void ui_end_frame();

// Destroys the ImGui context (after UiRenderer::shutdown(), while the device
// still lives — the font atlas upload is long done, but order stays obvious).
void ui_shutdown();

} // namespace nf::editor
