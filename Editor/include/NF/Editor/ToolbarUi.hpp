#pragma once

// NF/Editor/ToolbarUi.hpp — the editor's top bar: menus, icon toolbar row,
// settings window and the import/export dialogs.
//
// Lives outside Panels.cpp so that file stays "the docked panels" and this one
// stays "everything the user clicks before a panel exists". Share of state:
// EditorUiSettings is a process-wide session object (one editor, one set of
// preferences), and the toolbar reads EditorApp like any panel does — it never
// touches the scene except through EditorApp.
//
// Dialogs that need a real file picker (Import model, Export mesh, Open, Save
// As) call the Win32 common dialogs directly: the shell is already a Windows
// program, and a path text box is not a substitute for "show me my files".

#include <NF/ECS/ECS.hpp>
#include <NF/Editor/Inspector.hpp>
#include <NF/Editor/UiShell.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>

#include <vector>

namespace nf::editor {

/// Session preferences. Not persisted yet: the editor writes no settings file
/// (imgui.ini is suppressed for deterministic layout), so a restart starts from
/// these defaults — which is also why they are chosen to be good defaults.
/// Profiler window visibility. Shared by the toolbar (the View toggle) and the
/// panels (the window itself), so it lives on the session settings object rather
/// than in one translation unit's anonymous namespace.
struct EditorUiSettings {
    // Viewport
    // E1: OFF by default. The lead's report was "white grid lines hurt the
    // eyes" — a full-brightness grid over the whole floor is louder than the
    // geometry it is supposed to be a reference for. It stays one click away in
    // View > Grid and in Settings, and is drawn subtly when switched on
    // (see draw_ground_grid).
    bool show_grid = false;
    float grid_step = 1.0f;
    bool show_fps = true;
    bool show_validation = true;
    bool show_profiler = false;
    // Snapping (applied to the gizmo when Snap is on)
    bool snap_enabled = false;
    float snap_move = 0.5f;
    float snap_rotate = 15.0f;
    float snap_scale = 0.25f;
    // Accent colour (the one accent the whole UI uses)
    float accent_r = 0.298f;
    float accent_g = 0.553f;
    float accent_b = 1.000f;
    // Export dialog memory
    int export_format = 5;        // MeshFormat::Glb — one self-contained file
    bool export_whole_scene = true;
    // Sky preset the Create > Sky and Settings buttons write: 0 clear day,
    // 1 golden hour, 2 night. Applied to the scene's Sky component.
    int sky_preset = 0;
    // Window visibility
    bool show_settings = false;
    bool show_export = false;
    bool show_import = false;
    bool show_about = false;
};

EditorUiSettings& ui_settings();

/// Applies the accent colour to the ImGui style (called when it changes).
void apply_ui_accent(const EditorUiSettings& settings);

/// The sky palette behind preset 0 (clear day) / 1 (golden hour) / 2 (night),
/// as a SkyComponent. These are the same palettes rendering::TimeOfDay blends
/// through, so a preset picked in the UI and a timelapse agree about what the
/// names mean. An out-of-range preset yields the clear-day palette.
///
/// Inline in the header on purpose: the table is pure data, and Toolbar.cpp
/// belongs to the editor *application* target while NFEditorCore (which the
/// tests link) does not compile it — a .cpp definition here would be invisible
/// to the suite.
inline runtime::SkyComponent sky_preset_values(int preset) {
    // Preset 0 (clear day) is the palette the renderer itself paints on cold
    // start: a scene without a Sky entity keeps the renderer's default sky
    // (rendering::SkyParams{} — Runtime::extract_sky only overrides when a
    // component exists), and first launch must not jump to a different look
    // the moment the user picks "Clear day" or adds a sky. The values below
    // mirror SkyParams{} field for field, so preset 0, Create > Sky presets
    // and the cold-start viewport all agree. Tests pin this equality
    // (test_editor_finishing.cpp) and RHITests' test_sky.cpp pins the look.
    runtime::SkyComponent sky;
    if (preset == 1) { // golden hour
        sky.zenith_r = 0.28f; sky.zenith_g = 0.18f; sky.zenith_b = 0.42f;
        sky.horizon_r = 0.98f; sky.horizon_g = 0.45f; sky.horizon_b = 0.22f;
        sky.ground_r = 0.10f; sky.ground_g = 0.08f; sky.ground_b = 0.12f;
        sky.sun_disk = 1.2f;
        sky.sun_glow = 1.6f;
    } else if (preset == 2) { // night
        sky.zenith_r = 0.015f; sky.zenith_g = 0.03f; sky.zenith_b = 0.08f;
        sky.horizon_r = 0.05f; sky.horizon_g = 0.08f; sky.horizon_b = 0.15f;
        sky.ground_r = 0.01f; sky.ground_g = 0.01f; sky.ground_b = 0.02f;
        sky.sun_disk = 0.4f;
        sky.sun_glow = 0.4f;
    } else { // clear day = the cold-start default sky (rendering::SkyParams{})
        sky.zenith_r = 0.055f; sky.zenith_g = 0.195f; sky.zenith_b = 0.600f;
        sky.horizon_r = 0.550f; sky.horizon_g = 0.660f; sky.horizon_b = 0.800f;
        sky.ground_r = 0.135f; sky.ground_g = 0.125f; sky.ground_b = 0.110f;
        sky.sun_disk = 1.0f;
        sky.sun_glow = 1.0f;
    }
    return sky;
}

/// Exactly what the toolbar hands to EditorApp::apply_sky_edit for a preset, so
/// a test can drive the production path rather than a re-implementation of it.
inline SkyEdit sky_preset_edit(int preset) {
    const runtime::SkyComponent sky = sky_preset_values(preset);
    SkyEdit edit;
    edit.enabled = true;
    edit.zenith[0] = sky.zenith_r; edit.zenith[1] = sky.zenith_g; edit.zenith[2] = sky.zenith_b;
    edit.horizon[0] = sky.horizon_r; edit.horizon[1] = sky.horizon_g; edit.horizon[2] = sky.horizon_b;
    edit.ground[0] = sky.ground_r; edit.ground[1] = sky.ground_g; edit.ground[2] = sky.ground_b;
    edit.sun_disk = sky.sun_disk;
    edit.sun_glow = sky.sun_glow;
    return edit;
}

/// Records the main menu bar, the icon toolbar row, the settings window and the
/// import/export/about dialogs. Fills the same UiIntents the panels do (Open and
/// Save As still go through the shell's VFS-aware handlers).
void toolbar_ui(EditorApp& app, const UiFrameStats& stats, UiIntents& intents);

// --- the entities the Settings window's Rendering section edits ---------------
//
// Render preferences are component state, not renderer state: the light's
// shadow parameters live on runtime::DirectionalLight and the sky's palette on
// runtime::SkyComponent, and the renderer consumes the FIRST of each (the same
// "one per scene, the renderer takes the first" rule apply_sky_edit relies on
// for the sky). A Settings window that picked any other entity would edit
// something nothing draws, so the selection rule is shared here and pinned by
// a test rather than re-derived in the paint code.
//
// Both return kInvalidEntity for a scene without that component; the window
// reports that instead of silently editing nothing. Header-inline for the usual
// reason: Toolbar.cpp belongs to the editor *application* target, which
// NFEditorCore — and therefore EditorTests — does not compile.

/// The scene's directional light, or kInvalidEntity when it has none.
inline ecs::Entity scene_light_entity(const ecs::World& world) {
    const std::vector<ecs::Entity> lights = world.query<runtime::DirectionalLight>();
    return lights.empty() ? ecs::kInvalidEntity : lights.front();
}

/// The scene's sky, or kInvalidEntity when it has none.
inline ecs::Entity scene_sky_entity(const ecs::World& world) {
    const std::vector<ecs::Entity> skies = world.query<runtime::SkyComponent>();
    return skies.empty() ? ecs::kInvalidEntity : skies.front();
}

} // namespace nf::editor
