// G9 editor finishing: the cold-start contract of the toolbar/settings area.
//
// Toolbar.cpp compiles into the editor APPLICATION target (not NFEditorCore),
// so the widget layout itself is not unit-testable — the headless editor
// automation and the lead's cold-start screenshot own that. What IS testable
// is the inline data this area serves: the sky preset table (ToolbarUi.hpp)
// and the EditorUiSettings session defaults. Both are pinned here.
//
// The load-bearing pin is preset 0 == the renderer's cold-start sky. A scene
// without a Sky entity keeps the renderer's default sky (rendering::SkyParams{}
// — Runtime::extract_sky only overrides when a component exists), so first
// launch and the "Clear day" preset must paint the SAME look, or picking the
// preset on a fresh scene visibly jumps.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Rendering/Sky.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>

using namespace nf;

// --- the default sky ----------------------------------------------------------

NF_TEST(editor_default_sky_preset_matches_the_cold_start_sky) {
    // What the cold-start viewport shows when the scene has no Sky entity.
    const rendering::SkyParams cold_start{};
    const runtime::SkyComponent day = editor::sky_preset_values(0);

    NF_CHECK(day.zenith_r == cold_start.zenith.x);
    NF_CHECK(day.zenith_g == cold_start.zenith.y);
    NF_CHECK(day.zenith_b == cold_start.zenith.z);
    NF_CHECK(day.horizon_r == cold_start.horizon.x);
    NF_CHECK(day.horizon_g == cold_start.horizon.y);
    NF_CHECK(day.horizon_b == cold_start.horizon.z);
    NF_CHECK(day.ground_r == cold_start.ground.x);
    NF_CHECK(day.ground_g == cold_start.ground.y);
    NF_CHECK(day.ground_b == cold_start.ground.z);
    NF_CHECK(day.sun_disk == cold_start.sun_disk);
    NF_CHECK(day.sun_glow == cold_start.sun_glow);
    NF_CHECK(day.enabled == cold_start.enabled);

    // The edit the toolbar hands to EditorApp::apply_sky_edit carries the same
    // palette plus enabled=true, so applying "Clear day" cannot dim the sky.
    const editor::SkyEdit edit = editor::sky_preset_edit(0);
    NF_CHECK(edit.enabled);
    NF_CHECK(edit.zenith[0] == day.zenith_r && edit.zenith[1] == day.zenith_g &&
             edit.zenith[2] == day.zenith_b);
    NF_CHECK(edit.horizon[0] == day.horizon_r && edit.horizon[1] == day.horizon_g &&
             edit.horizon[2] == day.horizon_b);
    NF_CHECK(edit.ground[0] == day.ground_r && edit.ground[1] == day.ground_g &&
             edit.ground[2] == day.ground_b);
    NF_CHECK(edit.sun_disk == day.sun_disk);
    NF_CHECK(edit.sun_glow == day.sun_glow);
}

NF_TEST(editor_sky_presets_stay_distinguishable) {
    // "Pick night must change something on screen": the three palettes must
    // remain three different skies, and each must read as its name.
    const runtime::SkyComponent day = editor::sky_preset_values(0);
    const runtime::SkyComponent dusk = editor::sky_preset_values(1);
    const runtime::SkyComponent night = editor::sky_preset_values(2);

    NF_CHECK(day.horizon_r != dusk.horizon_r);
    NF_CHECK(dusk.horizon_r != night.horizon_r);
    NF_CHECK(day.horizon_r != night.horizon_r);
    NF_CHECK(dusk.horizon_r > day.horizon_r);   // sunset horizon burns brighter
    NF_CHECK(night.horizon_r < day.horizon_r);  // night is darker...
    NF_CHECK(night.zenith_b > night.zenith_r);  // ...and blue-shifted
    NF_CHECK(night.sun_disk < day.sun_disk);
    NF_CHECK(night.sun_glow < day.sun_glow);

    // Out of range falls back to the clear-day palette, never to garbage.
    const runtime::SkyComponent oob_high = editor::sky_preset_values(7);
    const runtime::SkyComponent oob_low = editor::sky_preset_values(-1);
    NF_CHECK(oob_high.horizon_r == day.horizon_r && oob_high.zenith_b == day.zenith_b);
    NF_CHECK(oob_low.sun_disk == day.sun_disk && oob_low.sun_glow == day.sun_glow);
}

// --- the session defaults ------------------------------------------------------

NF_TEST(editor_ui_settings_defaults_are_cold_start_friendly) {
    // A fresh stack object, NOT ui_settings(): that one is the process-wide
    // session and may have been mutated by another window in this binary.
    const editor::EditorUiSettings st{};

    // E1: the grid is OFF on first launch and stays one click away.
    NF_CHECK(!st.show_grid);
    NF_CHECK(st.grid_step > 0.0f);
    // The viewport reads are on by default; the heavyweight ones are not.
    NF_CHECK(st.show_fps);
    NF_CHECK(st.show_validation);
    NF_CHECK(!st.show_profiler);
    // Snapping off; the day sky is the default preset (matches the cold-start
    // sky — see editor_default_sky_preset_matches_the_cold_start_sky).
    NF_CHECK(!st.snap_enabled);
    NF_CHECK(st.sky_preset == 0);
    // GLB is the default export format: one self-contained file.
    NF_CHECK(st.export_format == 5);
    NF_CHECK(st.export_whole_scene);
    // No dialog opens itself on launch.
    NF_CHECK(!st.show_settings && !st.show_export && !st.show_import && !st.show_about);
}

// --- Settings > Rendering: which entity the section edits ---------------------
//
// The render preferences the lead's list names (shadow strength/bias/cascades)
// are COMPONENT state, not renderer state: they live on the scene's directional
// light, and the renderer draws the FIRST one (the same "the renderer takes the
// first" rule apply_sky_edit relies on for the sky). If the lookup ever picked a
// different entity the sliders would edit something nothing renders — silently,
// because the widget would still move. That is what these two pin.

NF_TEST(editor_settings_render_edits_the_first_light_and_sky) {
    scene::Scene scene("SettingsRender");
    // Deliberately TWO of each: "the first" is only meaningful with a second.
    const ecs::Entity light_a = scene.world().create_entity();
    scene.world().add<runtime::DirectionalLight>(light_a, runtime::DirectionalLight{});
    const ecs::Entity light_b = scene.world().create_entity();
    scene.world().add<runtime::DirectionalLight>(light_b, runtime::DirectionalLight{});
    const ecs::Entity sky_a = scene.world().create_entity();
    scene.world().add<runtime::SkyComponent>(sky_a, runtime::SkyComponent{});
    const ecs::Entity sky_b = scene.world().create_entity();
    scene.world().add<runtime::SkyComponent>(sky_b, runtime::SkyComponent{});

    NF_CHECK(editor::scene_light_entity(scene.world()) == light_a);
    NF_CHECK(editor::scene_sky_entity(scene.world()) == sky_a);
}

NF_TEST(editor_settings_render_says_so_when_the_scene_has_nothing_to_edit) {
    // An empty scene has no light and no sky. Both lookups must report that
    // rather than returning a stale or fabricated entity: the section prints
    // "no light in this scene" and shows no sliders, which is the honest state.
    scene::Scene scene("SettingsRenderEmpty");
    NF_CHECK(!editor::scene_light_entity(scene.world()).valid());
    NF_CHECK(!editor::scene_sky_entity(scene.world()).valid());

    // A camera is not a light: the lookup keys on the component, not on
    // "something is selected / something exists".
    const ecs::Entity cam = scene.world().create_entity();
    scene.world().add<runtime::CameraComponent>(cam, runtime::CameraComponent{});
    NF_CHECK(!editor::scene_light_entity(scene.world()).valid());
}

NF_TEST(editor_settings_render_shows_the_scenes_real_shadow_values) {
    // The section's sliders start from read_light on the entity it edits, so the
    // values the user sees are the values the renderer will use — not the
    // struct's defaults. A scene whose light was authored with non-default
    // shadows must read back those shadows.
    scene::Scene scene("SettingsRenderValues");
    const ecs::Entity light = scene.world().create_entity();
    runtime::DirectionalLight dl{};
    dl.shadow_strength = 0.25f;
    dl.shadow_bias = 0.002f;
    dl.shadow_cascades = 2u;
    dl.shadow_distance = 120.0f;
    scene.world().add<runtime::DirectionalLight>(light, dl);

    bool has = false;
    const editor::LightEdit le = editor::read_light(scene.world(), light, has);
    NF_CHECK(has);
    NF_CHECK_NEAR(le.shadow_strength, 0.25f, 1e-6f);
    NF_CHECK_NEAR(le.shadow_bias, 0.002f, 1e-6f);
    NF_CHECK(le.shadow_cascades == 2);
    NF_CHECK_NEAR(le.shadow_distance, 120.0f, 1e-6f);

    // And the section edits exactly the entity the lookup names.
    NF_CHECK(editor::scene_light_entity(scene.world()) == light);
}
