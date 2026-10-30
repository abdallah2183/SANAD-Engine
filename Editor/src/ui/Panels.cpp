// Panels.cpp — ImGui panel recording: toolbar, outliner, viewport, inspector,
// assets, console. All scene mutations go through EditorApp (commands +
// validation); widgets only collect input and forward intents.

#define _CRT_SECURE_NO_WARNINGS
#include <NF/Editor/UiShell.hpp>
#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Editor/ProjectLauncher.hpp> // start_here choice persisted with the shell settings

#include <NF/Assets/AssetManager.hpp>
#include <NF/Editor/AudioPreview.hpp>
#include <NF/Editor/FileSystemPanel.hpp>
#include <NF/Editor/ReflectedInspector.hpp>
#include <NF/Editor/UiRenderer.hpp>
#include <NF/Core/Profiler.hpp>
#include <NF/Core/FileSystem.hpp>
#include <NF/Rendering/DynamicResolution.hpp>
#include <NF/Rendering/RenderScale.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Vfx/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>
#include <NF/Editor/UiText.hpp> // AV()/AVF()/rt_* — the single display-string path
#include <NF/Editor/UiTheme.hpp> // semantic colours (danger/success/accent)
#include <NF/Editor/ScenePanels.hpp> // the dedicated per-function editors
#include <NF/Editor/TransformGizmoView.hpp> // in-viewport arrows/rings/boxes

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: default layout only (first frame)
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <vector>

namespace nf::editor {

namespace {

// Persistent per-widget edit state (reset when the inspected entity changes).
struct InspectorCache {
    ecs::Entity entity;
    bool valid = false;
    char name[129]{};
    float pos[3]{};
    float rot[3]{};
    float scl[3]{};
    bool cam_active = false;
    float cam[3]{}; // fov, near, far
    // Weapon and hit points (Phase 44). The PROFILE is cached, not the
    // component: the live state is rebuilt by the command, so the panel never
    // edits a magazine that is half empty.
    bool weapon_has = false;
    runtime::WeaponProfile weapon{};
    bool health_has = false;
    runtime::HealthComponent health{};
    // Combat agent and affiliation (Phase 46). The agent's aim state is live,
    // so only the authored configuration is cached; the target is a text field
    // because it names another entity.
    bool agent_has = false;
    runtime::CombatAgentComponent agent{};
    char agent_target[129]{};
    bool agent_auto_target = false;
    bool faction_has = false;
    runtime::FactionComponent faction{};
    char faction_team[65]{};
    bool faction_friendly_fire = false;
    bool faction_enabled = true;
    float light_dir[3]{};
    float light_color[3]{};
    float light_intensity = 1.0f;
    bool light_shadows = true;
    float light_shadow_strength = 1.0f;
    float light_shadow_bias = 0.0005f;
    int light_shadow_cascades = 4;
    float light_shadow_distance = 0.0f;
    // Sky / environment (Phase 13)
    bool sky_has = false;
    bool sky_enabled = true;
    // Day/night cycle. `tod_has` stays false until the component is added, which
    // is what the panel keys its "add" affordance off.
    bool tod_has = false;
    bool tod_enabled = true;
    bool tod_drive_light = true;
    float tod_hours = 12.0f;
    float tod_day_length = 240.0f;
    float sky_zenith[3]{0.20f, 0.42f, 0.85f};
    float sky_horizon[3]{0.62f, 0.72f, 0.82f};
    float sky_ground[3]{0.09f, 0.09f, 0.11f};
    float sky_sun_disk = 1.0f;
    float sky_sun_glow = 1.0f;
    // Post-processing stack (§206). Same "has" pattern as the sky above: the
    // panel keys its "add" affordance off pp_has, because the component is
    // absent until the author asks for it and an absent component is what keeps
    // a scene's `PostProcess:` line out of the saved file.
    bool pp_has = false;
    bool pp_bloom_enabled = false;
    float pp_bloom_threshold = 1.0f;
    float pp_bloom_knee = 0.5f;
    float pp_bloom_intensity = 1.0f;
    float pp_bloom_radius = 1.0f;
    bool pp_grade_enabled = false;
    float pp_grade_contrast = 1.0f;
    float pp_grade_pivot = 1.0f;
    float pp_grade_temperature = 0.0f;
    float pp_grade_tint = 0.0f;
    float pp_grade_gamma = 1.0f;
    bool pp_sharpen_enabled = false;
    float pp_sharpen_amount = 0.0f;
    float pp_sharpen_radius = 1.0f;
    float pp_saturation = 1.0f;
    float pp_vignette = 0.0f;
    bool pp_lens_enabled = false;
    float pp_lens_distortion = 0.0f;
    float pp_lens_chroma = 0.0f;
    bool pp_dof_enabled = false;
    float pp_dof_focus = 10.0f;
    float pp_dof_range = 2.0f;
    float pp_dof_radius = 6.0f;
    bool pp_motion_enabled = false;
    float pp_motion_intensity = 1.0f;
    float pp_motion_length = 0.05f;
    // Projected decal (Phase 36). Placement and facing are owned by the entity
    // Transform, so the cache holds only the values this panel edits; the
    // "has" flag keys the add affordance, exactly like the post block above.
    bool decal_has = false;
    char decal_material[256]{};
    float decal_size[3]{1.0f, 1.0f, 1.0f};
    float decal_opacity = 1.0f;
    bool decal_enabled = true;
    // Anti-aliasing. Its own pass on the tonemapped image, so it is cached
    // beside the post block rather than inside it.
    bool pp_fxaa_enabled = false;
    float pp_fxaa_threshold = 0.125f;
    float pp_fxaa_threshold_min = 0.0312f;
    // Internal render scale (Phase 37): one value, not a stage toggle. 1.0 is
    // the identity, above it supersamples, below it renders cheaper.
    float pp_render_scale = 1.0f;
    // Dynamic resolution (Phase 38). The static scale above is the ceiling;
    // these control how the governor walks between it and the floor.
    bool pp_dynamic_resolution = false;
    float pp_target_fps = 60.0f;
    float pp_min_render_scale = 0.5f;
    // Phase 40: 0 = CPU frame time, 1 = real GPU timestamps with CPU fallback.
    int pp_dynamic_resolution_timing = runtime::kDynamicResolutionTimingCpu;
    // Phase 42: decide the scale from the frame WINDOW instead of the
    // per-frame governor. Only meaningful while dynamic resolution is on.
    bool pp_frame_policy = false;
    // Opt-in: 0 exposure and -1 operator mean "the renderer's own", so a scene
    // that never authored them keeps whatever the renderer already had.
    float pp_exposure = 0.0f;
    int pp_tonemap = -1;
    char pp_lut_path[256]{};
    float pp_lut_strength = 0.0f;
    bool pp_fog_enabled = false;
    float pp_fog_color[3]{0.62f, 0.66f, 0.70f};
    float pp_fog_start = 60.0f;
    float pp_fog_end = 120.0f;
    // Scene audio environment (reverb zone / music / ambience bed). Same "has"
    // pattern as the sky and post-processing above: the component is absent
    // until the author asks for it, and an absent component is what keeps the
    // scene's line out of the saved file.
    //
    // The reverb zone is cached as the component itself (six floats and a bool,
    // so a mirror struct would be pure translation). Music and ambience cache
    // only the fields the widget shows — their components own decoded samples,
    // and copying megabytes into a per-frame cache is the wrong trade.
    bool rz_has = false;
    audio::ReverbZoneComponent rz_edit{};
    bool mus_has = false;
    char mus_buffer[256]{};
    float mus_volume = 1.0f;
    float mus_fade = 0.0f;
    bool mus_enabled = true;
    bool amb_has = false;
    char amb_buffer[256]{};
    float amb_fade = 0.0f;
    bool amb_enabled = true;
    char mesh_id[64]{};
    char mesh_mat[192]{};
    char mat_path[256]{};
    float mat_base[4]{0.8f, 0.8f, 0.8f, 1.0f};
    float mat_metal = 0.0f;
    float mat_rough = 0.4f;
    float mat_ao = 1.0f;
    float mat_em[3]{};
    float mat_estr = 0.0f;
    int mat_choice = 0;
    int albedo_choice = 0;
    int mip_choice = 2; // 0=None, 1=Nearest, 2=Linear (mirrors rhi::MipMapMode)
    bool mat_live = false; // a slider drag is writing previews, uncommitted
    rendering::PBRMaterialParams mat_before{}; // snapshot at drag start (undo target)
    // Physics: RigidBody
    int rb_type = 1;       // 0=Static, 1=Dynamic, 2=Kinematic
    float rb_mass = 1.0f;
    float rb_friction = 0.5f;
    float rb_restitution = 0.1f;
    float rb_lin_damp = 0.05f;
    float rb_ang_damp = 0.05f;
    bool rb_allow_sleep = true;
    // Physics: Collider
    int col_shape = 0;     // 0=Sphere, 1=Box, 2=Plane
    float col_radius = 0.5f;
    float col_half[3]{0.5f, 0.5f, 0.5f};
    float col_normal[3]{0.0f, 1.0f, 0.0f};
    // Destructible (Phase 19). The seed is a u32 the fracture planes are
    // derived from, so it is edited as one rather than through a float slider
    // (a float would round-trip through a value that is not the seed).
    int dst_chunks = 4;
    unsigned dst_seed = 0x5EEDBEEFu;
    float dst_strength = 25.0f;
    float dst_threshold = 8.0f;
    float dst_blast = 2.0f;
    bool dst_enabled = true;
    // Combat feedback (Phase 59). The component was authorable only through the
    // `Feedback:` scene line until this panel existed. Same "has" pattern as the
    // destructible block above: absent until the author attaches one, and an
    // absent component is what keeps the line out of the saved file.
    bool fb_has = false;
    bool fb_enabled = true;
    bool fb_audio = true;
    bool fb_light = true;
    float fb_flash_time = 0.055f;
    float fb_flash_size = 0.22f;
    float fb_blast_time = 0.16f;
    float fb_blast_size = 0.9f;
    float fb_tracer_time = 0.09f;
    float fb_tracer_length = 9.0f;
    float fb_tracer_width = 0.025f;
    float fb_impact_time = 6.0f;
    float fb_impact_size = 0.14f;
    float fb_shot_volume = 0.7f;
    float fb_impact_volume = 0.45f;
    float fb_impact_volume_explosive = 0.75f;
    float fb_impact_volume_fire = 0.55f;
    float fb_light_intensity = 3.0f;
    float fb_light_radius = 7.0f;
    float fb_light_color[3]{1.0f, 0.72f, 0.35f};
    int fb_max_flashes = 48;
    int fb_max_tracers = 96;
    int fb_max_impacts = 192;
    int fb_max_sounds = 16;
    int fb_max_lights = 4;
    // Perception (Phase 60). Same "has" pattern as the two blocks above: the
    // component is absent until the author attaches one, and the absent
    // component is what keeps the `Perception:` line out of the saved file.
    bool perc_has = false;
    bool perc_enabled = true;
    bool perc_pain = true;
    bool perc_governs = true;
    float perc_sight = 30.0f;
    float perc_fov = 60.0f;
    float perc_gain = 2.0f;
    float perc_loss = 0.6f;
    float perc_linger = 3.0f;
    float perc_hearing = 18.0f;
    float perc_touch = 1.5f;
    float perc_noise = 0.0f;
    float perc_visible = 1.0f;
    float perc_radius = 0.5f;
    float perc_eye = 1.5f;
    // Squad radio (Phase 68). The token buffer matches the loader's 64-character
    // cap plus the terminator; the radio is the authored link range.
    bool squad_has = false;
    bool squad_enabled = true;
    float squad_radio = 30.0f;
    float squad_lost = 0.0f;
    // Phase 71: horizontal antenna half-angle; 0 is omnidirectional.
    float squad_cone = 0.0f;
    char squad_token[65]{};
    // Radio jammer (Phase 70). The field radius and the fraction of an
    // antenna's reach it removes inside that radius.
    bool jammer_has = false;
    bool jammer_enabled = true;
    float jammer_radius = 12.0f;
    float jammer_strength = 1.0f;
    // Decision layer (Phase 61). The action list is cached as the components'
    // own structs: it is a variable-length list of seven-field records, and
    // mirroring it into parallel arrays would be seven places to keep in step.
    bool brain_has = false;
    bool brain_enabled = true;
    float brain_rate = 0.25f;
    std::vector<runtime::BrainAction> brain_actions;
    // State machines (Phase 62), cached on the same terms: the states and the
    // edges are the components' own structs.
    bool sm_has = false;
    bool sm_enabled = true;
    float sm_rate = 0.0f;
    std::vector<runtime::StateSpec> sm_states;
    std::vector<runtime::TransitionSpec> sm_transitions;
    // Animation
    int anim_clip = 0;     // index into the component's clip table
    float anim_speed = 1.0f;
    int anim_loop = 1;     // 0=None, 1=Loop, 2=PingPong
    bool anim_paused = false;
    bool anim_state_machine = false;
    std::vector<std::string> anim_clip_names;
    // Audio
    float aud_volume = 1.0f;
    float aud_pitch = 1.0f;
    bool aud_looping = false;
    bool aud_spatial = false;
    bool aud_autoplay = false;
    float aud_min_dist = 1.0f;
    float aud_max_dist = 50.0f;
    char aud_buffer[256]{};
    // Sky clear color (Phase 25; the file always carried it, the panel did not)
    float sky_clear[3]{0.03f, 0.03f, 0.07f};
    // Lua script (Phase 24). The path is the persisted field; enabled is the
    // only other persisted field. The source lives in the file, so the cache
    // carries no source copy — the panel reads size/line counts live.
    char script_path[256]{};
    bool script_enabled = true;
    // Particles / cloth / character (Phase 25). Mirrors of the scene
    // components; Apply forwards the whole struct through EditorApp.
    bool part_has = false;
    bool part_enabled = true;
    float part_rate = 30.0f;
    float part_lifetime = 1.5f;
    float part_lifetime_spread = 0.5f;
    float part_velocity[3]{0.0f, 3.0f, 0.0f};
    float part_vel_spread[3]{1.0f, 1.0f, 1.0f};
    float part_gravity[3]{0.0f, -9.8f, 0.0f};
    float part_drag = 0.0f;
    float part_start_size = 0.25f;
    float part_end_size = 0.0f;
    float part_start_color[3]{1.0f, 0.9f, 0.7f};
    float part_end_color[3]{1.0f, 0.3f, 0.1f};
    int part_max = 1024;
    bool cloth_has = false;
    bool cloth_enabled = true;
    int cloth_res_x = 12;
    int cloth_res_z = 12;
    float cloth_spacing = 0.25f;
    float cloth_mass = 0.2f;
    float cloth_damping = 0.02f;
    float cloth_stiffness = 0.9f;
    int cloth_iterations = 8;
    int cloth_substeps = 2;
    float cloth_gravity[3]{0.0f, -9.81f, 0.0f};
    bool char_has = false;
    bool char_enabled = true;
    float char_radius = 0.4f;
    float char_max_speed = 6.0f;
    float char_acceleration = 40.0f;
    float char_air_control = 0.35f;
    float char_jump_speed = 7.0f;
    float char_slope = 45.0f;
    float char_mass = 80.0f;
    float char_friction = 0.8f;
    float char_wish[3]{};
    bool char_jump = false;
    // Navigation (Phase 28). The volume is placement plus bake tuning — its
    // Transform origin is the area corner, so no position is mirrored here. The
    // agent is config plus the authored goal; the live path is runtime state
    // and deliberately has no widget at all.
    bool navm_has = false;
    float navm_area[3]{20.0f, 8.0f, 20.0f};
    float navm_cell = 0.5f;
    float navm_cell_height = 0.25f;
    float navm_slope = 45.0f;
    float navm_climb = 0.5f;
    float navm_headroom = 2.0f;
    float navm_min_area = 2.0f;
    float navm_radius = 0.0f;
    float navm_jump_distance = 4.0f;
    float navm_jump_height = 1.5f;
    int navm_max_verts = 6;
    bool navm_enabled = true;
    int navm_link_count = 0;
    // Terrain (Phase 63). The height source is a field, not a mesh, so the
    // section edits the generator: resolution, extent and the noise/heightmap
    // that produce the surface. Heightmap and material are text because they
    // are project paths, and the paths live in the scene file either way.
    bool terr_has = false;
    bool terr_enabled = true;
    int terr_res = 64;
    float terr_area = 100.0f;
    float terr_height = 10.0f;
    float terr_freq = 0.03f;
    int terr_octaves = 4;
    int terr_seed = 1;
    char terr_heightmap[256]{};
    char terr_material[128]{};
    bool nava_has = false;
    float nava_speed = 4.0f;
    float nava_goal[3]{};
    float nava_arrive = 0.25f;
    bool nava_enabled = true;
    // Patrol route (Phase 64). The points are cached one row per destination;
    // the live cursor (which point, how long left waiting) is runtime state and
    // is read for display, never edited.
    bool navr_has = false;
    float navr_wait = 0.0f;
    bool navr_loop = true;
    bool navr_ping_pong = false;
    bool navr_enabled = true;
    int navr_hold = 0; // NavRouteHoldPolicy: 0 off, 1 aware, 2 sensing
    float navr_resume = 0.0f;
    float navr_investigate = 0.0f;
    // Phase 67: a squad token, one word, at most 64 characters. The loader
    // refuses anything longer, so the buffer has one byte of headroom for the
    // terminator and the setter rejects whitespace.
    char navr_squad[65]{};
    std::vector<runtime::NavPointSpec> navr_points;
    // Vehicles (Step 11). Mirrors of the tune; Apply forwards the whole
    // struct through EditorApp. The four *_in fields are the debug-drive
    // inputs (live only, like the character's wish_dir/jump).
    bool veh_has = false;
    bool veh_enabled = true;
    float veh_chassis[3]{0.9f, 0.5f, 2.0f};
    float veh_mass = 1500.0f;
    float veh_wheel_radius = 0.35f;
    float veh_wheel_width = 0.3f;
    float veh_track = 0.9f;
    float veh_wheelbase = 1.3f;
    float veh_wheel_y = -0.35f;
    float veh_steer = 30.0f;
    float veh_torque = 500.0f;
    float veh_susp_min = 0.30f;
    float veh_susp_max = 0.50f;
    float veh_susp_freq = 1.5f;
    float veh_susp_damping = 0.5f;
    float veh_tire_peak = 1.2f;
    float veh_tire_limit = 1.0f;
    float veh_inertia = 0.9f;
    float veh_angular_damping = 0.2f;
    float veh_brake = 1500.0f;
    float veh_handbrake = 4000.0f;
    float veh_throttle_in = 0.0f;
    float veh_steer_in = 0.0f;
    float veh_brake_in = 0.0f;
    float veh_handbrake_in = 0.0f;
    // Phases 115-122
    bool cable_has = false;
    float cable_length = 10.0f;
    int cable_particles = 16;
    float cable_stiffness = 0.95f;
    float cable_damping = 0.03f;
    float cable_density = 0.5f;
    float cable_tension_max = 5000.0f;
    bool cable_breakable = false;
    bool cable_pin_start = true;
    bool cable_pin_end = true;
    float cable_end_offset[3]{0.0f, -5.0f, 5.0f};
    bool cable_enabled = true;
    float cable_live_tension = 0.0f;

    bool weather_has = false;
    int weather_type = 0;
    float w_fog_density = 0.015f;
    float w_fog_falloff = 0.08f;
    float w_fog_base = 0.0f;
    float w_fog_max_opacity = 1.0f;
    float w_fog_color[3]{0.65f, 0.75f, 0.85f};
    float w_sun_inscattering = 0.3f;
    float w_ground_wetness = 0.0f;
    float w_wind_strength = 2.0f;
    float w_wind_dir[3]{1.0f, 0.0f, 0.0f};
    bool weather_enabled = true;

    bool rule_tile_has = false;
    int rt_default_id = 1;
    int rt_preset = 0;
    int rt_count = 16;
    bool rt_auto_update = true;
    bool rt_enabled = true;

    bool flow_has = false;
    int ff_width = 32;
    int ff_height = 32;
    float ff_cell_size = 1.0f;
    float ff_target[2]{16.0f, 16.0f};
    bool ff_enabled = true;

    bool talent_has = false;
    char tt_id[64]{"hero_talents"};
    char tt_name[64]{"Skill Tree"};
    int tt_available = 5;
    int tt_spent = 0;
    int tt_unlocked = 0;
    bool talent_enabled = true;

    bool ragdoll_has = false;
    float rd_weight = 0.0f;
    int rd_pose = 0;
    float rd_duration = 0.5f;
    bool rd_momentum = true;
    bool ragdoll_enabled = true;

    bool inv_has = false;
    int inv_cols = 8;
    int inv_rows = 4;
    float inv_max_weight = 50.0f;
    float inv_cur_weight = 0.0f;
    int inv_item_count = 0;
    bool inv_auto_pack = true;
    bool inv_enabled = true;

    bool sa_has = false;
    int sa_mat = 0;
    float sa_vol = 1.0f;
    float sa_pitch = 0.05f;
    bool sa_wetness = true;
    bool sa_enabled = true;
    std::string error;
};

InspectorCache& inspector_cache() {
    static InspectorCache cache;
    return cache;
}

// Profiler window visibility lives on the shared session settings object
// (NF/Editor/ToolbarUi.hpp) so the toolbar toggle and this window agree.
bool& show_profiler_window() {
    return ui_settings().show_profiler;
}

void push_info(ConsoleBuffer& console, const std::string& what) {
    console.push(LogMessage{LogLevel::Info, LogCategory::Editor, what,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

void push_error(ConsoleBuffer& console, const std::string& what, const std::string& err) {
    console.push(LogMessage{LogLevel::Error, LogCategory::Editor, what + ": " + err,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

// --- Localisation helpers (Phase 15) ----------------------------------------
// AV()/AVF()/rt_* live in NF/Editor/UiText.hpp now (shared with Toolbar.cpp):
// one display-string path for the whole editor, so a fix there fixes both.
// Window/header labels append a stable "###id" so switching languages never
// resets docking or widget state (IDs stay English).
//
// There is no TR() helper here on purpose — see the note in Toolbar.cpp. It was
// defined and never used in this file, and at the call sites where it did get
// used it was wrong (display positions). Logic that needs an unshaped string
// should call ui::tr(key) directly.

// MaterialEdit assembled from the inspector cache (sliders + color widgets).
// Single source for the live path and the explicit Apply button.
MaterialEdit material_edit_from_cache(const InspectorCache& ic) {
    MaterialEdit e;
    for (int i = 0; i < 4; ++i) {
        e.base_color[i] = (i < 3) ? ic.mat_base[i] : 1.0f;
    }
    e.metallic = ic.mat_metal;
    e.roughness = ic.mat_rough;
    e.ao = ic.mat_ao;
    for (int i = 0; i < 3; ++i) {
        e.emission[i] = ic.mat_em[i];
    }
    e.emission_strength = ic.mat_estr;
    return e;
}

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?";
}

ImVec4 level_color(LogLevel l) {
    switch (l) {
        case LogLevel::Warn: return ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
        case LogLevel::Error:
        case LogLevel::Fatal: return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        default: return ImVec4(0.75f, 0.78f, 0.82f, 1.0f);
    }
}

// Short label for the per-line category tag. Keep in sync with
// Logger::LogCategory; None renders as "-" so an uncategorised message still
// gets a visible separator.
const char* category_name(LogCategory c) {
    switch (c) {
        case LogCategory::None: return "-";
        case LogCategory::Core: return "Core";
        case LogCategory::Render: return "Render";
        case LogCategory::Physics: return "Physics";
        case LogCategory::Audio: return "Audio";
        case LogCategory::Network: return "Network";
        case LogCategory::Asset: return "Asset";
        case LogCategory::Editor: return "Editor";
        case LogCategory::Script: return "Script";
        case LogCategory::ECS: return "ECS";
        case LogCategory::Platform: return "Platform";
        case LogCategory::RHI: return "RHI";
        case LogCategory::Jobs: return "Jobs";
        case LogCategory::Scene: return "Scene";
        default: return "?";
    }
}

const char* entity_label_ptr(EditorApp& app, ecs::Entity e) {
    thread_local std::string scratch;
    if (const ecs::World* w = app.world()) {
        scratch = entity_label(*w, e);
    } else {
        scratch = "Entity ?";
    }
    return scratch.c_str();
}

// --- Ground grid -------------------------------------------------------------
//
// The viewport draws the ground grid itself, in screen space, by projecting
// world grid lines through the same view-projection the renderer uses. A grid is
// what every modelling tool gives you for scale and alignment; without it an
// empty scene is a gradient with no sense of size, which is what the viewport
// looked like before.
//
// Projection convention: NDC (-1..1, +Y up) -> screen pixels with row 0 at the
// TOP. That is the engine's convention (Mat4::perspective negates m[1][1] so
// world-up lands in low-numbered framebuffer rows), and it is the same mapping
// viewport_ndc_to_pixel uses for picking — one convention, one place to get it
// wrong.

/// Clips a segment (in NDC) against the [-1, 1] box. Returns false when the
/// whole segment is outside; otherwise writes the clipped endpoints.
bool clip_ndc_segment(float& x0, float& y0, float& x1, float& y1) {
    auto clip = [](float& a, float& b, float lo, float hi) {
        // Liang-Barsky on one axis: parametric t in [0, 1].
        const float d = b - a;
        if (d == 0.0f) {
            return (a >= lo && a <= hi);
        }
        float t0 = 0.0f;
        float t1 = 1.0f;
        const float inv = 1.0f / d;
        const float ta = (lo - a) * inv;
        const float tb = (hi - a) * inv;
        const float lo_t = std::min(ta, tb);
        const float hi_t = std::max(ta, tb);
        t0 = std::max(t0, lo_t);
        t1 = std::min(t1, hi_t);
        if (t0 > t1) {
            return false;
        }
        a = a + d * t0;
        b = a + d * (t1 - t0);
        return true;
    };
    return clip(x0, x1, -1.0f, 1.0f) && clip(y0, y1, -1.0f, 1.0f);
}

/// Projects one world point into viewport pixels. False when the point is
/// behind the camera (w <= 0), where a projection would fold inside out.
bool project_to_viewport(const Mat4& view_proj, float wx, float wy, float wz, const ImVec2& min,
                         const ImVec2& max, ImVec2& out) {
    const Vec4 clip = view_proj * Vec4{wx, wy, wz, 1.0f};
    if (clip.w <= 1e-5f) {
        return false;
    }
    // NDC -> px is LINEAR here, with NO y flip, and that is the point.
    //
    // The NDC that comes out of the camera's view_projection is y-DOWN:
    // Mat4::perspective negates m[1][1] for Vulkan, so world +Y lands at
    // NDC y < 0, which is memory row 0, and memory row 0 IS the top of the
    // image ImGui::Image presents. So render-NDC -> pixel row is plain
    // `ny * 0.5 + 0.5`.
    //
    // The `0.5 - ny * 0.5` this used to apply is the bridge for the OTHER
    // direction: it converts RENDER NDC into POINTER NDC, which is y-UP
    // (Panels' to_ndc maps the image top to +1, and viewport_ndc_to_pixel
    // maps +1 back to row 0). Applying it here drew the ground grid
    // vertically mirrored against the render underneath it.
    const float ndc_x = clip.x / clip.w;
    const float ndc_y = clip.y / clip.w;
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    out.x = min.x + (ndc_x * 0.5f + 0.5f) * w;
    out.y = min.y + (ndc_y * 0.5f + 0.5f) * h;
    return true;
}

/// Draws the y = 0 grid, `half_extent` cells either side of `center`, one line
/// every `spacing`. The world axis lines (x = 0 and z = 0) are drawn in their
/// axis colours so the origin stays unambiguous.
///
/// `center` is the VIEW PIVOT in x/z, not the origin. With the pivot hardcoded
/// to (0,0) the grid was a fixed 50-cell patch around the world origin, so the
/// moment a level was framed anywhere else the viewport lost its floor
/// reference entirely — the exact situation framing exists to create.
constexpr int kGridHalfExtent = 50; // cells either side; extent = 50 * grid_step
void draw_ground_grid(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const Mat4& view_proj,
                      float spacing, int half_extent, float center_x, float center_z) {
    if (spacing <= 0.0f || half_extent <= 0) {
        return;
    }
    const float extent = static_cast<float>(half_extent) * spacing;
    // E1: "white grid lines hurt the eyes". The grid is off by default (see
    // EditorUiSettings::show_grid) and, when switched on, sits well below the
    // geometry instead of competing with it — alpha 38 was the reported problem.
    const ImU32 line_col = IM_COL32(255, 255, 255, 18);
    const ImU32 axis_x = IM_COL32(230, 90, 90, 110);   // the X axis
    const ImU32 axis_z = IM_COL32(90, 140, 230, 110);  // the Z axis
    // Lines are laid out on the spacing grid anchored to the WORLD origin, not
    // to the pivot: re-anchoring to the pivot would make the whole floor slide
    // as the user orbits, which is far more distracting than a fixed lattice.
    // The pivot only chooses WHICH 50 cells of that infinite lattice get drawn.
    // Both helpers are pure and live in Viewport.hpp, so the window is pinned
    // headlessly (test_viewport.cpp) rather than only by looking at the screen.
    const GridCellRange xr = grid_cell_range_around(center_x, spacing, half_extent);
    const GridCellRange zr = grid_cell_range_around(center_z, spacing, half_extent);
    // Pass 0 = lines of constant x (running along z), pass 1 = constant z.
    for (int pass = 0; pass < 2; ++pass) {
        const GridCellRange& r = (pass == 0) ? xr : zr;
        const bool along_z = (pass == 0);
        for (int i = r.lo; i <= r.hi; ++i) {
            const float o = static_cast<float>(i) * spacing;
            // Beyond 20 m only every other line survives; see grid_line_visible.
            if (!grid_line_visible(i, o)) {
                continue;
            }
            const float fixed = (along_z ? o : center_z);
            const float mid = (along_z ? center_x : o);
            ImVec2 a{}, b{};
            const bool ok_a =
                project_to_viewport(view_proj, fixed, 0.0f, mid - extent, min, max, a);
            const bool ok_b =
                project_to_viewport(view_proj, fixed, 0.0f, mid + extent, min, max, b);
            if (!ok_a || !ok_b) {
                continue; // a segment with an endpoint behind the camera is skipped whole
            }
            // The world axis lines are the ones that pass through x = 0 / z = 0,
            // which is NOT the same index as "the pivot line" — the pivot moves,
            // the origin does not.
            const bool on_axis = grid_cell_is_world_axis(i, spacing);
            const ImU32 col = on_axis ? (along_z ? axis_z : axis_x) : line_col;
            dl->AddLine(a, b, col, on_axis ? 1.4f : 1.0f);
        }
    }
}

void draw_2d_grid(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const Mat4& view_proj,
                  float spacing, int half_extent, float center_x, float center_y) {
    if (spacing <= 0.0f || half_extent <= 0) {
        return;
    }
    const float extent = static_cast<float>(half_extent) * spacing;
    const ImU32 line_col = IM_COL32(255, 255, 255, 18);
    const ImU32 axis_x = IM_COL32(230, 90, 90, 140);   // X axis (Red)
    const ImU32 axis_y = IM_COL32(90, 230, 110, 140);  // Y axis (Green)
    const GridCellRange xr = grid_cell_range_around(center_x, spacing, half_extent);
    const GridCellRange yr = grid_cell_range_around(center_y, spacing, half_extent);

    for (int pass = 0; pass < 2; ++pass) {
        const GridCellRange& r = (pass == 0) ? xr : yr;
        const bool along_y = (pass == 0);
        for (int i = r.lo; i <= r.hi; ++i) {
            const float o = static_cast<float>(i) * spacing;
            if (!grid_line_visible(i, o)) {
                continue;
            }
            ImVec2 a{}, b{};
            const bool ok_a = along_y
                ? project_to_viewport(view_proj, o, center_y - extent, 0.0f, min, max, a)
                : project_to_viewport(view_proj, center_x - extent, o, 0.0f, min, max, a);
            const bool ok_b = along_y
                ? project_to_viewport(view_proj, o, center_y + extent, 0.0f, min, max, b)
                : project_to_viewport(view_proj, center_x + extent, o, 0.0f, min, max, b);
            if (!ok_a || !ok_b) {
                continue;
            }
            const bool on_axis = grid_cell_is_world_axis(i, spacing);
            const ImU32 col = on_axis ? (along_y ? axis_y : axis_x) : line_col;
            dl->AddLine(a, b, col, on_axis ? 1.6f : 1.0f);
        }
    }
}

// --- Navigation debug overlay (Step 8) ---------------------------------------
//
// The baked mesh outlines plus every bound agent's remaining route, projected
// over the viewport image like the ground grid above: drawn, not composited,
// with the same "a segment with an endpoint behind the camera is skipped
// whole" rule. Read-only in every direction — it consumes
// Runtime::nav_debug_snapshot() and writes no scene, no component, no runtime
// state — so there is nothing to undo, save or localise inside the draw.
//
// Colours follow the grid's philosophy (quiet annotation, not competing
// geometry): mesh edges sit at grid-like alpha, agent routes ride the accent
// warm side so a route reads against both the mesh and the floor, arrived
// agents dim to the mesh colour because their story is over, and a walker held
// by its senses goes red so a paused patrol is visibly paused for a reason.
void draw_navmesh_overlay(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const Mat4& view_proj,
                          const runtime::NavDebugSnapshot& snap) {
    if (!snap.has_mesh) {
        return;
    }
    const ImU32 mesh_col = IM_COL32(110, 220, 180, 42);
    const ImU32 route_col = IM_COL32(255, 196, 110, 200);
    const ImU32 route_done_col = IM_COL32(110, 220, 180, 110);
    const ImU32 route_held_col = IM_COL32(255, 110, 110, 230);
    const ImU32 route_investigating_col = IM_COL32(255, 230, 80, 240);
    const ImU32 goal_col = IM_COL32(255, 196, 110, 220);
    for (const std::vector<Vec3>& loop : snap.mesh_polygons) {
        if (loop.size() < 3) {
            continue; // degenerate: the snapshot keeps the loop, the draw skips it
        }
        for (usize v = 0; v < loop.size(); ++v) {
            const Vec3& a3 = loop[v];
            const Vec3& b3 = loop[(v + 1) % loop.size()];
            ImVec2 a{}, b{};
            if (!project_to_viewport(view_proj, a3.x, a3.y, a3.z, min, max, a) ||
                !project_to_viewport(view_proj, b3.x, b3.y, b3.z, min, max, b)) {
                continue;
            }
            dl->AddLine(a, b, mesh_col, 1.0f);
        }
    }
    for (const runtime::NavDebugAgent& agent : snap.agents) {
        // Yellow is the search: the walker is off its route, looking at a
        // last known position. Red still means a hold, green means arrived.
        const ImU32 col = agent.route_investigating
                              ? route_investigating_col
                              : (agent.route_held ? route_held_col
                                                  : (agent.at_goal ? route_done_col : route_col));
        // Remaining route as a polyline through the unconsumed waypoints. The
        // agent's own marker is the first joint: the line starts where the
        // walker stands, not where the plan started.
        Vec3 prev = agent.position;
        for (const Vec3& wp : agent.path) {
            ImVec2 a{}, b{};
            if (project_to_viewport(view_proj, prev.x, prev.y, prev.z, min, max, a) &&
                project_to_viewport(view_proj, wp.x, wp.y, wp.z, min, max, b)) {
                dl->AddLine(a, b, col, 2.0f);
            }
            prev = wp;
        }
        // Agent marker + goal ring. An agent with no path (off the mesh,
        // unreachable) still gets both: a walker with nowhere to go is
        // exactly what this overlay exists to show.
        ImVec2 pa{}, pg{};
        if (project_to_viewport(view_proj, agent.position.x, agent.position.y, agent.position.z,
                                min, max, pa)) {
            dl->AddCircleFilled(pa, 3.5f, col, 12);
        }
        if (project_to_viewport(view_proj, agent.goal.x, agent.goal.y, agent.goal.z, min, max,
                                pg)) {
            dl->AddCircle(pg, 5.0f, goal_col, 16, 1.5f);
        }
    }
}

// --- Orientation gizmo -------------------------------------------------------
//
// The world axes projected through the camera's own basis, pinned to the
// viewport's bottom-right corner. Without it an orbited view has no fixed
// reference for which way is up — the ground grid only helps while it is
// switched on, and even then it is a reference on the floor, not a compass.
//
// The basis comes from position/target/up rather than from the view matrix on
// purpose: this engine builds its matrices row-vector and reading axes out of
// the columns of `view` is exactly the kind of sign slip that mirrors the
// widget without ever failing to compile.
void draw_orientation_gizmo(ImDrawList* dl, const ImVec2& max, const rendering::Camera& cam) {
    const float line_h = ImGui::GetTextLineHeight();
    const float radius = line_h * 2.0f;
    const float pad = line_h * 0.9f;
    const ImVec2 center(max.x - radius - pad, max.y - radius - pad);

    float fx = cam.target.x - cam.position.x;
    float fy = cam.target.y - cam.position.y;
    float fz = cam.target.z - cam.position.z;
    const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl <= 1e-6f) {
        return;
    }
    fx /= fl;
    fy /= fl;
    fz /= fl;

    // A near-vertical look direction makes cross(forward, world_up) degenerate;
    // -Z is the fallback so the widget keeps turning instead of collapsing.
    float ux = 0.0f, uy = 1.0f, uz = 0.0f;
    if (std::fabs(fy) > 0.999f) {
        ux = 0.0f;
        uy = 0.0f;
        uz = 1.0f;
    }
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl <= 1e-6f) {
        return;
    }
    rx /= rl;
    ry /= rl;
    rz /= rl;
    // true_up = cross(right, forward)
    const float tx = ry * fz - rz * fy;
    const float ty = rz * fx - rx * fz;
    const float tz = rx * fy - ry * fx;

    struct Axis {
        float x, y, z;
        ImVec4 col;
        const char* label;
    };
    const Axis axes[3] = {
        {1.0f, 0.0f, 0.0f, theme::axis_x(), "X"},
        {0.0f, 1.0f, 0.0f, theme::axis_y(), "Y"},
        {0.0f, 0.0f, 1.0f, theme::axis_z(), "Z"},
    };
    // Depth = how much the axis points away from the camera. Draw far to near
    // so an axis pointing into the screen passes behind the other two.
    float depth[3];
    for (int i = 0; i < 3; ++i) {
        depth[i] = axes[i].x * fx + axes[i].y * fy + axes[i].z * fz;
    }
    int order[3] = {0, 1, 2};
    for (int i = 1; i < 3; ++i) {
        const int key = order[i];
        int j = i - 1;
        while (j >= 0 && depth[order[j]] < depth[key]) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }

    // Three clean arms from the origin, nothing else: no disc, no ring, no
    // tails. Every extra shape was visual noise — this is the Unity/Blender
    // tripod: colour + letter is the whole encoding.
    dl->AddCircleFilled(center, 2.5f, IM_COL32(20, 22, 28, 230), 12);
    for (int i = 0; i < 3; ++i) {
        const Axis& a = axes[order[i]];
        const float dx = (a.x * rx + a.y * ry + a.z * rz) * radius;
        const float dy = -(a.x * tx + a.y * ty + a.z * tz) * radius;
        const ImVec2 tip(center.x + dx, center.y + dy);
        const ImU32 col = ImGui::GetColorU32(a.col);
        // Arm: dark outline under the colour keeps it legible over any
        // background.
        dl->AddLine(center, tip, IM_COL32(0, 0, 0, 130), 4.5f);
        dl->AddLine(center, tip, col, 2.5f);
        // Knob: dark ring, colour fill, white letter with a shadow.
        const float knob = line_h * 0.40f;
        dl->AddCircleFilled(tip, knob + 1.5f, IM_COL32(12, 14, 18, 220), 20);
        dl->AddCircleFilled(tip, knob, col, 20);
        const ImVec2 ts = ImGui::CalcTextSize(a.label);
        const ImVec2 tp(tip.x - ts.x * 0.5f, tip.y - ts.y * 0.5f);
        dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 200), a.label);
        dl->AddText(tp, IM_COL32(255, 255, 255, 255), a.label);
    }

    // Hover hint, only while the pointer is actually over the widget.
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const float hx = mp.x - center.x;
    const float hy = mp.y - center.y;
    const float hit = radius + line_h * 0.55f;
    if (hx * hx + hy * hy < hit * hit) {
        ImGui::SetTooltip("%s", AV("axis_gizmo_hint").c_str());
    }
}

// The per-row category chip in the console.
//
// DRAWN, not a Button. As a SmallButton its label WAS the category name, so any
// two visible log lines in the same category submitted two items with the same
// ID — ImGui's "2 visible items with conflicting ID!" popup (and, in the
// non-programmer build, an invisible hit target that did nothing when clicked).
// The conflict only surfaced once the panel was tall enough to show two lines of
// one category at once, which is why it appeared after a layout change.
//
// A chip is a LABEL. A label must not consume an ID.
void draw_category_chip(const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float pad_x = 6.0f;
    const float h = ts.y + 4.0f;
    const ImVec2 a(pos.x, pos.y);
    const ImVec2 b(pos.x + ts.x + pad_x * 2.0f, pos.y + h);
    dl->AddRectFilled(a, b, IM_COL32(52, 56, 63, 255), 3.0f);
    dl->AddText(ImVec2(a.x + pad_x, a.y + 2.0f), IM_COL32(168, 176, 188, 255), text);
    // Reserve the space the chip occupies so SameLine() lands after it.
    ImGui::Dummy(ImVec2(b.x - a.x, h));
}

// Row tint per entity type in the outliner.
//
// A colour, not an icon glyph. The UI font has no icon glyphs — Toolbar.cpp
// learned that the hard way, every typed symbol rasterised as an empty box —
// and drawing a glyph over the label would fight the tree node's own hit box,
// which is what selection, drag & drop and the context menu hang off. A tint is
// one PushStyleColor and cannot break any of them.
//
// Only the SPECIAL kinds are tinted; a mesh is the common case and stays the
// theme's text colour, so the hierarchy does not turn into a colour chart.
// Alpha 0 means "leave the theme colour alone".
ImVec4 outliner_kind_color(OutlinerKind kind) {
    switch (kind) {
        case OutlinerKind::Camera:
            return ImVec4(0.55f, 0.78f, 1.00f, 1.0f); // cool blue
        case OutlinerKind::Light:
            return ImVec4(1.00f, 0.80f, 0.45f, 1.0f); // warm amber
        case OutlinerKind::Sky:
            return ImVec4(0.60f, 0.88f, 0.85f, 1.0f); // pale teal
        case OutlinerKind::Sprite2D:
            return ImVec4(0.30f, 0.88f, 0.60f, 1.0f); // vibrant emerald
        case OutlinerKind::Tilemap:
            return ImVec4(0.40f, 0.92f, 0.75f, 1.0f); // mint tilemap
        case OutlinerKind::Audio:
            return ImVec4(0.85f, 0.55f, 1.00f, 1.0f); // rich violet
        case OutlinerKind::Physics:
            return ImVec4(1.00f, 0.60f, 0.35f, 1.0f); // sunset orange
        case OutlinerKind::Weather:
            return ImVec4(0.35f, 0.75f, 1.00f, 1.0f); // vibrant sky blue
        case OutlinerKind::Cable:
            return ImVec4(1.00f, 0.65f, 0.25f, 1.0f); // warm cable amber
        case OutlinerKind::RuleTile:
            return ImVec4(0.35f, 0.90f, 0.65f, 1.0f); // vibrant mint
        case OutlinerKind::FlowField:
            return ImVec4(0.20f, 0.85f, 0.50f, 1.0f); // emerald green
        case OutlinerKind::TalentTree:
            return ImVec4(0.95f, 0.45f, 0.75f, 1.0f); // talent pink
        case OutlinerKind::Inventory:
            return ImVec4(0.70f, 0.50f, 0.95f, 1.0f); // inventory purple
        case OutlinerKind::Ragdoll:
            return ImVec4(0.95f, 0.40f, 0.40f, 1.0f); // physics coral
        case OutlinerKind::Mesh:
        case OutlinerKind::Empty:
        default:
            return ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}

// Tiny vector glyph per entity kind, drawn with ImDrawList right after the
// tree node (never a font glyph: the UI font has no icon codepoints, typed
// symbols rasterise as tofu). Same flat style as the FileSystem icons, sized
// for a tree row (~13px): one glance tells camera / sun / sky / cube apart.
void outliner_kind_icon(ImDrawList* dl, ImVec2 c, float s, OutlinerKind kind) {
    if (dl == nullptr || s <= 0.0f) {
        return;
    }
    const float h = s * 0.5f;
    const float t = (s * 0.10f > 1.2f) ? s * 0.10f : 1.2f;
    switch (kind) {
        case OutlinerKind::Camera: {
            const ImU32 body = IM_COL32(140, 200, 255, 255);
            const float w = h * 0.95f;
            const float bh = h * 0.62f;
            // Viewfinder bump.
            dl->AddRectFilled(ImVec2(c.x - w * 0.55f, c.y - bh - h * 0.28f),
                              ImVec2(c.x - w * 0.05f, c.y - bh + 1.0f), body, 1.0f);
            // Body.
            dl->AddRectFilled(ImVec2(c.x - w, c.y - bh), ImVec2(c.x + w, c.y + bh), body,
                              1.5f);
            // Lens: dark ring + glass dot.
            dl->AddCircleFilled(ImVec2(c.x + w * 0.25f, c.y), bh * 0.52f,
                                IM_COL32(24, 32, 44, 255), 16);
            dl->AddCircleFilled(ImVec2(c.x + w * 0.25f, c.y), bh * 0.26f, body, 12);
            break;
        }
        case OutlinerKind::Light: {
            const ImU32 sun = IM_COL32(255, 205, 110, 255);
            dl->AddCircleFilled(c, h * 0.36f, sun, 16);
            // 8 rays: axes + diagonals, no trig needed.
            const float r0 = h * 0.55f;
            const float r1 = h * 0.92f;
            const float d = 0.7071f;
            const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1},  {0, -1},
                                      {d, d}, {d, -d}, {-d, d}, {-d, -d}};
            for (const auto& v : dirs) {
                dl->AddLine(ImVec2(c.x + v[0] * r0, c.y + v[1] * r0),
                            ImVec2(c.x + v[0] * r1, c.y + v[1] * r1), sun, t * 0.8f);
            }
            break;
        }
        case OutlinerKind::Sky: {
            const ImU32 cl = IM_COL32(150, 225, 215, 255);
            // Fluffy cloud, fills the cell: flat base + three puffs.
            dl->AddRectFilled(ImVec2(c.x - h * 0.78f, c.y - h * 0.02f),
                              ImVec2(c.x + h * 0.78f, c.y + h * 0.48f), cl, 2.5f);
            dl->AddCircleFilled(ImVec2(c.x - h * 0.42f, c.y - h * 0.10f), h * 0.42f,
                                cl, 14);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.08f, c.y - h * 0.32f), h * 0.54f,
                                cl, 16);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.55f, c.y - h * 0.02f), h * 0.32f,
                                cl, 12);
            break;
        }
        case OutlinerKind::Sprite2D: {
            const ImU32 frame = IM_COL32(36, 180, 120, 255);
            const ImU32 inner = IM_COL32(20, 45, 35, 255);
            const ImU32 sun = IM_COL32(255, 215, 100, 255);
            const ImU32 mountain = IM_COL32(52, 211, 153, 255);
            dl->AddRectFilled(ImVec2(c.x - h * 0.90f, c.y - h * 0.80f),
                              ImVec2(c.x + h * 0.90f, c.y + h * 0.80f), inner, 2.0f);
            dl->AddRect(ImVec2(c.x - h * 0.90f, c.y - h * 0.80f),
                        ImVec2(c.x + h * 0.90f, c.y + h * 0.80f), frame, 2.0f, 0, t * 0.9f);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.40f, c.y - h * 0.30f), h * 0.22f, sun, 10);
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.70f, c.y + h * 0.60f),
                                  ImVec2(c.x - h * 0.05f, c.y - h * 0.15f),
                                  ImVec2(c.x + h * 0.60f, c.y + h * 0.60f), mountain);
            break;
        }
        case OutlinerKind::Tilemap: {
            const ImU32 border = IM_COL32(45, 212, 160, 255);
            const ImU32 tile_a = IM_COL32(52, 211, 153, 220);
            const ImU32 tile_b = IM_COL32(20, 70, 50, 200);
            const float q = h * 0.85f;
            dl->AddRectFilled(ImVec2(c.x - q, c.y - q), ImVec2(c.x, c.y), tile_a, 1.0f);
            dl->AddRectFilled(ImVec2(c.x, c.y - q), ImVec2(c.x + q, c.y), tile_b, 1.0f);
            dl->AddRectFilled(ImVec2(c.x - q, c.y), ImVec2(c.x, c.y + q), tile_b, 1.0f);
            dl->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.x + q, c.y + q), tile_a, 1.0f);
            dl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), border, 1.5f, 0, t);
            dl->AddLine(ImVec2(c.x - q, c.y), ImVec2(c.x + q, c.y), border, t * 0.8f);
            dl->AddLine(ImVec2(c.x, c.y - q), ImVec2(c.x, c.y + q), border, t * 0.8f);
            break;
        }
        case OutlinerKind::Audio: {
            const ImU32 spk = IM_COL32(200, 140, 255, 255);
            dl->AddRectFilled(ImVec2(c.x - h * 0.80f, c.y - h * 0.35f),
                              ImVec2(c.x - h * 0.30f, c.y + h * 0.35f), spk, 1.0f);
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.30f, c.y - h * 0.35f),
                                  ImVec2(c.x + h * 0.20f, c.y - h * 0.75f),
                                  ImVec2(c.x - h * 0.30f, c.y + h * 0.35f), spk);
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.30f, c.y + h * 0.35f),
                                  ImVec2(c.x + h * 0.20f, c.y - h * 0.75f),
                                  ImVec2(c.x + h * 0.20f, c.y + h * 0.75f), spk);
            dl->PathArcTo(ImVec2(c.x + h * 0.10f, c.y), h * 0.45f, -0.25f * 3.14159f, 0.25f * 3.14159f, 8);
            dl->PathStroke(spk, 0, t * 0.85f);
            dl->PathArcTo(ImVec2(c.x + h * 0.10f, c.y), h * 0.80f, -0.30f * 3.14159f, 0.30f * 3.14159f, 10);
            dl->PathStroke(spk, 0, t * 0.85f);
            break;
        }
        case OutlinerKind::Physics: {
            const ImU32 phys = IM_COL32(255, 150, 70, 255);
            const float pw = h * 0.85f;
            const ImVec2 p0(c.x, c.y - h * 0.90f);
            const ImVec2 p1(c.x + pw, c.y - h * 0.50f);
            const ImVec2 p2(c.x + pw * 0.75f, c.y + h * 0.35f);
            const ImVec2 p3(c.x, c.y + h * 0.95f);
            const ImVec2 p4(c.x - pw * 0.75f, c.y + h * 0.35f);
            const ImVec2 p5(c.x - pw, c.y - h * 0.50f);
            dl->AddTriangleFilled(p0, p1, p3, IM_COL32(60, 30, 15, 200));
            dl->AddTriangleFilled(p0, p3, p5, IM_COL32(60, 30, 15, 200));
            dl->AddLine(p0, p1, phys, t);
            dl->AddLine(p1, p2, phys, t);
            dl->AddLine(p2, p3, phys, t);
            dl->AddLine(p3, p4, phys, t);
            dl->AddLine(p4, p5, phys, t);
            dl->AddLine(p5, p0, phys, t);
            dl->AddCircleFilled(c, h * 0.28f, phys, 10);
            break;
        }
        case OutlinerKind::Mesh: {
            // Solid cube: three shaded faces, not an outline — reads as a
            // cube at a glance instead of a diamond.
            const ImU32 top = IM_COL32(205, 220, 238, 255);
            const ImU32 left = IM_COL32(140, 162, 188, 255);
            const ImU32 right = IM_COL32(98, 118, 148, 255);
            const ImVec2 ctop(c.x, c.y - h);
            const ImVec2 r(c.x + h * 0.88f, c.y - h * 0.42f);
            const ImVec2 m(c.x, c.y + h * 0.02f);
            const ImVec2 l(c.x - h * 0.88f, c.y - h * 0.42f);
            const ImVec2 b(c.x, c.y + h);
            const ImVec2 bl(c.x - h * 0.88f, c.y + h * 0.52f);
            const ImVec2 br(c.x + h * 0.88f, c.y + h * 0.52f);
            dl->AddQuadFilled(ctop, r, m, l, top);
            dl->AddQuadFilled(l, m, b, bl, left);
            dl->AddQuadFilled(m, r, br, b, right);
            break;
        }
        case OutlinerKind::Weather: {
            const ImU32 sun_col = IM_COL32(255, 200, 60, 255);
            const ImU32 cld_col = IM_COL32(190, 220, 255, 255);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.3f, c.y - h * 0.3f), h * 0.45f, sun_col, 10);
            dl->AddCircleFilled(ImVec2(c.x - h * 0.25f, c.y + h * 0.2f), h * 0.38f, cld_col, 10);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.2f, c.y + h * 0.2f), h * 0.45f, cld_col, 10);
            dl->AddRectFilled(ImVec2(c.x - h * 0.25f, c.y), ImVec2(c.x + h * 0.2f, c.y + h * 0.5f), cld_col);
            break;
        }
        case OutlinerKind::Cable: {
            const ImU32 cab_col = IM_COL32(255, 170, 70, 255);
            const ImVec2 p1(c.x - h * 0.85f, c.y - h * 0.5f);
            const ImVec2 p2(c.x - h * 0.3f, c.y + h * 0.65f);
            const ImVec2 p3(c.x + h * 0.3f, c.y + h * 0.65f);
            const ImVec2 p4(c.x + h * 0.85f, c.y - h * 0.5f);
            dl->AddBezierCubic(p1, p2, p3, p4, cab_col, t * 1.3f);
            dl->AddCircleFilled(p1, t * 0.9f, cab_col, 6);
            dl->AddCircleFilled(p4, t * 0.9f, cab_col, 6);
            break;
        }
        case OutlinerKind::RuleTile: {
            const ImU32 t1 = IM_COL32(80, 220, 160, 255);
            const ImU32 t2 = IM_COL32(30, 140, 95, 255);
            const float q = h * 0.7f;
            dl->AddRectFilled(ImVec2(c.x - q, c.y - q), ImVec2(c.x, c.y), t1, 1.0f);
            dl->AddRectFilled(ImVec2(c.x, c.y - q), ImVec2(c.x + q, c.y), t2, 1.0f);
            dl->AddRectFilled(ImVec2(c.x - q, c.y), ImVec2(c.x, c.y + q), t2, 1.0f);
            dl->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.x + q, c.y + q), t1, 1.0f);
            break;
        }
        case OutlinerKind::FlowField: {
            const ImU32 fl_col = IM_COL32(50, 220, 130, 255);
            dl->AddLine(ImVec2(c.x - h * 0.7f, c.y - h * 0.3f), ImVec2(c.x + h * 0.6f, c.y - h * 0.3f), fl_col, t);
            dl->AddTriangleFilled(ImVec2(c.x + h * 0.85f, c.y - h * 0.3f), ImVec2(c.x + h * 0.35f, c.y - h * 0.6f), ImVec2(c.x + h * 0.35f, c.y), fl_col);
            dl->AddLine(ImVec2(c.x - h * 0.7f, c.y + h * 0.35f), ImVec2(c.x + h * 0.6f, c.y + h * 0.35f), fl_col, t);
            dl->AddTriangleFilled(ImVec2(c.x + h * 0.85f, c.y + h * 0.35f), ImVec2(c.x + h * 0.35f, c.y + h * 0.05f), ImVec2(c.x + h * 0.35f, c.y + h * 0.65f), fl_col);
            break;
        }
        case OutlinerKind::TalentTree: {
            const ImU32 tal_col = IM_COL32(245, 120, 190, 255);
            const float r = h * 0.85f;
            dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x - r * 0.3f, c.y), ImVec2(c.x + r * 0.3f, c.y), tal_col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + r), ImVec2(c.x - r * 0.3f, c.y), ImVec2(c.x + r * 0.3f, c.y), tal_col);
            dl->AddTriangleFilled(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - r * 0.3f), ImVec2(c.x, c.y + r * 0.3f), tal_col);
            dl->AddTriangleFilled(ImVec2(c.x + r, c.y), ImVec2(c.x, c.y - r * 0.3f), ImVec2(c.x, c.y + r * 0.3f), tal_col);
            break;
        }
        case OutlinerKind::Inventory: {
            const ImU32 inv_col = IM_COL32(180, 130, 245, 255);
            dl->AddRectFilled(ImVec2(c.x - h * 0.7f, c.y - h * 0.5f), ImVec2(c.x + h * 0.7f, c.y + h * 0.75f), inv_col, 2.0f);
            dl->AddRect(ImVec2(c.x - h * 0.4f, c.y - h * 0.85f), ImVec2(c.x + h * 0.4f, c.y - h * 0.5f), inv_col, 1.0f);
            break;
        }
        case OutlinerKind::Ragdoll: {
            const ImU32 rag_col = IM_COL32(245, 100, 100, 255);
            dl->AddCircleFilled(ImVec2(c.x, c.y - h * 0.55f), h * 0.28f, rag_col, 8);
            dl->AddLine(ImVec2(c.x, c.y - h * 0.25f), ImVec2(c.x, c.y + h * 0.25f), rag_col, t);
            dl->AddLine(ImVec2(c.x - h * 0.55f, c.y - h * 0.1f), ImVec2(c.x + h * 0.55f, c.y - h * 0.1f), rag_col, t);
            dl->AddLine(ImVec2(c.x, c.y + h * 0.25f), ImVec2(c.x - h * 0.5f, c.y + h * 0.8f), rag_col, t);
            dl->AddLine(ImVec2(c.x, c.y + h * 0.25f), ImVec2(c.x + h * 0.5f, c.y + h * 0.8f), rag_col, t);
            break;
        }
        case OutlinerKind::Empty:
        default: {
            // Plain object: small hollow square, dim so it stays quiet.
            const ImU32 dim = IM_COL32(125, 132, 145, 200);
            const float q = h * 0.62f;
            dl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), dim, 1.5f,
                        0, t * 0.8f);
            break;
        }
    }
}

// Ground plane glyph: a flat quad with grid lines. The outliner only knows
// "Mesh" by components, so Ground and Cube would share the cube — the name
// check below (display layer only) gives the default scene's ground a plane
// that cannot be confused with a cube.
bool outliner_is_ground_label(const std::string& label) {
    if (label.size() != 6) {
        return false;
    }
    const char* want = "ground";
    for (size_t i = 0; i < 6; ++i) {
        char ch = label[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
        if (ch != want[i]) {
            return false;
        }
    }
    return true;
}

void outliner_plane_icon(ImDrawList* dl, ImVec2 c, float s) {
    if (dl == nullptr || s <= 0.0f) {
        return;
    }
    const float h = s * 0.5f;
    const float t = (s * 0.10f > 1.2f) ? s * 0.10f : 1.2f;
    const ImU32 grass = IM_COL32(115, 195, 135, 255);
    const ImU32 dark = IM_COL32(70, 130, 90, 255);
    // Flat perspective quad.
    const ImVec2 l(c.x - h * 0.95f, c.y + h * 0.05f);
    const ImVec2 r(c.x + h * 0.95f, c.y + h * 0.05f);
    const ImVec2 br(c.x + h * 0.55f, c.y + h * 0.60f);
    const ImVec2 bl(c.x - h * 0.55f, c.y + h * 0.60f);
    dl->AddQuadFilled(l, r, br, bl, grass);
    // Grid lines across it.
    dl->AddLine(ImVec2(c.x - h * 0.75f, c.y + h * 0.33f),
                ImVec2(c.x + h * 0.75f, c.y + h * 0.33f), dark, t * 0.8f);
    dl->AddLine(ImVec2(c.x - h * 0.12f, c.y + h * 0.05f),
                ImVec2(c.x - h * 0.07f, c.y + h * 0.60f), dark, t * 0.8f);
    dl->AddLine(ImVec2(c.x + h * 0.12f, c.y + h * 0.05f),
                ImVec2(c.x + h * 0.07f, c.y + h * 0.60f), dark, t * 0.8f);
    // Horizon: the plane floats under it.
    dl->AddLine(ImVec2(c.x - h * 0.95f, c.y - h * 0.45f),
                ImVec2(c.x + h * 0.95f, c.y - h * 0.45f), dark, t * 0.8f);
}

bool outliner_is_audio_label(const std::string& label) {
    std::string lower;
    lower.reserve(label.size());
    for (char c : label) {
        if (c >= 'A' && c <= 'Z') {
            lower.push_back(static_cast<char>(c + ('a' - 'A')));
        } else {
            lower.push_back(c);
        }
    }
    return lower.find("audio") != std::string::npos ||
           lower.find("sound") != std::string::npos ||
           lower.find("chime") != std::string::npos ||
           lower.find("hum") != std::string::npos ||
           lower.find("music") != std::string::npos ||
           lower.find("voice") != std::string::npos;
}

void outliner_audio_icon(ImDrawList* dl, ImVec2 c, float s) {
    if (dl == nullptr || s <= 0.0f) {
        return;
    }
    const float h = s * 0.5f;
    const ImU32 spk_col = IM_COL32(185, 155, 245, 255);
    // Speaker box
    dl->AddRectFilled(ImVec2(c.x - h * 0.8f, c.y - h * 0.35f),
                      ImVec2(c.x - h * 0.3f, c.y + h * 0.35f), spk_col, 1.0f);
    // Speaker cone
    const ImVec2 p1(c.x - h * 0.3f, c.y - h * 0.35f);
    const ImVec2 p2(c.x + h * 0.15f, c.y - h * 0.72f);
    const ImVec2 p3(c.x + h * 0.15f, c.y + h * 0.72f);
    const ImVec2 p4(c.x - h * 0.3f, c.y + h * 0.35f);
    dl->AddQuadFilled(p1, p2, p3, p4, spk_col);
    // Sound wave arcs
    dl->PathArcTo(ImVec2(c.x + h * 0.15f, c.y), h * 0.42f, -0.32f * 3.14159265f, 0.32f * 3.14159265f, 6);
    dl->PathStroke(spk_col, 0, 1.3f);
    dl->PathArcTo(ImVec2(c.x + h * 0.15f, c.y), h * 0.75f, -0.32f * 3.14159265f, 0.32f * 3.14159265f, 6);
    dl->PathStroke(spk_col, 0, 1.3f);
}

// Colored X/Y/Z vector control (Blender/Unreal style).
// X = Red, Y = Green, Z = Blue with live drag, click-to-reset pill buttons.
bool draw_vec3_control(const char* label, float values[3], float reset_val = 0.0f,
                       float speed = 0.05f, float min_val = 0.0f, float max_val = 0.0f,
                       const char* format = "%.2f") {
    bool changed = false;
    ImGui::PushID(label);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float label_w = 75.0f;
    const float item_w = std::max(40.0f, (avail_w - label_w - 60.0f) / 3.0f);

    ImGui::TextUnformatted(label);
    ImGui::SameLine(label_w);

    const float line_h = ImGui::GetFrameHeight();
    const ImVec2 btn_sz(line_h * 0.9f, line_h);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 2.0f));

    // X Component (Red)
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(215, 55, 55, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(235, 75, 75, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(190, 40, 40, 255));
    if (ImGui::Button("X##btn", btn_sz)) {
        values[0] = reset_val;
        changed = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(item_w);
    if (ImGui::DragFloat("##X", &values[0], speed, min_val, max_val, format)) {
        changed = true;
    }
    ImGui::SameLine();

    // Y Component (Green)
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(45, 175, 75, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(65, 205, 95, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(35, 145, 55, 255));
    if (ImGui::Button("Y##btn", btn_sz)) {
        values[1] = reset_val;
        changed = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(item_w);
    if (ImGui::DragFloat("##Y", &values[1], speed, min_val, max_val, format)) {
        changed = true;
    }
    ImGui::SameLine();

    // Z Component (Blue)
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(45, 115, 225, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 140, 245, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(35, 95, 200, 255));
    if (ImGui::Button("Z##btn", btn_sz)) {
        values[2] = reset_val;
        changed = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(item_w);
    if (ImGui::DragFloat("##Z", &values[2], speed, min_val, max_val, format)) {
        changed = true;
    }

    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

// --- Reflection-driven widgets ---------------------------------------------
//
// The widget dispatch is a direct translation of ReflectedWidgetKind; all the
// policy (which properties are visible, which widget, how they group) lives in
// ReflectedObjectView, which is covered by EditorTests. Keeping the ImGui side
// free of decisions is the point: there is nothing here worth testing that is
// not already tested one layer down.
//
// Returns true when the user changed the value; the write goes through the view,
// which validates before touching the object.
bool draw_reflected_field(ReflectedObjectView& view, usize index) {
    const ReflectedField& field = view.fields()[index];
    const std::string current = view.value_of(index);
    bool changed = false;

    ImGui::PushID(static_cast<int>(index));

    switch (field.widget) {
        case ReflectedWidgetKind::FloatDrag: {
            float v = std::strtof(current.c_str(), nullptr);
            if (ImGui::DragFloat(field.label.c_str(), &v, 0.01f)) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
                changed = view.set_value(index, buf);
            }
            break;
        }
        case ReflectedWidgetKind::IntDrag: {
            int v = static_cast<int>(std::strtol(current.c_str(), nullptr, 10));
            if (ImGui::DragInt(field.label.c_str(), &v)) {
                changed = view.set_value(index, std::to_string(v));
            }
            break;
        }
        case ReflectedWidgetKind::BoolCheckbox: {
            bool v = current == "true";
            if (ImGui::Checkbox(field.label.c_str(), &v)) {
                changed = view.set_value(index, v ? "true" : "false");
            }
            break;
        }
        case ReflectedWidgetKind::TextInput: {
            char buffer[256]{};
            std::strncpy(buffer, current.c_str(), sizeof(buffer) - 1);
            if (ImGui::InputText(field.label.c_str(), buffer, sizeof(buffer))) {
                changed = view.set_value(index, buffer);
            }
            break;
        }
        case ReflectedWidgetKind::Vec3Drag: {
            float v[3] = {0.0f, 0.0f, 0.0f};
            std::sscanf(current.c_str(), "%f %f %f", &v[0], &v[1], &v[2]);
            if (ImGui::DragFloat3(field.label.c_str(), v, 0.01f)) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%.9g %.9g %.9g",
                              static_cast<double>(v[0]), static_cast<double>(v[1]),
                              static_cast<double>(v[2]));
                changed = view.set_value(index, buf);
            }
            break;
        }
        case ReflectedWidgetKind::QuatEulerDrag: {
            // Edited in degrees, stored as a quaternion. The conversion is the
            // engine's own pair in NF/Scene/Transform.hpp rather than a second
            // implementation here, so the inspector and the physics write-back
            // cannot disagree about what a rotation means.
            float qv[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            std::sscanf(current.c_str(), "%f %f %f %f", &qv[0], &qv[1], &qv[2], &qv[3]);
            float rx = 0.0f, ry = 0.0f, rz = 0.0f;
            scene::euler_xyz_degrees_from_quat(Quat{qv[0], qv[1], qv[2], qv[3]}, rx, ry, rz);

            float degrees[3] = {rx, ry, rz};
            if (ImGui::DragFloat3(field.label.c_str(), degrees, 0.5f)) {
                const Quat q = scene::quat_from_euler_xyz_degrees(degrees[0], degrees[1], degrees[2]);
                char buf[200];
                std::snprintf(buf, sizeof(buf), "%.9g %.9g %.9g %.9g",
                              static_cast<double>(q.x), static_cast<double>(q.y),
                              static_cast<double>(q.z), static_cast<double>(q.w));
                changed = view.set_value(index, buf);
            }
            break;
        }
        case ReflectedWidgetKind::EntityPicker: {
            // An id field, not a real picker: a click-to-pick control needs the
            // viewport's selection flow, and a half-wired picker that silently
            // clears the reference is worse than an explicit id box.
            char buffer[32]{};
            std::strncpy(buffer, current.c_str(), sizeof(buffer) - 1);
            if (ImGui::InputText(field.label.c_str(), buffer, sizeof(buffer))) {
                changed = view.set_value(index, buffer);
            }
            break;
        }
        case ReflectedWidgetKind::EnumCombo: {
            if (field.enum_variants.empty()) {
                ImGui::TextDisabled("%s: (unregistered enum)", field.label.c_str());
                break;
            }
            int selected = 0;
            for (usize i = 0; i < field.enum_variants.size(); ++i) {
                if (field.enum_variants[i] == current) selected = static_cast<int>(i);
            }
            std::vector<const char*> names;
            names.reserve(field.enum_variants.size());
            for (const std::string& name : field.enum_variants) names.push_back(name.c_str());

            if (ImGui::Combo(field.label.c_str(), &selected, names.data(),
                             static_cast<int>(names.size()))) {
                changed = view.set_value(index, field.enum_variants[static_cast<usize>(selected)]);
            }
            break;
        }
        case ReflectedWidgetKind::Unsupported:
            // Shown read-only rather than hidden: a reflected property with no
            // widget is a gap in the editor, and a gap that renders as nothing is
            // indistinguishable from a property that was never declared.
            ImGui::TextDisabled("%s = %s (no widget for this type)",
                                field.label.c_str(), current.c_str());
            break;
    }

    ImGui::PopID();
    return changed;
}

} // namespace

// --- Start Here guide -------------------------------------------------------
// A first launch used to be a viewport, four panels and no instruction. Three
// buttons cover the whole beginner path — new scene, ground, play — and each
// one goes through the same EditorApp call the menu does, so the guide cannot
// drift into a second, broken implementation of "New".
void draw_start_here(EditorApp& app) {
    EditorUiSettings& st = ui_settings();
    if (!st.show_start_here) {
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin((AV("start_here_title") + "###NFStartHere").c_str(), &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", AV("start_here_intro").c_str());
        ImGui::Spacing();
        // Negative width = fill the window; the panel is auto-resizing, so the
        // three buttons share one deliberate column instead of three sizes.
        // Outline, not fill: the guide is the one place three actions must read
        // as a path, and a filled brand color would compete with Play/active
        // tool state elsewhere. A blue hairline + white label is unambiguous
        // and stays "plain" (the old gold fill was the thing being complained
        // about).
        const ImVec2 wide(-1.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::alpha(theme::accent(), 0.16f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::alpha(theme::accent(), 0.28f));
        ImGui::PushStyleColor(ImGuiCol_Border, theme::accent());
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        std::string err;
        if (ImGui::Button((AV("start_here_step1") + "##nf_sh_1").c_str(), wide)) {
            if (!app.new_scene(err)) {
                push_error(app.console(), "New scene failed", err);
            }
        }
        if (ImGui::Button((AV("start_here_step2") + "##nf_sh_2").c_str(), wide)) {
            if (!app.create_primitive(PrimitiveKind::Ground, ecs::kInvalidEntity, err)) {
                push_error(app.console(), "Add ground failed", err);
            }
        }
        if (ImGui::Button((AV("start_here_step3") + "##nf_sh_3").c_str(), wide)) {
            if (!app.launch_game(err)) {
                push_error(app.console(), "Play failed", err);
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", AV("start_here_tip").c_str());
    }
    ImGui::End();
    if (!open) {
        st.show_start_here = false;
        // Persist the dismissal so the guide does not reopen on every launch.
        LauncherSettings saved = load_settings();
        saved.start_here = false;
        save_settings(saved);
    }
}

UiIntents ui_frame(EditorApp& app, const UiFrameStats& stats) {
    UiIntents intents;

    // Autosave runs off the frame clock, before any panel draws. Ticking it here
    // rather than from Runtime::update keeps the Runtime unaware of save slots.
    app.tick_autosave(stats.dt_seconds);

    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Top bar: menus + toolbar + dialogs (NF/Editor/ToolbarUi.hpp).
    // Recorded first so BeginViewportSideBar reserves the top area BEFORE
    // DockSpaceOverViewport, keeping the dockspace perfectly below the toolbar.
    toolbar_ui(app, stats, intents);

    // Full-window dockspace. A human session restores the layout it saved last
    // time (ui_init's IniFilename), so the default below is only built when
    // there is nothing to restore — rebuilding unconditionally would throw the
    // user's arrangement away on every launch. A scripted run has no ini and
    // therefore always gets the deterministic default.
    {
        const ImGuiID dockspace_id = ImGui::GetID("SANADDockSpace");
        ImGui::DockSpaceOverViewport(dockspace_id, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
        static bool dock_layout_built = false;
        // ImGui loads the ini during the first NewFrame, so by now a restored
        // layout already has a split node tree under this id.
        const ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspace_id);
        const bool layout_restored = (existing != nullptr && existing->ChildNodes[0] != nullptr);
        if (!dock_layout_built || ui_settings().reset_layout_requested) {
            const bool want_default = ui_settings().reset_layout_requested || !layout_restored;
            dock_layout_built = true;
            ui_settings().reset_layout_requested = false;
            if (want_default) {
                ImGui::DockBuilderRemoveNode(dockspace_id);
                ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
                ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

            ImGuiID dock_left = 0, dock_left_top = 0, dock_left_bottom = 0;
            ImGuiID dock_right = 0, dock_bottom = 0, dock_center = 0;
            ImGuiID dock_rest = dockspace_id;
            // Left column: outliner on top, FileSystem browser underneath. The
            // browser belongs on the left, under the tree it browses — that is
            // the arrangement every scene editor converges on, and it fills the
            // dead space a short outliner otherwise leaves. The column is wider
            // than a bare tree needs because it now carries two panels, and the
            // split is even so the browser clears its own chrome (tabs + two
            // nav rows + filter) with room left for a list.
            ImGui::DockBuilderSplitNode(dock_rest, ImGuiDir_Left, 0.24f, &dock_left, &dock_rest);
            ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.50f, &dock_left_bottom,
                                        &dock_left_top);
            // 0.32, not 0.26: the right column now carries FOUR tabs (Inspector,
            // Camera, Lighting, Environment) and a tab bar that does not fit its
            // own tabs scrolls them behind arrows — which hides a whole editor
            // behind a widget the user has to discover.
            ImGui::DockBuilderSplitNode(dock_rest, ImGuiDir_Right, 0.32f, &dock_right, &dock_rest);
            ImGui::DockBuilderSplitNode(dock_rest, ImGuiDir_Down, 0.24f, &dock_bottom, &dock_center);

            ImGui::DockBuilderDockWindow("Outliner", dock_left_top);
            // "Assets" is the FileSystem browser's window ID (kept so existing
            // layouts survive the upgrade) — see the note at its Begin().
            ImGui::DockBuilderDockWindow("Assets", dock_left_bottom);
            ImGui::DockBuilderDockWindow("World", dock_left_top);
            ImGui::DockBuilderDockWindow("Viewport", dock_center);
            ImGui::DockBuilderDockWindow("Inspector", dock_right);
            ImGui::DockBuilderDockWindow("NFEnvironment", dock_right);
            ImGui::DockBuilderDockWindow("NFTalentTree", dock_right);
            ImGui::DockBuilderDockWindow("NFCameraPanel", dock_right);
            ImGui::DockBuilderDockWindow("NFLighting", dock_right);
            ImGui::DockBuilderDockWindow("Console", dock_bottom);
            ImGui::DockBuilderDockWindow("NFRender", dock_bottom);
            ImGui::DockBuilderDockWindow("NFFlowField", dock_bottom);
            ImGui::DockBuilderDockWindow("NFCableSim", dock_bottom);
                ImGui::DockBuilderFinish(dockspace_id);
                // DockBuilderFinish does NOT mark the ini settings dirty (it
                // only re-docks the windows into the new nodes), so a default
                // layout built here was never written to disk — the persisted
                // layout would only ever exist if the user happened to drag a
                // splitter. Mark it explicitly.
                ImGui::MarkIniSettingsDirty();
            }
        }
    }

    const EditorStatus st = app.status();



    // --- Left: Scene Outliner --------------------------------------------------
    if (ImGui::Begin((AV("outliner") + "###Outliner").c_str())) {
        // Quick-Add Primitives / Entities Menu
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(37, 99, 235, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(59, 130, 246, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(29, 78, 216, 255));
        if (ImGui::Button(AV("outliner_add_entity").c_str())) {
            ImGui::OpenPopup("##outliner_add_popup");
        }
        ImGui::PopStyleColor(3);
        if (ImGui::BeginPopup("##outliner_add_popup")) {
            const ecs::Entity parent = app.selection().has_selection() ? app.selection().primary() : ecs::kInvalidEntity;
            std::string err;
            if (ImGui::MenuItem(AV("cube").c_str())) {
                if (!app.create_primitive(PrimitiveKind::Cube, parent, err)) {
                    push_error(app.console(), "Create cube failed", err);
                }
            }
            if (ImGui::MenuItem(AV("sphere").c_str())) {
                if (!app.create_primitive(PrimitiveKind::Sphere, parent, err)) {
                    push_error(app.console(), "Create sphere failed", err);
                }
            }
            if (ImGui::MenuItem(AV("quad").c_str())) {
                if (!app.create_primitive(PrimitiveKind::Quad, parent, err)) {
                    push_error(app.console(), "Create quad failed", err);
                }
            }
            if (ImGui::MenuItem(AV("ground").c_str())) {
                if (!app.create_primitive(PrimitiveKind::Ground, parent, err)) {
                    push_error(app.console(), "Create ground failed", err);
                }
            }
            if (ImGui::MenuItem(AV("sprite_2d").c_str())) {
                if (!app.create_sprite(parent, err)) {
                    push_error(app.console(), "Create sprite failed", err);
                }
            }
            if (ImGui::MenuItem(AV("tilemap_2d").c_str())) {
                if (!app.create_tilemap(parent, err)) {
                    push_error(app.console(), "Create tilemap failed", err);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(AV("camera").c_str())) {
                if (!app.create_camera(parent, err)) {
                    push_error(app.console(), "Create camera failed", err);
                }
            }
            if (ImGui::MenuItem(AV("light_sun").c_str())) {
                if (!app.create_directional_light(parent, err)) {
                    push_error(app.console(), "Create light failed", err);
                }
            }
            if (ImGui::MenuItem(AV("point_light").c_str())) {
                if (!app.create_point_light(parent, err)) {
                    push_error(app.console(), "Create point light failed", err);
                }
            }
            if (ImGui::MenuItem(AV("spot_light").c_str())) {
                if (!app.create_spot_light(parent, err)) {
                    push_error(app.console(), "Create spot light failed", err);
                }
            }
            if (ImGui::MenuItem(AV("audio_source").c_str())) {
                if (!app.create_audio_emitter(parent, err)) {
                    push_error(app.console(), "Create audio source failed", err);
                }
            }
            if (ImGui::MenuItem(AV("sky").c_str())) {
                if (!app.create_sky_entity(parent, err)) {
                    push_error(app.console(), "Create sky failed", err);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(AV("add_weather").c_str())) {
                if (!app.create_weather(parent, err)) {
                    push_error(app.console(), "Create weather failed", err);
                }
            }
            if (ImGui::MenuItem(AV("add_cable").c_str())) {
                if (!app.create_cable(parent, err)) {
                    push_error(app.console(), "Create cable failed", err);
                }
            }
            if (ImGui::MenuItem(AV("add_rule_tile").c_str())) {
                if (!app.create_rule_tile(parent, err)) {
                    push_error(app.console(), "Create rule tile failed", err);
                }
            }
            if (ImGui::MenuItem(AV("add_flow_field").c_str())) {
                if (!app.create_flow_field(parent, err)) {
                    push_error(app.console(), "Create flow field failed", err);
                }
            }
            if (ImGui::MenuItem(AV("add_talent_tree").c_str())) {
                if (!app.create_talent_tree(parent, err)) {
                    push_error(app.console(), "Create talent tree failed", err);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(AV("create_empty").c_str())) {
                const int n = static_cast<int>(app.status().entity_count);
                if (!app.create_entity(ui::tr("entity_prefix") + " " + std::to_string(n), parent, err)) {
                    push_error(app.console(), "Create entity failed", err);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(36, 110, 78, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(48, 145, 102, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(28, 90, 62, 255));
        const bool btn_create_empty = ImGui::Button(AV("create_empty").c_str());
        ImGui::PopStyleColor(3);
        if (btn_create_empty) {
            std::string err;
            const int n = static_cast<int>(app.status().entity_count);
            // A3b: this string is STORED as the entity's name, so it must be the
            // logical form (ui::tr), not the shaped display form (AV) — the name
            // is shaped again by entity_label_ptr() on the way to the widget.
            // Storing shaped text would double-shape it and corrupt the name.
            if (!app.create_entity(ui::tr("entity_prefix") + " " + std::to_string(n),
                                   ecs::kInvalidEntity, err)) {
                push_error(app.console(), "Create entity failed", err);
            }
        }
        ImGui::SameLine();
        const bool has_sel = app.selection().has_selection();
        if (!has_sel) {
            ImGui::BeginDisabled();
        }
        // A4: immediate, undoable delete. This used to only arm a pending delete
        // and wait for an inline Confirm button below the toolbar — easy to miss
        // entirely, so it read as "Delete does nothing". delete_entity captures a
        // DeleteEntityCommand, so the whole thing is one undo step and Ctrl+Z
        // restores it; that is the safety net instead of a confirm step.
        if (ImGui::Button(AV("delete").c_str())) {
            std::string err;
            if (!app.delete_entity(app.selection().primary(), err)) {
                push_error(app.console(), "Delete failed", err);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(AV("duplicate").c_str())) {
            std::string err;
            if (!app.duplicate_selection(err)) {
                push_error(app.console(), "Duplicate failed", err);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", "Ctrl+D");
        }
        if (!has_sel) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!has_sel) {
            ImGui::BeginDisabled();
        }
        // Short label + tooltip carrying the full name. "Save as prefab" is
        // ~120px at this font, so at the outliner's share of a 1280px window it
        // rendered as "Save as pr" — a clipped button reads as a broken panel,
        // and the full wording is one hover away (and unchanged in the row's
        // context menu, which has the width for it).
        if (ImGui::Button(AV("prefab").c_str())) {
            const ecs::Entity sel = app.selection().primary();
            std::string err;
            const std::string prefab_path =
                std::string("content://Prefabs/") + entity_label_ptr(app, sel) + ".nfscene";
            if (!app.create_prefab(sel, prefab_path, err)) {
                push_error(app.console(), "Save as prefab failed", err);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", AV("save_prefab").c_str());
        }
        if (!has_sel) {
            ImGui::EndDisabled();
        }
        // Live search: filters the cached rows every frame (see
        // EditorApp::outliner_rows), so a match under a collapsed parent is
        // still one glance away.
        char outliner_filter[128]{};
        std::strncpy(outliner_filter, app.outliner().filter_text.c_str(), sizeof(outliner_filter) - 1);
        // The hint lives INSIDE the field. As a trailing label it ate ~60px of a
        // narrow panel to repeat what an empty box already implies, and in the
        // Arabic UI it was the only string in the row that had to be shifted.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputTextWithHint("##outliner_filter", AV("filter").c_str(), outliner_filter,
                                     sizeof(outliner_filter))) {
            app.outliner().filter_text = outliner_filter;
        }

        // 1-Click Feature Quick-Spawn Bar
        {
            auto quick_spawn_pill = [&](const char* label, const char* tip, ImU32 bg, auto action) {
                ImGui::PushStyleColor(ImGuiCol_Button, bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 80, 105, 255));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 2.0f));
                if (ImGui::Button(label)) {
                    action();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tip);
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
            };
            quick_spawn_pill(("+ " + AV("filter_weather")).c_str(), "Create Atmospheric Weather entity", IM_COL32(14, 116, 144, 220), [&]{
                std::string err; app.create_weather(ecs::kInvalidEntity, err);
            });
            ImGui::SameLine(0.0f, 3.0f);
            quick_spawn_pill(("+ " + AV("filter_cable")).c_str(), "Create Cable Simulation entity", IM_COL32(180, 83, 9, 220), [&]{
                std::string err; app.create_cable(ecs::kInvalidEntity, err);
            });
            ImGui::SameLine(0.0f, 3.0f);
            quick_spawn_pill(("+ " + AV("filter_talents")).c_str(), "Create Talent Progression Tree", IM_COL32(190, 24, 93, 220), [&]{
                std::string err; app.create_talent_tree(ecs::kInvalidEntity, err);
            });
            ImGui::SameLine(0.0f, 3.0f);
            quick_spawn_pill(("+ " + AV("filter_nav")).c_str(), "Create Flow Field Navigation Grid", IM_COL32(21, 128, 61, 220), [&]{
                std::string err; app.create_flow_field(ecs::kInvalidEntity, err);
            });
        }

        static int s_outliner_cat = 0;
        {
            auto draw_cat_pill = [&](const char* label, int cat_idx, ImU32 active_color) {
                const bool active = (s_outliner_cat == cat_idx);
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, active_color);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active_color);
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 37, 46, 200));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(48, 56, 70, 255));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 190, 205, 230));
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 2.0f));
                if (ImGui::Button(label)) {
                    s_outliner_cat = (active && cat_idx != 0) ? 0 : cat_idx;
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(3);
            };
            draw_cat_pill(AV("filter_all").c_str(), 0, IM_COL32(59, 130, 246, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill("3D", 1, IM_COL32(79, 140, 255, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill("2D", 2, IM_COL32(16, 185, 129, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill(AV("filter_light").c_str(), 3, IM_COL32(245, 158, 11, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill(AV("filter_audio").c_str(), 4, IM_COL32(168, 85, 247, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill(AV("filter_physics").c_str(), 5, IM_COL32(249, 115, 22, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill(AV("filter_weather").c_str(), 6, IM_COL32(56, 189, 248, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill(AV("filter_cable").c_str(), 7, IM_COL32(251, 146, 60, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill(AV("filter_nav").c_str(), 8, IM_COL32(34, 197, 94, 220));
            ImGui::SameLine(0.0f, 4.0f);
            draw_cat_pill(AV("filter_talents").c_str(), 9, IM_COL32(236, 72, 153, 220));
        }
        // Live census: total entities plus selection size, so an empty-looking
        // tree still answers "is the scene really empty".
        {
            const size_t total = app.status().entity_count;
            const size_t n_sel = app.selection().all().size();
            if (n_sel > 0) {
                ImGui::TextDisabled(
                    "%s  |  %s", AVF("stats_entities", total).c_str(),
                    AVF("selected_count", n_sel).c_str());
            } else {
                ImGui::TextDisabled("%s", AVF("stats_entities", total).c_str());
            }
        }
        std::vector<OutlinerRow> rows = app.outliner_rows();
        if (s_outliner_cat != 0) {
            std::vector<OutlinerRow> filtered;
            for (const OutlinerRow& r : rows) {
                bool keep = false;
                if (s_outliner_cat == 1) keep = (r.kind == OutlinerKind::Mesh);
                else if (s_outliner_cat == 2) keep = (r.kind == OutlinerKind::Sprite2D || r.kind == OutlinerKind::Tilemap || r.kind == OutlinerKind::RuleTile);
                else if (s_outliner_cat == 3) keep = (r.kind == OutlinerKind::Light || r.kind == OutlinerKind::Sky);
                else if (s_outliner_cat == 4) keep = (r.kind == OutlinerKind::Audio || (r.feature_mask & (1 << 7)));
                else if (s_outliner_cat == 5) keep = (r.kind == OutlinerKind::Physics || r.kind == OutlinerKind::Cable || r.kind == OutlinerKind::Ragdoll);
                else if (s_outliner_cat == 6) keep = (r.kind == OutlinerKind::Weather || (r.feature_mask & (1 << 0)));
                else if (s_outliner_cat == 7) keep = (r.kind == OutlinerKind::Cable || (r.feature_mask & (1 << 1)));
                else if (s_outliner_cat == 8) keep = (r.kind == OutlinerKind::FlowField || (r.feature_mask & (1 << 3)));
                else if (s_outliner_cat == 9) keep = (r.kind == OutlinerKind::TalentTree || (r.feature_mask & (1 << 4)));
                if (keep) {
                    OutlinerRow flat = r;
                    flat.depth = 0;
                    filtered.push_back(flat);
                }
            }
            rows = std::move(filtered);
        }
        if (rows.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", AV("empty_scene_hint").c_str());
        }
        // Clipped: a 600-row village must not pay 600 shaped tree nodes per
        // frame. Open state is keyed by stable entity id (###e<id>), never by
        // row index, so clipping never collapses the tree.
        //
        // Icon gutter: every row reserves space for its kind glyph by prefixing
        // plain spaces (measured once, display-only — the ###e<id> suffix keeps
        // widget identity). The glyph is painted into that gutter after the
        // node, so selection, drag & drop and the context menu are untouched.
        const float ol_icon_sz = 13.0f;
        const float ol_space_w = ImGui::CalcTextSize(" ").x;
        int ol_pad_n = (ol_space_w > 0.0f)
                           ? static_cast<int>(std::ceil((ol_icon_sz + 10.0f) / ol_space_w))
                           : 5;
        if (ol_pad_n < 4) {
            ol_pad_n = 4;
        }
        const std::string ol_pad(static_cast<size_t>(ol_pad_n), ' ');
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int clip_i = clipper.DisplayStart; clip_i < clipper.DisplayEnd; ++clip_i) {
                const OutlinerRow& row = rows[static_cast<size_t>(clip_i)];
                ImGui::PushID(static_cast<int>(row.entity.id));
            const bool selected = app.selection().contains(row.entity);
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
            if (selected) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            if (!row.has_children) {
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            }
            if (!app.outliner().is_expanded(row.entity) && row.has_children) {
                ImGui::SetNextItemOpen(false, ImGuiCond_Always);
            }
            // Display text is shaped (an Arabic entity name must read
            // right-to-left); the ID suffix stays the numeric entity, so
            // renaming an entity or switching language never collapses the
            // tree — the label text is not part of the widget identity.
            // The leading spaces are the icon gutter (see above).
            const std::string row_text =
                ol_pad + ui::shape_arabic(row.is_prefab ? ("[Prefab] " + row.label) : row.label);
            const std::string row_id = "###e" + std::to_string(row.entity.id);
            const ImVec4 kind_col = outliner_kind_color(row.kind);
            const bool tinted = (kind_col.w > 0.0f);
            if (tinted) {
                ImGui::PushStyleColor(ImGuiCol_Text, kind_col);
            }
            const bool open = ImGui::TreeNodeEx((row_text + row_id).c_str(), flags);
            if (tinted) {
                ImGui::PopStyleColor();
            }
            // Paint the kind glyph into the gutter: label starts at the tree
            // node's label spacing, the gutter is the reserved prefix.
            {
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                const float label_x = rmin.x + ImGui::GetTreeNodeToLabelSpacing();
                const float ix = label_x + 2.0f;
                const float iy = (rmin.y + rmax.y - ol_icon_sz) * 0.5f;
                const ImVec2 ic(ix + ol_icon_sz * 0.5f, iy + ol_icon_sz * 0.5f);
                if (row.kind == OutlinerKind::Mesh && outliner_is_ground_label(row.label)) {
                    outliner_plane_icon(ImGui::GetWindowDrawList(), ic, ol_icon_sz);
                } else if (outliner_is_audio_label(row.label)) {
                    outliner_audio_icon(ImGui::GetWindowDrawList(), ic, ol_icon_sz);
                } else {
                    outliner_kind_icon(ImGui::GetWindowDrawList(), ic, ol_icon_sz,
                                       row.kind);
                }

                // Feature badge chips on the right side of the row
                if (row.feature_mask != 0) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    float bx = rmax.x - 6.0f;
                    const float by = (rmin.y + rmax.y) * 0.5f;
                    auto draw_chip = [&](const char* tag, ImU32 bg, ImU32 fg) {
                        const std::string shaped = ui::shape_arabic(tag);
                        const ImVec2 tsz = ImGui::CalcTextSize(shaped.c_str());
                        const float cw = tsz.x + 8.0f;
                        const float ch = 14.0f;
                        bx -= cw;
                        dl->AddRectFilled(ImVec2(bx, by - ch * 0.5f), ImVec2(bx + cw, by + ch * 0.5f), bg, 3.0f);
                        dl->AddText(ImVec2(bx + 4.0f, by - tsz.y * 0.5f), fg, shaped.c_str());
                        bx -= 4.0f;
                    };
                    if (row.feature_mask & (1 << 0)) draw_chip(AV("filter_weather").c_str(), IM_COL32(14, 116, 144, 200), IM_COL32(224, 242, 254, 255));
                    if (row.feature_mask & (1 << 1)) draw_chip(AV("filter_cable").c_str(), IM_COL32(180, 83, 9, 200), IM_COL32(254, 243, 199, 255));
                    if (row.feature_mask & (1 << 2)) draw_chip(AV("rule_tile").c_str(), IM_COL32(4, 120, 87, 200), IM_COL32(209, 250, 229, 255));
                    if (row.feature_mask & (1 << 3)) draw_chip(AV("filter_nav").c_str(), IM_COL32(21, 128, 61, 200), IM_COL32(220, 252, 231, 255));
                    if (row.feature_mask & (1 << 4)) draw_chip(AV("filter_talents").c_str(), IM_COL32(190, 24, 93, 200), IM_COL32(253, 231, 243, 255));
                    if (row.feature_mask & (1 << 5)) draw_chip(AV("inventory").c_str(), IM_COL32(109, 40, 217, 200), IM_COL32(237, 233, 254, 255));
                    if (row.feature_mask & (1 << 6)) draw_chip(AV("ragdoll").c_str(), IM_COL32(185, 28, 28, 200), IM_COL32(254, 226, 226, 255));
                    if (row.feature_mask & (1 << 7)) draw_chip(AV("surface_audio").c_str(), IM_COL32(146, 64, 14, 200), IM_COL32(254, 243, 199, 255));
                }
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                const ImGuiIO& io = ImGui::GetIO();
                // Ctrl OR Shift toggles (Shift matches the viewport gesture).
                if (io.KeyCtrl || io.KeyShift) {
                    if (selected) {
                        app.selection().remove(row.entity);
                    } else {
                        app.selection().add(row.entity);
                    }
                } else {
                    app.selection().set_single(row.entity);
                }
            }
            // Drag to reparent; drop target accepts entity payloads.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("NF_ENTITY", &row.entity, sizeof(ecs::Entity));
                ImGui::TextUnformatted(ui::shape_arabic(row.label).c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NF_ENTITY")) {
                    ecs::Entity child;
                    std::memcpy(&child, pl->Data, sizeof(ecs::Entity));
                    std::string err;
                    if (!app.reparent(child, row.entity, err)) {
                        push_error(app.console(), "Reparent failed", err);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            // Right-click menu: the same Delete / Save-as-prefab the toolbar
            // buttons offer, where the pointer already is. Delete is undoable
            // (Ctrl+Z restores), matching the button's safety contract.
            if (ImGui::BeginPopupContextItem("##rowctx")) {
                if (ImGui::MenuItem((AV("frame_selection") + "   F").c_str())) {
                    app.selection().set_single(row.entity);
                    intents.nav_frame = 1;
                }
                if (ImGui::MenuItem(AV("create_child").c_str())) {
                    std::string err;
                    const int n = static_cast<int>(app.status().entity_count);
                    if (!app.create_entity(ui::tr("entity_prefix") + " " + std::to_string(n), row.entity, err)) {
                        push_error(app.console(), "Create child failed", err);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem((AV("duplicate") + "   Ctrl+D").c_str())) {
                    std::string err;
                    if (!app.duplicate_selection(err)) {
                        push_error(app.console(), "Duplicate failed", err);
                    }
                }
                if (ImGui::MenuItem(AV("save_prefab").c_str())) {
                    std::string err;
                    const std::string prefab_path =
                        std::string("content://Prefabs/") + row.label + ".nfscene";
                    if (!app.create_prefab(row.entity, prefab_path, err)) {
                        push_error(app.console(), "Save as prefab failed", err);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(AV("delete").c_str())) {
                    std::string err;
                    if (!app.delete_entity(row.entity, err)) {
                        push_error(app.console(), "Delete failed", err);
                    }
                }
                ImGui::EndPopup();
            }
            if (open && row.has_children) {
                ImGui::TreePop();
            }
            ImGui::PopID();
            }
        }
        clipper.End();
        // Drop on empty outliner space = unparent to root.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NF_ENTITY")) {
                ecs::Entity child;
                std::memcpy(&child, pl->Data, sizeof(ecs::Entity));
                std::string err;
                if (!app.reparent(child, ecs::kInvalidEntity, err)) {
                    push_error(app.console(), "Reparent failed", err);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextWindow("##outliner_empty_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem(AV("create_empty").c_str())) {
                std::string err;
                const int n = static_cast<int>(app.status().entity_count);
                if (!app.create_entity(ui::tr("entity_prefix") + " " + std::to_string(n), ecs::kInvalidEntity, err)) {
                    push_error(app.console(), "Create entity failed", err);
                }
            }
            if (ImGui::BeginMenu(AV("primitive").c_str())) {
                if (ImGui::MenuItem(AV("add_cube").c_str())) {
                    std::string err;
                    app.create_primitive(PrimitiveKind::Cube, ecs::kInvalidEntity, err);
                }
                if (ImGui::MenuItem(AV("add_sphere").c_str())) {
                    std::string err;
                    app.create_primitive(PrimitiveKind::Sphere, ecs::kInvalidEntity, err);
                }
                if (ImGui::MenuItem(AV("add_quad").c_str())) {
                    std::string err;
                    app.create_primitive(PrimitiveKind::Quad, ecs::kInvalidEntity, err);
                }
                if (ImGui::MenuItem(AV("add_ground").c_str())) {
                    std::string err;
                    app.create_primitive(PrimitiveKind::Ground, ecs::kInvalidEntity, err);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(AV("light").c_str())) {
                if (ImGui::MenuItem(AV("light_sun").c_str())) {
                    std::string err;
                    app.create_directional_light(ecs::kInvalidEntity, err);
                }
                if (ImGui::MenuItem(AV("point_light").c_str())) {
                    std::string err;
                    app.create_point_light(ecs::kInvalidEntity, err);
                }
                if (ImGui::MenuItem(AV("spot_light").c_str())) {
                    std::string err;
                    app.create_spot_light(ecs::kInvalidEntity, err);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(AV("sprite_2d").c_str())) {
                std::string err;
                app.create_sprite(ecs::kInvalidEntity, err);
            }
            if (ImGui::MenuItem(AV("tilemap_2d").c_str())) {
                std::string err;
                app.create_tilemap(ecs::kInvalidEntity, err);
            }
            if (ImGui::MenuItem(AV("audio_source").c_str())) {
                std::string err;
                app.create_audio_emitter(ecs::kInvalidEntity, err);
            }
            if (ImGui::MenuItem(AV("add_camera").c_str())) {
                std::string err;
                app.create_camera(ecs::kInvalidEntity, err);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(AV("add_weather").c_str())) {
                std::string err;
                app.create_weather(ecs::kInvalidEntity, err);
            }
            if (ImGui::MenuItem(AV("add_cable").c_str())) {
                std::string err;
                app.create_cable(ecs::kInvalidEntity, err);
            }
            if (ImGui::MenuItem(AV("add_rule_tile").c_str())) {
                std::string err;
                app.create_rule_tile(ecs::kInvalidEntity, err);
            }
            if (ImGui::MenuItem(AV("add_flow_field").c_str())) {
                std::string err;
                app.create_flow_field(ecs::kInvalidEntity, err);
            }
            if (ImGui::MenuItem(AV("add_talent_tree").c_str())) {
                std::string err;
                app.create_talent_tree(ecs::kInvalidEntity, err);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    // --- Center: Viewport ------------------------------------------------------
    if (ImGui::Begin((AV("viewport") + "###Viewport").c_str())) {
        // Game view banner: while playing the viewport IS the game — same
        // offscreen target, but driven by the game camera and game input, not
        // by editor navigation/gizmos. The banner + Stop button is the way
        // back to editing (Esc works too), so Play never looks like a dead
        // button that changed nothing.
        if (app.playing()) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button((AV("stop") + " (Esc)").c_str())) {
                std::string stop_err;
                if (!app.stop(stop_err)) {
                    push_error(app.console(), "Stop failed", stop_err);
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "● GAME RUNNING — Arrows/WASD move, Space jumps, Shift sprints");
            ImGui::Separator();
        }
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float avail_h = ImGui::GetContentRegionAvail().y;
        // --- Gizmo toolbar (P2): mode / space / grid snapping ---
        // W/E/R switch the mode, but only when the viewport window is focused
        // and nothing is capturing text input — otherwise the console filter
        // and the rename field would steal every keystroke.
        {
            const bool can_hotkey = !ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused();
            if (can_hotkey) {
                if (ImGui::IsKeyPressed(ImGuiKey_W)) {
                    app.set_gizmo_mode(GizmoMode::Translate);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_E)) {
                    app.set_gizmo_mode(GizmoMode::Rotate);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                    app.set_gizmo_mode(GizmoMode::Scale);
                }
            }
        }
        // Active mode is a held accent button; "###gizmo_*" keeps widget IDs
        // stable so flipping the mode never re-arms hover.
        const ImVec4 kActiveBtn(0.298f, 0.553f, 1.000f, 0.85f); // matches UiBackend accent
        const GizmoMode cur_mode = app.gizmo_mode();
        auto mode_button = [&](GizmoMode m, const char* label) {
            const bool on = (cur_mode == m);
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, kActiveBtn);
            }
            if (ImGui::Button(label)) {
                app.set_gizmo_mode(m);
            }
            if (on) {
                ImGui::PopStyleColor();
            }
        };
        // 2D / 3D Dimension Mode Switcher
        const bool is_2d_mode = app.is_2d_mode();
        if (is_2d_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 185, 129, 220)); // Emerald
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(52, 211, 153, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(5, 150, 105, 255));
        }
        if (ImGui::Button("2D###vp_mode_2d")) {
            app.set_2d_mode(true);
            app.set_gizmo_mode(GizmoMode::Translate);
        }
        if (is_2d_mode) {
            ImGui::PopStyleColor(3);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ui::tr("view_mode_2d").c_str());
        }
        ImGui::SameLine();
        if (!is_2d_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(37, 99, 235, 220)); // Electric blue
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(96, 165, 250, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(29, 78, 216, 255));
        }
        if (ImGui::Button("3D###vp_mode_3d")) {
            app.set_2d_mode(false);
        }
        if (!is_2d_mode) {
            ImGui::PopStyleColor(3);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ui::tr("view_mode_3d").c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        // Gizmo labels translate (AV) with stable ###ids so the widget identity
        // survives a language switch; the temporaries outlive the Button call.
        mode_button(GizmoMode::Translate, (AV("move") + "###gizmo_move").c_str());
        ImGui::SameLine();
        mode_button(GizmoMode::Rotate, (AV("rotate") + "###gizmo_rotate").c_str());
        ImGui::SameLine();
        mode_button(GizmoMode::Scale, (AV("scale_tool") + "###gizmo_scale").c_str());
        ImGui::SameLine();
        // Space toggle: the label is the space a click switches TO (the hint
        // in parens names the active one).
        const bool local_space = app.gizmo_space() == GizmoSpace::Local;
        // A3b: the button shows the space you would switch TO. The ID after
        // ### stays English so the widget identity survives a language switch.
        const std::string space_label = local_space ? AV("world_space") : AV("local_space");
        if (ImGui::Button((space_label + "###gizmo_space").c_str())) {
            app.set_gizmo_space(local_space ? GizmoSpace::World : GizmoSpace::Local);
        }
        if (ImGui::IsItemHovered()) {
            // Built, not formatted: a %s placeholder inside a SHAPED Arabic
            // string would be reordered by the shaper along with the text.
            ImGui::SetTooltip("%s: %s", AV("transform_space").c_str(),
                              (local_space ? AV("local_space") : AV("world_space")).c_str());
        }
        ImGui::SameLine();
        // Frame the selection / frame everything. Two buttons rather than one
        // with a modifier: they are both common, and a modifier-only variant is
        // undiscoverable. Disabled (greyed, not hidden) when there is nothing
        // selected, so the toolbar does not reflow as the selection changes.
        ImGui::BeginDisabled(!app.selection().has_selection() || app.playing());
        if (ImGui::Button((AV("frame_selection") + "###frame_sel").c_str())) {
            intents.nav_frame = 1;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", AV("frame_selection_hint").c_str());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button((AV("frame_all") + "###frame_all").c_str())) {
            intents.nav_frame = 2;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", AV("frame_all_hint").c_str());
        }
        ImGui::SameLine();
        // Grid snapping (P2). A step of 0 disables its channel, so the
        // checkbox is "any channel on"; enabling with all steps at zero seeds
        // usable defaults rather than a checkbox that snaps to nothing.
        GizmoSnap& snap = app.gizmo_snap();
        bool snap_on = snap.any();
        if (ImGui::Checkbox((AV("snap") + "###gizmo_snap").c_str(), &snap_on)) {
            if (snap_on && !snap.any()) {
                snap = GizmoSnap{0.25f, 15.0f, 0.25f};
            } else if (!snap_on) {
                snap = GizmoSnap{};
            }
        }
        if (snap_on) {
            ImGui::SameLine();
            ImGui::PushItemWidth(72.0f);
            ImGui::DragFloat((AV("move") + "###snap_move").c_str(), &snap.translate_step, 0.05f, 0.0f, 64.0f,
                             "%.2f");
            ImGui::SameLine();
            ImGui::DragFloat((AV("rotate") + "###snap_rot").c_str(), &snap.rotate_step_deg, 1.0f, 0.0f, 180.0f,
                             "%.0f");
            ImGui::SameLine();
            ImGui::DragFloat((AV("scale") + "###snap_scale").c_str(), &snap.scale_step, 0.05f, 0.0f, 4.0f, "%.2f");
            ImGui::PopItemWidth();
        }
        // Lit-pixel counter: a renderer diagnostic, English on purpose (same
        // decision as the profiler block below). It sits behind the same
        // View > Viewport stats toggle as the census card — a number nobody
        // asked for does not belong in the default viewport header.
        if (stats.viewport_lit != 0 && ui_settings().show_viewport_stats) {
            ImGui::SameLine();
            ImGui::Text("viewport lit=%u", static_cast<unsigned>(stats.viewport_lit));
        }
        // Live viewport image: the offscreen Runtime target, sampled through
        // UiRenderer::kViewportTextureId (bound by the shell every frame).
        // Pixels are proven by render_offscreen plus periodic readback.
        // Flipped V: standard Vulkan puts NDC +Y in memory-BOTTOM rows while
        // ImGui samples UV (0,0) at the widget top — default UVs would show
        // the scene upside down (and mirror every NDC gesture vertically
        // while the centre still hits, which is exactly how this survived).
        ImVec2 img_size(avail_w > 0.0f ? avail_w : 10.0f, avail_h - 24.0f > 0.0f ? avail_h - 24.0f : 10.0f);
        // Feed the displayed size back: the shell renders the offscreen
        // target at the panel size, never the window size — otherwise the
        // image stretches whenever the docked panel aspect differs from
        // the window aspect. One frame of lag on resize is invisible.
        if (img_size.x >= 1.0f && img_size.y >= 1.0f) {
            app.viewport().width = static_cast<uint32_t>(img_size.x);
            app.viewport().height = static_cast<uint32_t>(img_size.y);
        }
        // Display convention (see render_memory_rows_follow_vulkan_top_left_origin):
        // memory row 0 is the image TOP and world +Y renders into low-numbered
        // rows, so default UVs present the frame upright. The flipped V that
        // used to sit here dated from when the projection was OpenGL-style and
        // the render itself came out upside down (Sep 14); the Sep 18 Y-flip
        // fix (negated m[1][1]) turned that correction into a double flip —
        // upside-down view with mirrored gizmo drags.
        ImGui::Image(static_cast<ImTextureID>(UiRenderer::kViewportTextureId), img_size);
        const ImVec2 vp_img_min = ImGui::GetItemRectMin();
        const ImVec2 vp_img_max = ImGui::GetItemRectMax();
        const bool vp_img_hovered = ImGui::IsItemHovered();

        // Ground grid over the image (drawn, not composited): world grid lines
        // projected through the camera the frame was rendered with, so the grid
        // sits in the scene rather than on the screen. Without it the viewport is
        // a gradient with no sense of scale or where the origin is.
        rendering::Camera vp_cam{};
        const bool have_cam =
            app.runtime() != nullptr &&
            app.runtime()->extract_camera(static_cast<uint32_t>(std::max(1.0f, img_size.x)),
                                          static_cast<uint32_t>(std::max(1.0f, img_size.y)),
                                          vp_cam);
        if (ui_settings().show_grid && have_cam) {
            const ImVec2 mn = vp_img_min;
            const ImVec2 mx = vp_img_max;
            if (app.is_2d_mode()) {
                draw_2d_grid(ImGui::GetWindowDrawList(), mn, mx, vp_cam.view_projection,
                             ui_settings().grid_step, kGridHalfExtent, app.view_pivot()[0],
                             app.view_pivot()[1]);
            } else {
                draw_ground_grid(ImGui::GetWindowDrawList(), mn, mx, vp_cam.view_projection,
                                 ui_settings().grid_step, kGridHalfExtent, app.view_pivot()[0],
                                 app.view_pivot()[2]);
            }
        }
        if (ui_settings().show_navmesh && have_cam && app.runtime() != nullptr) {
            const ImVec2 mn = vp_img_min;
            const ImVec2 mx = vp_img_max;
            draw_navmesh_overlay(ImGui::GetWindowDrawList(), mn, mx, vp_cam.view_projection,
                                 app.runtime()->nav_debug_snapshot());
        }
        GizmoHandle gizmo_hover = GizmoHandle::None;
        if (have_cam) {
            draw_orientation_gizmo(ImGui::GetWindowDrawList(), vp_img_max, vp_cam);
            gizmo_hover = draw_transform_gizmo(app, vp_cam, vp_img_min, vp_img_max);
        }
        // Interactive floating HUD overlay over the viewport image
        {
            ImGui::SetNextWindowPos(ImVec2(vp_img_min.x + 10.0f, vp_img_min.y + 10.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.80f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 0.0f));
            if (ImGui::Begin("##viewport_hud_overlay", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
                // 1. Dimension mode [2D] / [3D]
                const bool is_2d = app.is_2d_mode();
                if (is_2d) {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 185, 129, 220));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(52, 211, 153, 255));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(37, 99, 235, 220));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(96, 165, 250, 255));
                }
                if (ImGui::Button(is_2d ? "[ 2D ]###hud_dim" : "[ 3D ]###hud_dim")) {
                    app.set_2d_mode(!is_2d);
                    if (!is_2d) {
                        app.set_gizmo_mode(GizmoMode::Translate);
                    }
                }
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", is_2d ? ui::tr("view_mode_2d").c_str() : ui::tr("view_mode_3d").c_str());
                }

                ImGui::SameLine();
                // 2. Coordinate space switch [عالمي] / [محلي]
                const bool hud_local_space = app.gizmo_space() == GizmoSpace::Local;
                const std::string space_str = hud_local_space ? AV("local_space") : AV("world_space");
                if (ImGui::Button((space_str + "###hud_space").c_str())) {
                    app.set_gizmo_space(hud_local_space ? GizmoSpace::World : GizmoSpace::Local);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", (hud_local_space ? AV("local_space") : AV("world_space")).c_str());
                }

                ImGui::SameLine();
                // 3. Grid toggle [⊞ شبكة]
                bool& grid_on = ui_settings().show_grid;
                if (grid_on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(59, 130, 246, 200));
                }
                if (ImGui::Button((AV("grid") + "###hud_grid").c_str())) {
                    grid_on = !grid_on;
                }
                if (grid_on) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", AV("grid").c_str());
                }

                ImGui::SameLine();
                // 4. Snap toggle [🧲 مغناطيس]
                GizmoSnap& hud_snap = app.gizmo_snap();
                bool hud_snap_on = hud_snap.any();
                if (hud_snap_on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(234, 88, 12, 200));
                }
                if (ImGui::Button((AV("snap") + "###hud_snap").c_str())) {
                    if (!hud_snap_on) {
                        hud_snap = GizmoSnap{0.25f, 15.0f, 0.25f};
                    } else {
                        hud_snap = GizmoSnap{};
                    }
                }
                if (hud_snap_on) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", AV("snap").c_str());
                }

                ImGui::SameLine();
                // 5. Frame button [F]
                ImGui::BeginDisabled(!app.selection().has_selection() || app.playing());
                if (ImGui::Button("F###hud_frame")) {
                    intents.nav_frame = 1;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", AV("frame_selection_hint").c_str());
                }
                ImGui::EndDisabled();
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
        }

        // Live Atmospheric Weather & Systems HUD
        {
            const runtime::WeatherComponent* scene_weather = nullptr;
            int cable_count = 0;
            if (app.world() != nullptr) {
                for (ecs::Entity e : app.world()->all_entities()) {
                    if (const auto* wc = app.world()->get<runtime::WeatherComponent>(e)) {
                        scene_weather = wc;
                    }
                    if (app.world()->has<runtime::CableComponent>(e)) {
                        cable_count++;
                    }
                }
            }
            if (scene_weather != nullptr || cable_count > 0) {
                const ImVec2 img_mn = vp_img_min;
                ImGui::SetNextWindowPos(ImVec2(img_mn.x + 10.0f, img_mn.y + 10.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.78f);
                if (ImGui::Begin("##viewport_weather_hud", nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
                    if (scene_weather != nullptr) {
                        const std::string preset_names[7] = {
                            AV("weather_clear"), AV("weather_cloudy"),
                            AV("weather_overcast"), AV("weather_rain"),
                            AV("weather_thunderstorm"), AV("weather_foggy"),
                            AV("weather_snow")
                        };
                        const int p_idx = (scene_weather->weather_type >= 0 && scene_weather->weather_type < 7) ? scene_weather->weather_type : 0;
                        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s: %s", AV("weather_hud").c_str(), preset_names[p_idx].c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled("| %s: %.3f | %s: %.1f m/s", AV("fog_density").c_str(), scene_weather->fog_density, AV("wind_speed").c_str(), scene_weather->wind_strength);
                    }
                    if (cable_count > 0) {
                        if (scene_weather != nullptr) ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.30f, 1.0f), "| %s: %d", AV("cable_sim").c_str(), cable_count);
                    }
                    ImGui::End();
                }
            }
        }

        // Live simulation overlay: what the frame actually stepped, for the
        // "is anything running" glance. Positioned below the floating HUD.
        if (app.runtime() != nullptr && ui_settings().show_viewport_stats) {
            runtime::Runtime* rt = app.runtime();
            const ImVec2 img_mn = vp_img_min;
            ImGui::SetNextWindowPos(ImVec2(img_mn.x + 10.0f, img_mn.y + 48.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.6f);
            if (ImGui::Begin("##viewport_stats", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
                const EditorStatus status = app.status();
                ImGui::TextDisabled("%s", AVF("stats_entities", status.entity_count).c_str());
                ImGui::TextDisabled("%u x %u", app.viewport().width, app.viewport().height);
                ImGui::TextDisabled(
                    "%s",
                    AVF("stats_scripts", static_cast<int>(rt->scripts_runnable_count()),
                        static_cast<int>(rt->script_count()))
                        .c_str());
                ImGui::TextDisabled("%s",
                                    AVF("stats_particles", rt->particles_alive(),
                                        rt->particle_emitter_count())
                                        .c_str());
                ImGui::TextDisabled("%s", AVF("stats_cloths", rt->cloth_count()).c_str());
                ImGui::TextDisabled("%s", AVF("stats_characters", rt->character_count()).c_str());
                ImGui::TextDisabled("%s", AVF("stats_vehicles", rt->vehicle_count()).c_str());
                if (const ai::AIWorld* ai = rt->ai_world()) {
                    ImGui::TextDisabled("%s", AVF("stats_ai", ai->actors().size()).c_str());
                }
                // Navigation, beside the AI line: what the bake produced and
                // whether the walkers are arriving. Reads the bake, not the
                // components — a count of components cannot tell an unbaked
                // volume from a working one (see the inspector's live stats).
                ImGui::TextDisabled(
                    "%s",
                    AVF("stats_navmesh", rt->navmesh_polygon_count(), rt->nav_agents_at_goal(),
                        rt->nav_agent_count())
                        .c_str());
            }
            ImGui::End();
        }
        // Pointer gesture state machine (one viewport, so statics are fine):
        // press on the image arms a gizmo drag in the app, movement feeds it,
        // release folds one undoable command. NDC is recomputed from the live
        // mouse position every frame (the panel can resize mid-gesture).
        {
            const ImVec2 mn = vp_img_min;
            const ImVec2 mx = vp_img_max;
            const float w = mx.x - mn.x;
            const float h = mx.y - mn.y;
            auto to_ndc = [&](float& out_x, float& out_y) {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                float fx = (w > 1.0f) ? (mp.x - mn.x) / w : 0.0f;
                float fy = (h > 1.0f) ? (mp.y - mn.y) / h : 0.0f;
                if (fx < 0.0f) fx = 0.0f;
                if (fx > 1.0f) fx = 1.0f;
                if (fy < 0.0f) fy = 0.0f;
                if (fy > 1.0f) fy = 1.0f;
                out_x = fx * 2.0f - 1.0f;
                out_y = 1.0f - fy * 2.0f;
            };
            static bool press_armed = false;
            static float last_ndc_x = 0.0f;
            static float last_ndc_y = 0.0f;
            // While playing, the game owns the left button too (no gizmo
            // drags, no selection changes): structural edits are locked and
            // a drag would only fight the simulation.
            // A click on a gizmo handle owns the click instead: constrained
            // drag on the current selection, no re-pick.
            if (!app.playing() && !press_armed && vp_img_hovered &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (w > 1.0f && h > 1.0f) {
                    press_armed = true;
                    if (gizmo_hover != GizmoHandle::None) {
                        intents.viewport_gizmo_press = true;
                        intents.gizmo_handle = static_cast<int>(gizmo_hover);
                    } else {
                        intents.viewport_press = true;
                        // Ctrl/Shift at press time toggles the hit entity into the
                        // selection (group drag) instead of replacing it.
                        const ImGuiIO& press_io = ImGui::GetIO();
                        intents.viewport_press_additive =
                            press_io.KeyShift || press_io.KeyCtrl;
                    }
                    to_ndc(intents.press_ndc_x, intents.press_ndc_y);
                    last_ndc_x = intents.press_ndc_x;
                    last_ndc_y = intents.press_ndc_y;
                }
            } else if (press_armed) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float nx = 0.0f, ny = 0.0f;
                    to_ndc(nx, ny);
                    if (nx != last_ndc_x || ny != last_ndc_y) {
                        intents.viewport_drag = true;
                        intents.drag_ndc_x = nx;
                        intents.drag_ndc_y = ny;
                        last_ndc_x = nx;
                        last_ndc_y = ny;
                    }
                } else {
                    intents.viewport_release = true;
                    press_armed = false;
                }
            }
            // Viewport navigation (right button held on the image): orbit look
            // + WASD/QE fly + wheel zoom. Separate from the left-button gizmo
            // gesture above — right never selects, left never navigates.
            // Disabled while playing: the game camera is gameplay-owned and
            // editor orbiting would snap it every frame.
            if (!app.playing()) {
            {
                const ImGuiIO& nav_io = ImGui::GetIO();
                const bool over_view = vp_img_hovered;
                static bool nav_held = false;
                if (!nav_held && over_view && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    nav_held = true;
                } else if (nav_held && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    nav_held = false;
                }
                if (nav_held) {
                    const ImVec2 md = nav_io.MouseDelta;
                    if (md.x != 0.0f || md.y != 0.0f) {
                        intents.nav_orbit = true;
                        intents.nav_dx += md.x;
                        intents.nav_dy += md.y;
                    }
                }
                if (over_view && nav_io.MouseWheel != 0.0f) {
                    intents.nav_wheel += nav_io.MouseWheel;
                }
                // Fly keys only while navigating: plain WASD stays gizmo
                // shortcuts (main.cpp), and typing anywhere (WantTextInput)
                // never flies the camera.
                if (nav_held && !nav_io.WantTextInput) {
                    intents.nav_f = ImGui::IsKeyDown(ImGuiKey_W);
                    intents.nav_b = ImGui::IsKeyDown(ImGuiKey_S);
                    intents.nav_l = ImGui::IsKeyDown(ImGuiKey_A);
                    intents.nav_r = ImGui::IsKeyDown(ImGuiKey_D);
                    intents.nav_u = ImGui::IsKeyDown(ImGuiKey_E);
                    intents.nav_d = ImGui::IsKeyDown(ImGuiKey_Q);
                }
                // Home = frame the selection, Shift+Home = frame the scene.
                // Home is free in the viewport's key map (F is dolly-forward,
                // B is dolly-back, A/D/E/Q are the fly keys), and a bare letter
                // would collide with the gizmo shortcuts. Typed text and an
                // open rename field must never trigger it.
                if (over_view && !nav_io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
                        intents.nav_frame = nav_io.KeyShift ? 2 : 1;
                    }
                }
            }
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NF_MESH")) {
                intents.viewport_drop = true;
                intents.dropped_mesh_path.assign(static_cast<const char*>(pl->Data), pl->DataSize);
            }
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NF_PREFAB")) {
                const std::string prefab_path(static_cast<const char*>(pl->Data), pl->DataSize);
                std::string err;
                if (!app.instantiate_prefab(prefab_path, ecs::kInvalidEntity, err)) {
                    push_error(app.console(), "Instantiate prefab failed", err);
                } else {
                    push_info(app.console(), "Instantiated prefab: " + prefab_path);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (!app.selection().has_selection()) {
            ImGui::TextDisabled("%s", AV("viewport_hint_none").c_str());
        } else if (!app.viewport_dragging()) {
            const size_t n = app.selection().all().size();
            if (n > 1) {
                ImGui::TextDisabled(AV("viewport_hint_multi").c_str(), n);
            } else {
                ImGui::TextDisabled("%s", AV("viewport_hint_drag").c_str());
                ImGui::TextDisabled(AV("navigate_hint").c_str());
            }
        } else {
            const size_t n = app.selection().all().size();
            // English plural suffix only: the Arabic format needs no suffix,
            // so a Latin "s" must never reach the Arabic UI. Local (not a
            // literal on the widget line) so the literal gate stays clean.
            const char* plural = (n == 1 || rt_active()) ? "" : "s";
            ImGui::TextDisabled(AV("viewport_hint_dragging").c_str(), n, plural);
        }
    }
    ImGui::End();

    // --- Right: Inspector ------------------------------------------------------
    if (ImGui::Begin((AV("inspector") + "###Inspector").c_str())) {
        ImGui::PushItemWidth(-135.0f);
        ecs::World* w = app.world();
        const ecs::Entity sel = app.selection().primary();
        if (w == nullptr || !sel.valid() || !w->is_alive(sel)) {
            // Dashboard header
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s", AV("scene_hub_title").c_str());
            ImGui::TextDisabled("Scene Environment & Quick Setup");

            const std::string scene_name = app.scene_path().empty() ? AV("untitled_scene") : std::filesystem::path(app.scene_path()).filename().string();
            ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "%s", scene_name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", AVF("stats_entities", app.status().entity_count).c_str());
            ImGui::Separator();

            // 1. Quick Atmospheric Weather Controller
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s", AV("quick_weather_title").c_str());
            ImGui::TextDisabled("%s", AV("quick_weather_hint").c_str());

            static int quick_weather_preset = 0;
            static float quick_fog_density = 0.02f;
            static float quick_wetness = 0.0f;
            static float quick_wind_spd = 5.0f;

            runtime::WeatherComponent* current_wc = nullptr;
            if (w != nullptr) {
                for (ecs::Entity e : w->all_entities()) {
                    if (auto* wc = w->get<runtime::WeatherComponent>(e)) {
                        current_wc = wc;
                        quick_weather_preset = wc->weather_type;
                        quick_fog_density = wc->fog_density;
                        quick_wetness = wc->ground_wetness;
                        quick_wind_spd = wc->wind_strength;
                        break;
                    }
                }
            }

            auto weather_pill = [&](const char* name, int preset, ImU32 col) {
                const bool active = (quick_weather_preset == preset);
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, col);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(38, 44, 56, 200));
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                if (ImGui::Button(name)) {
                    quick_weather_preset = preset;
                    if (current_wc != nullptr) {
                        current_wc->weather_type = preset;
                    } else {
                        std::string err;
                        app.create_weather(ecs::kInvalidEntity, err);
                        if (app.world() != nullptr && app.selection().has_selection()) {
                            if (auto* nw = app.world()->get<runtime::WeatherComponent>(app.selection().primary())) {
                                nw->weather_type = preset;
                            }
                        }
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            };

            weather_pill(AV("weather_clear").c_str(), 0, IM_COL32(234, 179, 8, 255));
            ImGui::SameLine(0.0f, 4.0f);
            weather_pill(AV("weather_cloudy").c_str(), 1, IM_COL32(59, 130, 246, 255));
            ImGui::SameLine(0.0f, 4.0f);
            weather_pill(AV("weather_overcast").c_str(), 2, IM_COL32(100, 116, 139, 255));
            ImGui::SameLine(0.0f, 4.0f);
            weather_pill(AV("weather_rain").c_str(), 3, IM_COL32(6, 182, 212, 255));

            weather_pill(AV("weather_thunderstorm").c_str(), 4, IM_COL32(139, 92, 246, 255));
            ImGui::SameLine(0.0f, 4.0f);
            weather_pill(AV("weather_foggy").c_str(), 5, IM_COL32(148, 163, 184, 255));
            ImGui::SameLine(0.0f, 4.0f);
            weather_pill(AV("weather_snow").c_str(), 6, IM_COL32(224, 231, 255, 255));

            if (ImGui::SliderFloat((AV("fog_density") + "##qfd").c_str(), &quick_fog_density, 0.0f, 0.1f, "%.4f")) {
                if (current_wc != nullptr) current_wc->fog_density = quick_fog_density;
            }
            if (ImGui::SliderFloat((AV("ground_wetness") + "##qgw").c_str(), &quick_wetness, 0.0f, 1.0f, "%.2f")) {
                if (current_wc != nullptr) current_wc->ground_wetness = quick_wetness;
            }
            if (ImGui::SliderFloat((AV("wind_speed") + "##qws").c_str(), &quick_wind_spd, 0.0f, 50.0f, "%.1f m/s")) {
                if (current_wc != nullptr) current_wc->wind_strength = quick_wind_spd;
            }

            ImGui::Spacing();
            ImGui::Separator();

            // 2. Quick Create Advanced Systems
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.60f, 1.0f), "%s", AV("quick_create_title").c_str());

            auto quick_spawn_button = [&](const char* label, const char* desc, ImU32 col, auto fn) {
                ImGui::PushStyleColor(ImGuiCol_Button, col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 80, 105, 255));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
                if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) {
                    fn();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", desc);
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
            };

            quick_spawn_button(("+ " + AV("add_weather")).c_str(), "Atmospheric Weather, Height Fog, Wetness & Wind", IM_COL32(14, 116, 144, 200), [&]{
                std::string err; app.create_weather(ecs::kInvalidEntity, err);
            });
            quick_spawn_button(("+ " + AV("add_cable")).c_str(), "Verlet Physics Cable with Tension & Breakable Dynamics", IM_COL32(180, 83, 9, 200), [&]{
                std::string err; app.create_cable(ecs::kInvalidEntity, err);
            });
            quick_spawn_button(("+ " + AV("add_talent_tree")).c_str(), "Skill & Talent Progression Graph with Tiers & Gates", IM_COL32(190, 24, 93, 200), [&]{
                std::string err; app.create_talent_tree(ecs::kInvalidEntity, err);
            });
            quick_spawn_button(("+ " + AV("add_flow_field")).c_str(), "Crowd Flow Field Grid for Massive Scale AI Navigation", IM_COL32(21, 128, 61, 200), [&]{
                std::string err; app.create_flow_field(ecs::kInvalidEntity, err);
            });
            quick_spawn_button(("+ " + AV("add_rule_tile")).c_str(), "2D 8-Neighbor Rule Tile Autotiling Map", IM_COL32(4, 120, 87, 200), [&]{
                std::string err; app.create_rule_tile(ecs::kInvalidEntity, err);
            });

            ImGui::Separator();

            // 3. Active Advanced Systems Census
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.85f, 0.70f, 1.0f, 1.0f), "%s", AV("active_systems_census").c_str());

            int n_weather = 0, n_cables = 0, n_talents = 0, n_flow = 0, n_ruletiles = 0, n_inv = 0, n_rag = 0, n_saudio = 0;
            if (w != nullptr) {
                for (ecs::Entity e : w->all_entities()) {
                    if (w->has<runtime::WeatherComponent>(e)) n_weather++;
                    if (w->has<runtime::CableComponent>(e)) n_cables++;
                    if (w->has<runtime::TalentComponent>(e)) n_talents++;
                    if (w->has<runtime::FlowFieldComponent>(e)) n_flow++;
                    if (w->has<runtime::RuleTileComponent>(e)) n_ruletiles++;
                    if (w->has<runtime::InventoryComponent>(e)) n_inv++;
                    if (w->has<runtime::RagdollComponent>(e)) n_rag++;
                    if (w->has<runtime::SurfaceAudioComponent>(e)) n_saudio++;
                }
            }

            auto draw_system_row = [&](const char* label, int count, ImVec4 col, auto select_fn) {
                if (count <= 0) return;
                ImGui::Bullet();
                ImGui::TextColored(col, "%s: %d", label, count);
                ImGui::SameLine(ImGui::GetWindowWidth() - 75.0f);
                if (ImGui::SmallButton((AV("select") + "##" + label).c_str())) {
                    select_fn();
                }
            };

            if (n_weather + n_cables + n_talents + n_flow + n_ruletiles + n_inv + n_rag + n_saudio == 0) {
                ImGui::TextDisabled("%s", AV("no_active_systems").c_str());
            } else {
                draw_system_row(AV("weather_system").c_str(), n_weather, ImVec4(0.35f, 0.75f, 1.0f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::WeatherComponent>(e)) { app.selection().set_single(e); break; }
                });
                draw_system_row(AV("cable_sim").c_str(), n_cables, ImVec4(1.0f, 0.65f, 0.25f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::CableComponent>(e)) { app.selection().set_single(e); break; }
                });
                draw_system_row(AV("talent_tree").c_str(), n_talents, ImVec4(0.95f, 0.45f, 0.75f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::TalentComponent>(e)) { app.selection().set_single(e); break; }
                });
                draw_system_row(AV("flow_field").c_str(), n_flow, ImVec4(0.20f, 0.85f, 0.50f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::FlowFieldComponent>(e)) { app.selection().set_single(e); break; }
                });
                draw_system_row(AV("rule_tile").c_str(), n_ruletiles, ImVec4(0.35f, 0.90f, 0.65f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::RuleTileComponent>(e)) { app.selection().set_single(e); break; }
                });
                draw_system_row(AV("inventory").c_str(), n_inv, ImVec4(0.70f, 0.50f, 0.95f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::InventoryComponent>(e)) { app.selection().set_single(e); break; }
                });
                draw_system_row(AV("ragdoll").c_str(), n_rag, ImVec4(0.95f, 0.40f, 0.40f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::RagdollComponent>(e)) { app.selection().set_single(e); break; }
                });
                draw_system_row(AV("surface_audio").c_str(), n_saudio, ImVec4(1.0f, 0.75f, 0.45f, 1.0f), [&]{
                    for (ecs::Entity e : w->all_entities()) if (w->has<runtime::SurfaceAudioComponent>(e)) { app.selection().set_single(e); break; }
                });
            }
        } else {
            // Identity line: the stable Entity id + generation (what undo,
            // logs and the outliner's ###e<id> all key on).
            rt_disabled_str(AVF("inspector_entity_gen", static_cast<unsigned>(sel.id),
                                static_cast<unsigned>(sel.generation)));
            InspectorCache& ic = inspector_cache();
            if (!ic.valid || !(ic.entity == sel)) {
                // A drag in progress targets the old material: fold it into
                // one undo step before the cache is overwritten below.
                if (ic.mat_live) {
                    std::string commit_err;
                    app.commit_material_params(ic.mat_path, ic.mat_before, commit_err);
                    ic.mat_live = false;
                }
                ic.entity = sel;
                ic.valid = true;
                ic.error.clear();
                const std::string nm = entity_label_ptr(app, sel);
                std::strncpy(ic.name, nm.c_str(), sizeof(ic.name) - 1);
                ic.name[sizeof(ic.name) - 1] = '\0';
                const TransformEdit t = read_transform(*w, sel);
                ic.pos[0] = t.px;
                ic.pos[1] = t.py;
                ic.pos[2] = t.pz;
                ic.rot[0] = t.rx;
                ic.rot[1] = t.ry;
                ic.rot[2] = t.rz;
                ic.scl[0] = t.sx;
                ic.scl[1] = t.sy;
                ic.scl[2] = t.sz;
                bool has_cam = false;
                const CameraEdit c = read_camera(*w, sel, has_cam);
                ic.cam_active = c.active;
                ic.cam[0] = c.fov_y;
                ic.cam[1] = c.near_plane;
                ic.cam[2] = c.far_plane;
                ic.weapon = read_weapon(*w, sel, ic.weapon_has);
                ic.health = read_health(*w, sel, ic.health_has);
                ic.agent = read_combat_agent(*w, sel, ic.agent_has);
                std::strncpy(ic.agent_target, ic.agent.target.c_str(),
                             sizeof(ic.agent_target) - 1);
                ic.agent_target[sizeof(ic.agent_target) - 1] = '\0';
                ic.agent_auto_target = ic.agent.auto_target;
                ic.faction = read_faction(*w, sel, ic.faction_has);
                std::strncpy(ic.faction_team, ic.faction.team.c_str(),
                             sizeof(ic.faction_team) - 1);
                ic.faction_team[sizeof(ic.faction_team) - 1] = '\0';
                ic.faction_friendly_fire = ic.faction.friendly_fire;
                ic.faction_enabled = ic.faction.enabled;
                bool has_light = false;
                const LightEdit l = read_light(*w, sel, has_light);
                ic.light_dir[0] = l.dir_x;
                ic.light_dir[1] = l.dir_y;
                ic.light_dir[2] = l.dir_z;
                ic.light_color[0] = l.color_r;
                ic.light_color[1] = l.color_g;
                ic.light_color[2] = l.color_b;
                ic.light_intensity = l.intensity;
                ic.light_shadows = l.cast_shadows;
                ic.light_shadow_strength = l.shadow_strength;
                ic.light_shadow_bias = l.shadow_bias;
                ic.light_shadow_cascades = l.shadow_cascades;
                ic.light_shadow_distance = l.shadow_distance;
                const SkyEdit s = read_sky(*w, sel, ic.sky_has);
                ic.sky_enabled = s.enabled;
                // Read every frame, not only when the panel is open: a day/night
                // cycle advances during play, and a stale cached hour would show
                // the author a time the scene is no longer at.
                const TimeOfDayEdit tod = read_time_of_day(*w, sel, ic.tod_has);
                if (ic.tod_has) {
                    ic.tod_hours = tod.time_hours;
                    ic.tod_day_length = tod.day_length_seconds;
                    ic.tod_enabled = tod.enabled;
                    ic.tod_drive_light = tod.drive_light;
                }
                ic.sky_zenith[0] = s.zenith[0];
                ic.sky_zenith[1] = s.zenith[1];
                ic.sky_zenith[2] = s.zenith[2];
                ic.sky_horizon[0] = s.horizon[0];
                ic.sky_horizon[1] = s.horizon[1];
                ic.sky_horizon[2] = s.horizon[2];
                ic.sky_ground[0] = s.ground[0];
                ic.sky_ground[1] = s.ground[1];
                ic.sky_ground[2] = s.ground[2];
                ic.sky_sun_disk = s.sun_disk;
                ic.sky_sun_glow = s.sun_glow;
                const runtime::PostProcessComponent pp = read_post_process(*w, sel, ic.pp_has);
                if (ic.pp_has) {
                    ic.pp_bloom_enabled = pp.bloom_enabled;
                    ic.pp_bloom_threshold = pp.bloom_threshold;
                    ic.pp_bloom_knee = pp.bloom_knee;
                    ic.pp_bloom_intensity = pp.bloom_intensity;
                    ic.pp_bloom_radius = pp.bloom_radius;
                    ic.pp_grade_enabled = pp.grade_enabled;
                    ic.pp_grade_contrast = pp.grade_contrast;
                    ic.pp_grade_pivot = pp.grade_pivot;
                    ic.pp_grade_temperature = pp.grade_temperature;
                    ic.pp_grade_tint = pp.grade_tint;
                    ic.pp_grade_gamma = pp.grade_gamma;
                    ic.pp_sharpen_enabled = pp.sharpen_enabled;
                    ic.pp_sharpen_amount = pp.sharpen_amount;
                    ic.pp_sharpen_radius = pp.sharpen_radius;
                    ic.pp_saturation = pp.saturation;
                    ic.pp_vignette = pp.vignette;
                    ic.pp_lens_enabled = pp.lens_enabled;
                    ic.pp_lens_distortion = pp.lens_distortion;
                    ic.pp_lens_chroma = pp.lens_chromatic_aberration;
                    ic.pp_dof_enabled = pp.dof_enabled;
                    ic.pp_dof_focus = pp.dof_focus_distance;
                    ic.pp_dof_range = pp.dof_focus_range;
                    ic.pp_dof_radius = pp.dof_max_radius;
                    ic.pp_motion_enabled = pp.motion_enabled;
                    ic.pp_motion_intensity = pp.motion_intensity;
                    ic.pp_motion_length = pp.motion_max_length;
                    ic.pp_fxaa_enabled = pp.fxaa_enabled;
                    ic.pp_fxaa_threshold = pp.fxaa_threshold;
                    ic.pp_fxaa_threshold_min = pp.fxaa_threshold_min;
                    ic.pp_render_scale = pp.render_scale;
                    ic.pp_dynamic_resolution = pp.dynamic_resolution;
                    ic.pp_target_fps = pp.target_fps;
                    ic.pp_min_render_scale = pp.min_render_scale;
                    ic.pp_dynamic_resolution_timing = pp.dynamic_resolution_timing;
                    ic.pp_frame_policy = pp.frame_policy;
                    ic.pp_exposure = pp.exposure;
                    ic.pp_tonemap = pp.tonemap;
                    std::strncpy(ic.pp_lut_path, pp.lut_path.c_str(), sizeof(ic.pp_lut_path) - 1);
                    ic.pp_lut_path[sizeof(ic.pp_lut_path) - 1] = '\0';
                    ic.pp_lut_strength = pp.lut_strength;
                    ic.pp_fog_enabled = pp.fog_enabled;
                    ic.pp_fog_color[0] = pp.fog_r;
                    ic.pp_fog_color[1] = pp.fog_g;
                    ic.pp_fog_color[2] = pp.fog_b;
                    ic.pp_fog_start = pp.fog_start;
                    ic.pp_fog_end = pp.fog_end;
                }
                const DecalEdit decal = read_decal(*w, sel, ic.decal_has);
                if (ic.decal_has) {
                    std::strncpy(ic.decal_material, decal.material,
                                 sizeof(ic.decal_material) - 1);
                    ic.decal_material[sizeof(ic.decal_material) - 1] = '\0';
                    ic.decal_size[0] = decal.size_x;
                    ic.decal_size[1] = decal.size_y;
                    ic.decal_size[2] = decal.size_z;
                    ic.decal_opacity = decal.opacity;
                    ic.decal_enabled = decal.enabled;
                }
                if (const auto* rz = w->get<audio::ReverbZoneComponent>(sel)) {
                    ic.rz_has = true;
                    ic.rz_edit = *rz;
                } else {
                    ic.rz_has = false;
                }
                if (const auto* mus = w->get<audio::MusicComponent>(sel)) {
                    ic.mus_has = true;
                    std::strncpy(ic.mus_buffer, mus->buffer_name.c_str(), sizeof(ic.mus_buffer) - 1);
                    ic.mus_buffer[sizeof(ic.mus_buffer) - 1] = '\0';
                    ic.mus_volume = mus->volume;
                    ic.mus_fade = mus->fade_in_seconds;
                    ic.mus_enabled = mus->enabled;
                } else {
                    ic.mus_has = false;
                }
                if (const auto* amb = w->get<audio::AmbienceComponent>(sel)) {
                    ic.amb_has = true;
                    std::strncpy(ic.amb_buffer, amb->buffer_name.c_str(), sizeof(ic.amb_buffer) - 1);
                    ic.amb_buffer[sizeof(ic.amb_buffer) - 1] = '\0';
                    ic.amb_fade = amb->fade_in_seconds;
                    ic.amb_enabled = amb->enabled;
                } else {
                    ic.amb_has = false;
                }
                if (const auto* m = w->get<runtime::MeshComponent>(sel)) {
                    const std::string id = m->mesh_id.to_string();
                    std::strncpy(ic.mesh_id, id.c_str(), sizeof(ic.mesh_id) - 1);
                    ic.mesh_id[sizeof(ic.mesh_id) - 1] = '\0';
                    std::strncpy(ic.mesh_mat, m->material.c_str(), sizeof(ic.mesh_mat) - 1);
                    ic.mesh_mat[sizeof(ic.mesh_mat) - 1] = '\0';
                    const std::string mpath =
                        m->material.empty() ? runtime::kDefaultMaterialPath : m->material;
                    std::strncpy(ic.mat_path, mpath.c_str(), sizeof(ic.mat_path) - 1);
                    ic.mat_path[sizeof(ic.mat_path) - 1] = '\0';
                    if (app.runtime() != nullptr) {
                        rendering::PBRMaterialParams mp{};
                        app.runtime()->material_params(mpath, mp);
                        ic.mat_base[0] = mp.base_color[0];
                        ic.mat_base[1] = mp.base_color[1];
                        ic.mat_base[2] = mp.base_color[2];
                        ic.mat_base[3] = mp.base_color[3];
                        ic.mat_metal = mp.metallic;
                        ic.mat_rough = mp.roughness;
                        ic.mat_ao = mp.ao;
                        ic.mat_em[0] = mp.emission[0];
                        ic.mat_em[1] = mp.emission[1];
                        ic.mat_em[2] = mp.emission[2];
                        ic.mat_estr = mp.emission_strength;
                    }
                    ic.mat_choice = 0;
                    ic.albedo_choice = 0;
                } else {
                    ic.mesh_id[0] = '\0';
                    ic.mesh_mat[0] = '\0';
                    ic.mat_path[0] = '\0';
                }
                // Physics cache init
                if (const auto* rb = w->get<physics::RigidBodyComponent>(sel)) {
                    ic.rb_type = static_cast<int>(rb->type);
                    ic.rb_mass = rb->mass;
                    ic.rb_friction = rb->friction;
                    ic.rb_restitution = rb->restitution;
                    ic.rb_lin_damp = rb->linear_damping;
                    ic.rb_ang_damp = rb->angular_damping;
                    ic.rb_allow_sleep = rb->allow_sleep;
                }
                if (const auto* col = w->get<physics::ColliderComponent>(sel)) {
                    ic.col_shape = static_cast<int>(col->shape.type);
                    ic.col_radius = col->shape.sphere.radius;
                    ic.col_half[0] = col->shape.box.half_extents.x;
                    ic.col_half[1] = col->shape.box.half_extents.y;
                    ic.col_half[2] = col->shape.box.half_extents.z;
                    ic.col_normal[0] = col->shape.plane.normal.x;
                    ic.col_normal[1] = col->shape.plane.normal.y;
                    ic.col_normal[2] = col->shape.plane.normal.z;
                }
                // Animation cache init. The clip list comes from the component's
                // own table, so the dropdown can only offer clips that exist.
                ic.anim_clip_names.clear();
                ic.anim_clip = 0;
                if (const auto* anim = w->get<animation::AnimationComponent>(sel)) {
                    ic.anim_speed = anim->speed;
                    ic.anim_loop = static_cast<int>(anim->player.loop_mode());
                    ic.anim_paused = anim->paused;
                    ic.anim_state_machine = anim->use_state_machine;
                    ic.anim_clip_names.reserve(anim->clips.size());
                    for (const auto& entry : anim->clips) {
                        if (entry.first == anim->player.clip_name()) {
                            ic.anim_clip = static_cast<int>(ic.anim_clip_names.size());
                        }
                        ic.anim_clip_names.push_back(entry.first);
                    }
                }
                if (const auto* aud = w->get<audio::AudioComponent>(sel)) {
                    ic.aud_volume = aud->volume;
                    ic.aud_pitch = aud->pitch;
                    ic.aud_looping = aud->looping;
                    ic.aud_spatial = aud->spatial;
                    ic.aud_autoplay = aud->autoplay;
                    ic.aud_min_dist = aud->spatial_settings.min_distance;
                    ic.aud_max_dist = aud->spatial_settings.max_distance;
                }
                if (const auto* dst = w->get<runtime::DestructibleComponent>(sel)) {
                    ic.dst_chunks = static_cast<int>(dst->chunks);
                    ic.dst_seed = dst->seed;
                    ic.dst_strength = dst->strength;
                    ic.dst_threshold = dst->damage_threshold;
                    ic.dst_blast = dst->blast_radius;
                    ic.dst_enabled = dst->enabled;
                }
                if (const auto* fb = w->get<runtime::FeedbackComponent>(sel)) {
                    ic.fb_has = true;
                    ic.fb_enabled = fb->enabled;
                    ic.fb_audio = fb->audio;
                    ic.fb_light = fb->flash_light;
                    ic.fb_flash_time = fb->flash_seconds;
                    ic.fb_flash_size = fb->flash_size;
                    ic.fb_blast_time = fb->blast_flash_seconds;
                    ic.fb_blast_size = fb->blast_flash_size;
                    ic.fb_tracer_time = fb->tracer_seconds;
                    ic.fb_tracer_length = fb->tracer_length;
                    ic.fb_tracer_width = fb->tracer_width;
                    ic.fb_impact_time = fb->impact_seconds;
                    ic.fb_impact_size = fb->impact_size;
                    ic.fb_shot_volume = fb->shot_volume;
                    ic.fb_impact_volume = fb->impact_volume;
                    ic.fb_impact_volume_explosive = fb->impact_volume_explosive;
                    ic.fb_impact_volume_fire = fb->impact_volume_fire;
                    ic.fb_light_intensity = fb->flash_light_intensity;
                    ic.fb_light_radius = fb->flash_light_radius;
                    ic.fb_light_color[0] = fb->flash_light_r;
                    ic.fb_light_color[1] = fb->flash_light_g;
                    ic.fb_light_color[2] = fb->flash_light_b;
                    ic.fb_max_flashes = static_cast<int>(fb->max_flashes);
                    ic.fb_max_tracers = static_cast<int>(fb->max_tracers);
                    ic.fb_max_impacts = static_cast<int>(fb->max_impacts);
                    ic.fb_max_sounds = static_cast<int>(fb->max_sounds);
                    ic.fb_max_lights = static_cast<int>(fb->max_lights);
                } else {
                    ic.fb_has = false;
                }
                if (const auto* pc = w->get<runtime::PerceptionComponent>(sel)) {
                    ic.perc_has = true;
                    ic.perc_enabled = pc->enabled;
                    ic.perc_pain = pc->pain;
                    ic.perc_governs = pc->governs_combat;
                    ic.perc_sight = pc->sight_range;
                    ic.perc_fov = pc->sight_half_fov_deg;
                    ic.perc_gain = pc->sight_gain;
                    ic.perc_loss = pc->sight_loss;
                    ic.perc_linger = pc->sight_linger;
                    ic.perc_hearing = pc->hearing_range;
                    ic.perc_touch = pc->touch_range;
                    ic.perc_noise = pc->noise;
                    ic.perc_visible = pc->visibility;
                    ic.perc_radius = pc->radius;
                    ic.perc_eye = pc->eye_height;
                } else {
                    ic.perc_has = false;
                }
                if (const auto* squad = w->get<runtime::SquadComponent>(sel)) {
                    ic.squad_has = true;
                    ic.squad_enabled = squad->enabled;
                    ic.squad_radio = squad->radio;
                    ic.squad_lost = squad->lost;
                    ic.squad_cone = squad->cone;
                    std::strncpy(ic.squad_token, squad->token.c_str(),
                                 sizeof(ic.squad_token) - 1);
                    ic.squad_token[sizeof(ic.squad_token) - 1] = '\0';
                } else {
                    ic.squad_has = false;
                }
                if (const auto* jammer = w->get<runtime::JammerComponent>(sel)) {
                    ic.jammer_has = true;
                    ic.jammer_enabled = jammer->enabled;
                    ic.jammer_radius = jammer->radius;
                    ic.jammer_strength = jammer->strength;
                } else {
                    ic.jammer_has = false;
                }
                if (const auto* brain = w->get<runtime::BrainComponent>(sel)) {
                    ic.brain_has = true;
                    ic.brain_enabled = brain->enabled;
                    ic.brain_rate = brain->rate;
                    ic.brain_actions = brain->actions;
                } else {
                    ic.brain_has = false;
                    ic.brain_actions.clear();
                }
                if (const auto* machine = w->get<runtime::StateMachineComponent>(sel)) {
                    ic.sm_has = true;
                    ic.sm_enabled = machine->enabled;
                    ic.sm_rate = machine->rate;
                    ic.sm_states = machine->states;
                    ic.sm_transitions = machine->transitions;
                } else {
                    ic.sm_has = false;
                    ic.sm_states.clear();
                    ic.sm_transitions.clear();
                }
                if (const auto* sc = w->get<scripting::ScriptComponent>(sel)) {
                    std::strncpy(ic.script_path, sc->path.c_str(), sizeof(ic.script_path) - 1);
                    ic.script_path[sizeof(ic.script_path) - 1] = '\0';
                    ic.script_enabled = sc->enabled;
                } else {
                    ic.script_path[0] = '\0';
                    ic.script_enabled = true;
                }
                if (const auto* aud = w->get<audio::AudioComponent>(sel)) {
                    std::strncpy(ic.aud_buffer, aud->buffer_name.c_str(), sizeof(ic.aud_buffer) - 1);
                    ic.aud_buffer[sizeof(ic.aud_buffer) - 1] = '\0';
                } else {
                    ic.aud_buffer[0] = '\0';
                }
                if (const auto* sky = w->get<runtime::SkyComponent>(sel)) {
                    ic.sky_clear[0] = sky->clear_r;
                    ic.sky_clear[1] = sky->clear_g;
                    ic.sky_clear[2] = sky->clear_b;
                }
                if (const auto* pc = w->get<vfx::ParticleComponent>(sel)) {
                    ic.part_has = true;
                    ic.part_enabled = pc->enabled;
                    ic.part_rate = pc->config.rate;
                    ic.part_lifetime = pc->config.lifetime;
                    ic.part_lifetime_spread = pc->config.lifetime_spread;
                    ic.part_velocity[0] = pc->config.velocity.x;
                    ic.part_velocity[1] = pc->config.velocity.y;
                    ic.part_velocity[2] = pc->config.velocity.z;
                    ic.part_vel_spread[0] = pc->config.velocity_spread.x;
                    ic.part_vel_spread[1] = pc->config.velocity_spread.y;
                    ic.part_vel_spread[2] = pc->config.velocity_spread.z;
                    ic.part_gravity[0] = pc->config.gravity.x;
                    ic.part_gravity[1] = pc->config.gravity.y;
                    ic.part_gravity[2] = pc->config.gravity.z;
                    ic.part_drag = pc->config.drag;
                    ic.part_start_size = pc->config.start_size;
                    ic.part_end_size = pc->config.end_size;
                    ic.part_start_color[0] = pc->config.start_color.x;
                    ic.part_start_color[1] = pc->config.start_color.y;
                    ic.part_start_color[2] = pc->config.start_color.z;
                    ic.part_end_color[0] = pc->config.end_color.x;
                    ic.part_end_color[1] = pc->config.end_color.y;
                    ic.part_end_color[2] = pc->config.end_color.z;
                    ic.part_max = static_cast<int>(pc->config.max_particles);
                } else {
                    ic.part_has = false;
                }
                if (const auto* cc = w->get<physics::ClothComponent>(sel)) {
                    ic.cloth_has = true;
                    ic.cloth_enabled = cc->enabled;
                    ic.cloth_res_x = cc->config.res_x;
                    ic.cloth_res_z = cc->config.res_z;
                    ic.cloth_spacing = cc->config.spacing;
                    ic.cloth_mass = cc->config.mass;
                    ic.cloth_damping = cc->config.damping;
                    ic.cloth_stiffness = cc->config.stiffness;
                    ic.cloth_iterations = cc->config.iterations;
                    ic.cloth_substeps = cc->config.substeps;
                    ic.cloth_gravity[0] = cc->config.gravity.x;
                    ic.cloth_gravity[1] = cc->config.gravity.y;
                    ic.cloth_gravity[2] = cc->config.gravity.z;
                } else {
                    ic.cloth_has = false;
                }
                if (const auto* ch = w->get<physics::CharacterComponent>(sel)) {
                    ic.char_has = true;
                    ic.char_enabled = ch->enabled;
                    ic.char_radius = ch->config.radius;
                    ic.char_max_speed = ch->config.max_speed;
                    ic.char_acceleration = ch->config.acceleration;
                    ic.char_air_control = ch->config.air_control;
                    ic.char_jump_speed = ch->config.jump_speed;
                    ic.char_slope = ch->config.slope_limit_deg;
                    ic.char_mass = ch->config.mass;
                    ic.char_friction = ch->config.friction;
                    ic.char_wish[0] = ch->wish_dir.x;
                    ic.char_wish[1] = ch->wish_dir.y;
                    ic.char_wish[2] = ch->wish_dir.z;
                    ic.char_jump = ch->jump;
                } else {
                    ic.char_has = false;
                }
                if (const auto* navm_comp = w->get<runtime::NavMeshComponent>(sel)) {
                    ic.navm_has = true;
                    ic.navm_area[0] = navm_comp->area_x;
                    ic.navm_area[1] = navm_comp->area_y;
                    ic.navm_area[2] = navm_comp->area_z;
                    ic.navm_cell = navm_comp->cell_size;
                    ic.navm_cell_height = navm_comp->cell_height;
                    ic.navm_slope = navm_comp->walkable_slope_deg;
                    ic.navm_climb = navm_comp->walkable_climb;
                    ic.navm_headroom = navm_comp->walkable_height;
                    ic.navm_min_area = navm_comp->min_region_area;
                    ic.navm_radius = navm_comp->agent_radius;
                    ic.navm_jump_distance = navm_comp->jump_distance;
                    ic.navm_jump_height = navm_comp->jump_height;
                    ic.navm_max_verts = static_cast<int>(navm_comp->max_verts_per_poly);
                    ic.navm_enabled = navm_comp->enabled;
                    ic.navm_link_count = static_cast<int>(navm_comp->links.size());
                } else {
                    ic.navm_has = false;
                }
                if (const auto* terr = w->get<runtime::TerrainComponent>(sel)) {
                    ic.terr_has = true;
                    ic.terr_enabled = terr->enabled;
                    ic.terr_res = static_cast<int>(terr->resolution);
                    ic.terr_area = terr->area;
                    ic.terr_height = terr->height_scale;
                    ic.terr_freq = terr->noise_frequency;
                    ic.terr_octaves = static_cast<int>(terr->octaves);
                    ic.terr_seed = static_cast<int>(terr->seed);
                    std::snprintf(ic.terr_heightmap, sizeof(ic.terr_heightmap), "%s",
                                  terr->heightmap.c_str());
                    std::snprintf(ic.terr_material, sizeof(ic.terr_material), "%s",
                                  terr->material.c_str());
                } else {
                    ic.terr_has = false;
                }
                if (const auto* nava_comp = w->get<runtime::NavAgentComponent>(sel)) {
                    ic.nava_has = true;
                    ic.nava_speed = nava_comp->speed;
                    ic.nava_goal[0] = nava_comp->goal_x;
                    ic.nava_goal[1] = nava_comp->goal_y;
                    ic.nava_goal[2] = nava_comp->goal_z;
                    ic.nava_arrive = nava_comp->arrive_radius;
                    ic.nava_enabled = nava_comp->enabled;
                } else {
                    ic.nava_has = false;
                }
                if (const auto* navr_comp = w->get<runtime::NavRouteComponent>(sel)) {
                    ic.navr_has = true;
                    ic.navr_wait = navr_comp->wait;
                    ic.navr_loop = navr_comp->loop;
                    ic.navr_ping_pong = navr_comp->ping_pong;
                    ic.navr_enabled = navr_comp->enabled;
                    ic.navr_hold = static_cast<int>(navr_comp->hold);
                    ic.navr_resume = navr_comp->resume;
                    ic.navr_investigate = navr_comp->investigate;
                    std::strncpy(ic.navr_squad, navr_comp->squad.c_str(),
                                 sizeof(ic.navr_squad) - 1);
                    ic.navr_squad[sizeof(ic.navr_squad) - 1] = '\0';
                    ic.navr_points = navr_comp->points;
                } else {
                    ic.navr_has = false;
                    ic.navr_points.clear();
                }
                if (const auto* veh_comp = w->get<physics::VehicleComponent>(sel)) {
                    ic.veh_has = true;
                    ic.veh_enabled = veh_comp->enabled;
                    ic.veh_chassis[0] = veh_comp->config.chassis_half_extents.x;
                    ic.veh_chassis[1] = veh_comp->config.chassis_half_extents.y;
                    ic.veh_chassis[2] = veh_comp->config.chassis_half_extents.z;
                    ic.veh_mass = veh_comp->config.chassis_mass;
                    ic.veh_wheel_radius = veh_comp->config.wheel_radius;
                    ic.veh_wheel_width = veh_comp->config.wheel_width;
                    ic.veh_track = veh_comp->config.track_half_width;
                    ic.veh_wheelbase = veh_comp->config.wheelbase_half_length;
                    ic.veh_wheel_y = veh_comp->config.wheel_y;
                    ic.veh_steer = veh_comp->config.max_steer_deg;
                    ic.veh_torque = veh_comp->config.engine_max_torque;
                    ic.veh_susp_min = veh_comp->config.suspension_min_length;
                    ic.veh_susp_max = veh_comp->config.suspension_max_length;
                    ic.veh_susp_freq = veh_comp->config.suspension_frequency_hz;
                    ic.veh_susp_damping = veh_comp->config.suspension_damping;
                    ic.veh_tire_peak = veh_comp->config.tire_peak_friction;
                    ic.veh_tire_limit = veh_comp->config.tire_limit_friction;
                    ic.veh_inertia = veh_comp->config.wheel_inertia;
                    ic.veh_angular_damping = veh_comp->config.angular_damping;
                    ic.veh_brake = veh_comp->config.max_brake_torque;
                    ic.veh_handbrake = veh_comp->config.max_handbrake_torque;
                    ic.veh_throttle_in = veh_comp->throttle;
                    ic.veh_steer_in = veh_comp->steer;
                    ic.veh_brake_in = veh_comp->brake;
                    ic.veh_handbrake_in = veh_comp->handbrake;
                } else {
                    ic.veh_has = false;
                }
                if (const auto* cb = w->get<runtime::CableComponent>(sel)) {
                    ic.cable_has = true;
                    ic.cable_length = cb->total_length;
                    ic.cable_particles = static_cast<int>(cb->particle_count);
                    ic.cable_stiffness = cb->stiffness;
                    ic.cable_damping = cb->damping;
                    ic.cable_density = cb->linear_density;
                    ic.cable_tension_max = cb->max_tension;
                    ic.cable_breakable = cb->breakable;
                    ic.cable_pin_start = cb->pin_start;
                    ic.cable_pin_end = cb->pin_end;
                    ic.cable_end_offset[0] = cb->end_offset.x;
                    ic.cable_end_offset[1] = cb->end_offset.y;
                    ic.cable_end_offset[2] = cb->end_offset.z;
                    ic.cable_enabled = cb->enabled;
                } else {
                    ic.cable_has = false;
                }
                if (const auto* wc = w->get<runtime::WeatherComponent>(sel)) {
                    ic.weather_has = true;
                    ic.weather_type = wc->weather_type;
                    ic.w_fog_density = wc->fog_density;
                    ic.w_fog_falloff = wc->fog_height_falloff;
                    ic.w_fog_base = wc->fog_base_height;
                    ic.w_fog_max_opacity = wc->fog_max_opacity;
                    ic.w_fog_color[0] = wc->fog_color[0];
                    ic.w_fog_color[1] = wc->fog_color[1];
                    ic.w_fog_color[2] = wc->fog_color[2];
                    ic.w_sun_inscattering = wc->sun_inscattering;
                    ic.w_ground_wetness = wc->ground_wetness;
                    ic.w_wind_strength = wc->wind_strength;
                    ic.w_wind_dir[0] = wc->wind_direction[0];
                    ic.w_wind_dir[1] = wc->wind_direction[1];
                    ic.w_wind_dir[2] = wc->wind_direction[2];
                    ic.weather_enabled = wc->enabled;
                } else {
                    ic.weather_has = false;
                }
                if (const auto* rtc = w->get<runtime::RuleTileComponent>(sel)) {
                    ic.rule_tile_has = true;
                    ic.rt_default_id = static_cast<int>(rtc->default_tile_id);
                    ic.rt_preset = rtc->rule_preset;
                    ic.rt_count = static_cast<int>(rtc->rule_count);
                    ic.rt_auto_update = rtc->auto_update;
                    ic.rt_enabled = rtc->enabled;
                } else {
                    ic.rule_tile_has = false;
                }
                if (const auto* ffc = w->get<runtime::FlowFieldComponent>(sel)) {
                    ic.flow_has = true;
                    ic.ff_width = static_cast<int>(ffc->grid_width);
                    ic.ff_height = static_cast<int>(ffc->grid_height);
                    ic.ff_cell_size = ffc->cell_size;
                    ic.ff_target[0] = ffc->target_x;
                    ic.ff_target[1] = ffc->target_y;
                    ic.ff_enabled = ffc->enabled;
                } else {
                    ic.flow_has = false;
                }
                if (const auto* tc = w->get<runtime::TalentComponent>(sel)) {
                    ic.talent_has = true;
                    std::strncpy(ic.tt_id, tc->tree_id.c_str(), sizeof(ic.tt_id) - 1);
                    ic.tt_id[sizeof(ic.tt_id) - 1] = '\0';
                    std::strncpy(ic.tt_name, tc->tree_name.c_str(), sizeof(ic.tt_name) - 1);
                    ic.tt_name[sizeof(ic.tt_name) - 1] = '\0';
                    ic.tt_available = static_cast<int>(tc->available_points);
                    ic.tt_spent = static_cast<int>(tc->spent_points);
                    ic.tt_unlocked = static_cast<int>(tc->unlocked_count);
                    ic.talent_enabled = tc->enabled;
                } else {
                    ic.talent_has = false;
                }
                if (const auto* rdc = w->get<runtime::RagdollComponent>(sel)) {
                    ic.ragdoll_has = true;
                    ic.rd_weight = rdc->blend_weight;
                    ic.rd_pose = rdc->pose_state;
                    ic.rd_duration = rdc->blend_duration;
                    ic.rd_momentum = rdc->preserve_momentum;
                    ic.ragdoll_enabled = rdc->enabled;
                } else {
                    ic.ragdoll_has = false;
                }
                if (const auto* inc = w->get<runtime::InventoryComponent>(sel)) {
                    ic.inv_has = true;
                    ic.inv_cols = static_cast<int>(inc->columns);
                    ic.inv_rows = static_cast<int>(inc->rows);
                    ic.inv_max_weight = inc->max_weight;
                    ic.inv_cur_weight = inc->current_weight;
                    ic.inv_item_count = static_cast<int>(inc->item_count);
                    ic.inv_auto_pack = inc->auto_pack;
                    ic.inv_enabled = inc->enabled;
                } else {
                    ic.inv_has = false;
                }
                if (const auto* sac = w->get<runtime::SurfaceAudioComponent>(sel)) {
                    ic.sa_has = true;
                    ic.sa_mat = sac->surface_material;
                    ic.sa_vol = sac->volume_scale;
                    ic.sa_pitch = sac->pitch_variation;
                    ic.sa_wetness = sac->wetness_modulation;
                    ic.sa_enabled = sac->enabled;
                } else {
                    ic.sa_has = false;
                }
            }
            if (!ic.error.empty()) {
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s: %s", AV("error").c_str(),
                                   ic.error.c_str());
            }
            // --- Add Component (Unity-style single entry point) ---
            // Every section below also carries its own Attach, but a user
            // hunting for "where do I add X" should have one place to look.
            // Each item forwards to the same validated EditorApp method the
            // section uses; missing-component items only.
            if (ImGui::Button((AV("add_component") + "###AddComponent").c_str())) {
                ImGui::OpenPopup("##add_component_menu");
            }
            if (ImGui::BeginPopup("##add_component_menu")) {
                std::string add_err;
                bool added = false;
                if (!w->has<runtime::CameraComponent>(sel) &&
                    ImGui::MenuItem(AV("camera").c_str())) {
                    added = app.set_camera(sel, CameraEdit{}, add_err);
                } else if (!w->has<runtime::DirectionalLight>(sel) &&
                           ImGui::MenuItem(AV("directional_light").c_str())) {
                    added = app.set_light(sel, LightEdit{}, add_err);
                } else if (!ic.sky_has && ImGui::MenuItem(AV("sky").c_str())) {
                    added = app.apply_sky_edit(SkyEdit{}, add_err);
                } else if (!ic.pp_has && ImGui::MenuItem(AV("post_process").c_str())) {
                    added = app.set_post_process(sel, runtime::PostProcessComponent{}, add_err);
                } else if (!w->has<runtime::DecalComponent>(sel) &&
                           ImGui::MenuItem(AV("decal").c_str())) {
                    added = app.set_decal(sel, DecalEdit{}, add_err);
                } else if (!ic.rz_has && ImGui::MenuItem(AV("reverb_zone").c_str())) {
                    // Music and ambience are deliberately NOT in this menu: both
                    // need a buffer path, and a component naming no file is
                    // refused by the setter (and warned about by the loader).
                    // Their sections carry the path field and the add button.
                    added = app.set_reverb_zone(sel, audio::ReverbZoneComponent{}, add_err);
                } else if (!w->has<runtime::SpriteComponent>(sel) &&
                           ImGui::MenuItem(AV("sprite_2d").c_str())) {
                    w->add<runtime::SpriteComponent>(sel, runtime::SpriteComponent{});
                    added = true;
                } else if (!w->has<runtime::TilemapComponent>(sel) &&
                           ImGui::MenuItem(AV("tilemap_2d").c_str())) {
                    w->add<runtime::TilemapComponent>(sel, runtime::TilemapComponent{});
                    added = true;
                } else if (!w->has<runtime::RigidBody2DComponent>(sel) &&
                           ImGui::MenuItem(AV("rigidbody_2d").c_str())) {
                    w->add<runtime::RigidBody2DComponent>(sel, runtime::RigidBody2DComponent{});
                    added = true;
                } else if (!w->has<runtime::Collider2DComponent>(sel) &&
                           ImGui::MenuItem(AV("collider_2d").c_str())) {
                    w->add<runtime::Collider2DComponent>(sel, runtime::Collider2DComponent{});
                    added = true;
                } else if (!w->has<physics::RigidBodyComponent>(sel) &&
                           ImGui::MenuItem(AV("rigid_body").c_str())) {
                    added = app.set_rigid_body(sel, physics::RigidBodyComponent{}, add_err);
                } else if (!w->has<physics::ColliderComponent>(sel) &&
                           ImGui::MenuItem(AV("collider").c_str())) {
                    added = app.set_collider(sel, physics::ColliderComponent{}, add_err);
                } else if (!w->has<runtime::DestructibleComponent>(sel) &&
                           ImGui::MenuItem(AV("destructible").c_str())) {
                    added = app.set_destructible(sel, runtime::DestructibleComponent{}, add_err);
                } else if (!w->has<vfx::ParticleComponent>(sel) &&
                           ImGui::MenuItem(AV("particles").c_str())) {
                    added = app.attach_particles(sel, add_err);
                } else if (!w->has<physics::ClothComponent>(sel) &&
                           ImGui::MenuItem(AV("cloth").c_str())) {
                    added = app.attach_cloth(sel, add_err);
                } else if (!w->has<physics::CharacterComponent>(sel) &&
                           ImGui::MenuItem(AV("character").c_str())) {
                    added = app.attach_character(sel, add_err);
                } else if (!w->has<runtime::NavMeshComponent>(sel) &&
                           ImGui::MenuItem(AV("navmesh").c_str())) {
                    added = app.attach_navmesh(sel, add_err);
                } else if (!w->has<runtime::NavAgentComponent>(sel) &&
                           ImGui::MenuItem(AV("nav_agent").c_str())) {
                    added = app.attach_nav_agent(sel, add_err);
                } else if (!w->has<runtime::NavRouteComponent>(sel) &&
                           ImGui::MenuItem(AV("nav_route").c_str())) {
                    added = app.attach_nav_route(sel, add_err);
                } else if (!w->has<runtime::TerrainComponent>(sel) &&
                           ImGui::MenuItem(AV("terrain").c_str())) {
                    added = app.attach_terrain(sel, add_err);
                } else if (!w->has<runtime::SquadComponent>(sel) &&
                           ImGui::MenuItem(AV("squad").c_str())) {
                    added = app.attach_squad(sel, add_err);
                } else if (!w->has<runtime::JammerComponent>(sel) &&
                           ImGui::MenuItem(AV("jammer").c_str())) {
                    added = app.attach_jammer(sel, add_err);
                } else if (!w->has<physics::VehicleComponent>(sel) &&
                           ImGui::MenuItem(AV("vehicle").c_str())) {
                    added = app.attach_vehicle(sel, add_err);
                } else if (!w->has<runtime::CableComponent>(sel) &&
                           ImGui::MenuItem(AV("cable_physics").c_str())) {
                    added = app.attach_cable(sel, add_err);
                } else if (!w->has<runtime::WeatherComponent>(sel) &&
                           ImGui::MenuItem(AV("weather_system").c_str())) {
                    added = app.attach_weather(sel, add_err);
                } else if (!w->has<runtime::RuleTileComponent>(sel) &&
                           ImGui::MenuItem(AV("rule_tile").c_str())) {
                    added = app.attach_rule_tile(sel, add_err);
                } else if (!w->has<runtime::FlowFieldComponent>(sel) &&
                           ImGui::MenuItem(AV("flow_field").c_str())) {
                    added = app.attach_flow_field(sel, add_err);
                } else if (!w->has<runtime::TalentComponent>(sel) &&
                           ImGui::MenuItem(AV("talent_tree").c_str())) {
                    added = app.attach_talent_tree(sel, add_err);
                } else if (!w->has<runtime::RagdollComponent>(sel) &&
                           ImGui::MenuItem(AV("ragdoll_blend").c_str())) {
                    added = app.attach_ragdoll(sel, add_err);
                } else if (!w->has<runtime::InventoryComponent>(sel) &&
                           ImGui::MenuItem(AV("grid_inventory").c_str())) {
                    added = app.attach_inventory(sel, add_err);
                } else if (!w->has<runtime::SurfaceAudioComponent>(sel) &&
                           ImGui::MenuItem(AV("surface_audio").c_str())) {
                    added = app.attach_surface_audio(sel, add_err);
                }
                if (added) {
                    ic.error.clear();
                    ic.valid = false;
                } else if (!add_err.empty()) {
                    ic.error = add_err;
                    push_error(app.console(), "Add component failed", add_err);
                }
                ImGui::EndPopup();
            }

            // Quick-Add Component Bar
            ImGui::Spacing();
            ImGui::TextDisabled("%s", AV("quick_add_component").c_str());
            {
                auto attach_pill = [&](const char* label, bool has_it, ImU32 col, auto attach_fn) {
                    if (has_it) {
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 185, 129, 200));
                        std::string checked_label = "v " + std::string(label);
                        ImGui::Button(checked_label.c_str());
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, col);
                        std::string add_label = "+ " + std::string(label);
                        if (ImGui::Button(add_label.c_str())) {
                            attach_fn();
                        }
                    }
                    ImGui::PopStyleColor();
                };

                attach_pill(AV("weather_system").c_str(), ic.weather_has, IM_COL32(14, 116, 144, 200), [&]{
                    std::string err; app.attach_weather(sel, err);
                });
                ImGui::SameLine(0.0f, 4.0f);
                attach_pill(AV("cable_sim").c_str(), ic.cable_has, IM_COL32(180, 83, 9, 200), [&]{
                    std::string err; app.attach_cable(sel, err);
                });
                ImGui::SameLine(0.0f, 4.0f);
                attach_pill(AV("talent_tree").c_str(), ic.talent_has, IM_COL32(190, 24, 93, 200), [&]{
                    std::string err; app.attach_talent_tree(sel, err);
                });
                ImGui::SameLine(0.0f, 4.0f);
                attach_pill(AV("flow_field").c_str(), ic.flow_has, IM_COL32(21, 128, 61, 200), [&]{
                    std::string err; app.attach_flow_field(sel, err);
                });

                attach_pill(AV("rule_tile").c_str(), ic.rule_tile_has, IM_COL32(4, 120, 87, 200), [&]{
                    std::string err; app.attach_rule_tile(sel, err);
                });
                ImGui::SameLine(0.0f, 4.0f);
                attach_pill(AV("ragdoll").c_str(), ic.ragdoll_has, IM_COL32(185, 28, 28, 200), [&]{
                    std::string err; app.attach_ragdoll(sel, err);
                });
                ImGui::SameLine(0.0f, 4.0f);
                attach_pill(AV("inventory").c_str(), ic.inv_has, IM_COL32(109, 40, 217, 200), [&]{
                    std::string err; app.attach_inventory(sel, err);
                });
                ImGui::SameLine(0.0f, 4.0f);
                attach_pill(AV("surface_audio").c_str(), ic.sa_has, IM_COL32(146, 64, 14, 200), [&]{
                    std::string err; app.attach_surface_audio(sel, err);
                });
            }
            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::CollapsingHeader((AV("name") + "###Name").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SetNextItemWidth(-86.0f);
                ImGui::InputText("##name", ic.name, sizeof(ic.name));
                ImGui::SameLine();
                if (ImGui::Button((AV("apply") + "##name").c_str())) {
                    std::string err;
                    if (!app.rename_entity(sel, ic.name, err)) {
                        ic.error = err;
                        push_error(app.console(), "Rename failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
            }
            if (w->has<scene::Transform>(sel) && ImGui::CollapsingHeader((AV("transform") + "###Transform").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                bool t_changed = false;
                t_changed |= draw_vec3_control(AV("position").c_str(), ic.pos, 0.0f, 0.05f);
                t_changed |= draw_vec3_control(AV("rotation").c_str(), ic.rot, 0.0f, 0.5f);
                t_changed |= draw_vec3_control(AV("scale").c_str(), ic.scl, 1.0f, 0.02f, 0.01f, 1000.0f);

                // Quick Transform Actions: Copy, Paste, Reset
                ImGui::Spacing();
                static struct {
                    float pos[3] = {0.0f, 0.0f, 0.0f};
                    float rot[3] = {0.0f, 0.0f, 0.0f};
                    float scl[3] = {1.0f, 1.0f, 1.0f};
                    bool valid = false;
                } s_tr_clip;

                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                if (ImGui::Button((AV("copy_transform") + "###btn_copy_tr").c_str())) {
                    s_tr_clip.pos[0] = ic.pos[0]; s_tr_clip.pos[1] = ic.pos[1]; s_tr_clip.pos[2] = ic.pos[2];
                    s_tr_clip.rot[0] = ic.rot[0]; s_tr_clip.rot[1] = ic.rot[1]; s_tr_clip.rot[2] = ic.rot[2];
                    s_tr_clip.scl[0] = ic.scl[0]; s_tr_clip.scl[1] = ic.scl[1]; s_tr_clip.scl[2] = ic.scl[2];
                    s_tr_clip.valid = true;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!s_tr_clip.valid);
                if (ImGui::Button((AV("paste_transform") + "###btn_paste_tr").c_str())) {
                    ic.pos[0] = s_tr_clip.pos[0]; ic.pos[1] = s_tr_clip.pos[1]; ic.pos[2] = s_tr_clip.pos[2];
                    ic.rot[0] = s_tr_clip.rot[0]; ic.rot[1] = s_tr_clip.rot[1]; ic.rot[2] = s_tr_clip.rot[2];
                    ic.scl[0] = s_tr_clip.scl[0]; ic.scl[1] = s_tr_clip.scl[1]; ic.scl[2] = s_tr_clip.scl[2];
                    t_changed = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button((AV("reset_transform") + "###btn_reset_tr").c_str())) {
                    ic.pos[0] = 0.0f; ic.pos[1] = 0.0f; ic.pos[2] = 0.0f;
                    ic.rot[0] = 0.0f; ic.rot[1] = 0.0f; ic.rot[2] = 0.0f;
                    ic.scl[0] = 1.0f; ic.scl[1] = 1.0f; ic.scl[2] = 1.0f;
                    t_changed = true;
                }
                ImGui::PopStyleVar();

                auto* cur_tr = w->get<scene::Transform>(sel);
                if (cur_tr != nullptr && t_changed) {
                    cur_tr->local_x = ic.pos[0];
                    cur_tr->local_y = ic.pos[1];
                    cur_tr->local_z = ic.pos[2];
                    cur_tr->rot_x = ic.rot[0];
                    cur_tr->rot_y = ic.rot[1];
                    cur_tr->rot_z = ic.rot[2];
                    cur_tr->scale_x = ic.scl[0];
                    cur_tr->scale_y = ic.scl[1];
                    cur_tr->scale_z = ic.scl[2];
                    cur_tr->dirty = true;
                    scene::propagate_transforms(*w);
                }

                if (const auto* t = w->get<scene::Transform>(sel)) {
                    // A parent name is user-authored text (may be Arabic):
                    // compose LOGICAL then shape once (concatenating two
                    // shaped strings would freeze LTR order). TextUnformatted
                    // keeps a shaped '%' in the name from being read as a
                    // format specifier.
                    rt_text_str(ui::shape_arabic(
                        ui::tr("parent_prefix") + " : " +
                        std::string(t->parent.valid() ? entity_label_ptr(app, t->parent)
                                                      : ui::tr("inspector_root"))));
                }
                const bool apply_btn = ImGui::Button((AV("apply") + "##transform").c_str());
                const bool drag_finished = ImGui::IsItemDeactivatedAfterEdit();
                if (apply_btn || drag_finished) {
                    TransformEdit e;
                    e.px = ic.pos[0];
                    e.py = ic.pos[1];
                    e.pz = ic.pos[2];
                    e.rx = ic.rot[0];
                    e.ry = ic.rot[1];
                    e.rz = ic.rot[2];
                    e.sx = ic.scl[0];
                    e.sy = ic.scl[1];
                    e.sz = ic.scl[2];
                    std::string err;
                    if (!app.set_transform(sel, e, err)) {
                        ic.error = err;
                        push_error(app.console(), "Transform edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
            }
            if (const auto* m = w->get<runtime::MeshComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("mesh") + "###Mesh").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::InputText(AV("asset_id").c_str(), ic.mesh_id, sizeof(ic.mesh_id));
                    // Shared material assignment (combo over known .nfmat paths).
                    std::vector<std::string> mat_list = app.known_materials();
                    {
                        const std::string cur = m->material.empty() ? runtime::kDefaultMaterialPath
                                                                    : m->material;
                        if (std::find(mat_list.begin(), mat_list.end(), cur) == mat_list.end()) {
                            mat_list.insert(mat_list.begin(), cur);
                        }
                    }
                    if (ic.mat_choice < 0 ||
                        ic.mat_choice >= static_cast<int>(mat_list.size())) {
                        ic.mat_choice = 0;
                    }
                    {
                        const std::string cur = m->material.empty() ? runtime::kDefaultMaterialPath
                                                                    : m->material;
                        for (int i = 0; i < static_cast<int>(mat_list.size()); ++i) {
                            if (mat_list[i] == cur) {
                                ic.mat_choice = i;
                                break;
                            }
                        }
                    }
                    auto mat_combo_item = [](void* data, int idx) -> const char* {
                        const auto* list = static_cast<const std::vector<std::string>*>(data);
                        if (idx < 0 || idx >= static_cast<int>(list->size())) {
                            return "";
                        }
                        return (*list)[static_cast<size_t>(idx)].c_str();
                    };
                    ImGui::SetNextItemWidth(-185.0f);
                    ImGui::Combo((AV("material") + "##material_asset").c_str(), &ic.mat_choice,
                                 mat_combo_item, &mat_list,
                                 static_cast<int>(mat_list.size()));
                    ImGui::SameLine();
                    if (ImGui::Button((AV("assign") + "##material").c_str())) {
                        std::string err;
                        if (!app.set_entity_material(sel, mat_list[static_cast<size_t>(ic.mat_choice)],
                                                     err)) {
                            ic.error = err;
                            push_error(app.console(), "Material assign failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false; // refresh params from the new material
                        }
                    }
                    std::string state_val = AV("asset_state_not_requested");
                    auto handle = app.asset_manager().find(m->mesh_id);
                    if (handle) {
                        switch (handle->state) {
                            case assets::AssetState::Unloaded: state_val = AV("asset_state_unloaded"); break;
                            case assets::AssetState::Loading: state_val = AV("asset_state_loading"); break;
                            case assets::AssetState::Ready: state_val = AV("asset_state_ready"); break;
                            case assets::AssetState::Failed:
                                state_val = AV("failed") + ": " + handle->error;
                                break;
                        }
                    }
                    rt_disabled_str(AV("status") + ": " + state_val);
                    if (ImGui::Button((AV("apply") + "##mesh").c_str())) {
                        std::string err;
                        if (!app.set_mesh(sel, ic.mesh_id, ic.mesh_mat, err)) {
                            ic.error = err;
                            push_error(app.console(), "Mesh edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // Shared PBR material of the selected mesh (scalar params, v0.1).
            if (w->has<runtime::MeshComponent>(sel) &&
                ImGui::CollapsingHeader((AV("material") + "###Material").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("%s", ic.mat_path);
                if (app.runtime() != nullptr && app.runtime()->material_dirty(ic.mat_path)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "*");
                }
                // Live edit: every slider tick writes straight through to the
                // viewport (no undo entry); releasing the pointer folds the
                // whole drag into one undo step. The explicit Apply button
                // below stays for keyboard/explicit workflows.
                bool mat_tweaked = false;
                mat_tweaked |= ImGui::ColorEdit3(AV("base_color").c_str(), ic.mat_base);
                mat_tweaked |= ImGui::SliderFloat(AV("metallic").c_str(), &ic.mat_metal, 0.0f, 1.0f);
                mat_tweaked |= ImGui::SliderFloat(AV("roughness").c_str(), &ic.mat_rough, 0.0f, 1.0f);
                mat_tweaked |= ImGui::SliderFloat(AV("ao").c_str(), &ic.mat_ao, 0.0f, 1.0f);
                mat_tweaked |= ImGui::ColorEdit3(AV("emission").c_str(), ic.mat_em);
                mat_tweaked |=
                    ImGui::SliderFloat(AV("emission_strength").c_str(), &ic.mat_estr, 0.0f, 8.0f);
                if (mat_tweaked) {
                    if (app.runtime() == nullptr) {
                        ic.error = "No runtime";
                    } else {
                        if (!ic.mat_live) {
                            ic.mat_before = read_material_params(*app.runtime(), ic.mat_path);
                            ic.mat_live = true;
                        }
                        std::string err;
                        if (!app.preview_material_params(ic.mat_path,
                                                         material_edit_from_cache(ic), err)) {
                            ic.error = err;
                            push_error(app.console(), "Material edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
                if (ic.mat_live && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    std::string err;
                    if (!app.commit_material_params(ic.mat_path, ic.mat_before, err)) {
                        ic.error = err;
                        push_error(app.console(), "Material edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                    ic.mat_live = false;
                }
                // Albedo texture picker ("None (scalar)" + known images).
                std::vector<std::string> tex_list;
                tex_list.emplace_back();
                for (const auto& t : app.known_textures()) {
                    tex_list.push_back(t);
                }
                {
                    const std::string cur =
                        (app.runtime() != nullptr) ? app.material_albedo(ic.mat_path) : std::string{};
                    ic.albedo_choice = 0;
                    for (int i = 0; i < static_cast<int>(tex_list.size()); ++i) {
                        if (tex_list[i] == cur) {
                            ic.albedo_choice = i;
                            break;
                        }
                    }
                }
                auto tex_combo_item = [](void* data, int idx) -> const char* {
                    const auto* list = static_cast<const std::vector<std::string>*>(data);
                    if (idx < 0 || idx >= static_cast<int>(list->size())) {
                        return "";
                    }
                    const std::string& s = (*list)[static_cast<size_t>(idx)];
                    return s.empty() ? "None (scalar)" : s.c_str();
                };
                ImGui::SetNextItemWidth(-185.0f);
                ImGui::Combo(AV("albedo").c_str(), &ic.albedo_choice, tex_combo_item, &tex_list,
                             static_cast<int>(tex_list.size()));
                // Albedo swatch: the same thumbnail the browser shows, drawn
                // next to the slot so a chosen texture is visible before the
                // viewport re-renders. "None (scalar)" draws nothing.
                if (ic.albedo_choice > 0 && app.preview_texture) {
                    const std::string& chosen = tex_list[static_cast<size_t>(ic.albedo_choice)];
                    const uintptr_t pid = app.preview_texture(chosen);
                    if (pid != 0) {
                        ImGui::SameLine();
                        ImGui::Image(static_cast<ImTextureID>(pid), ImVec2(24, 24));
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("apply") + "##albedo").c_str())) {
                    std::string err;
                    if (!app.set_material_albedo(
                            ic.mat_path, tex_list[static_cast<size_t>(ic.albedo_choice)], err)) {
                        ic.error = err;
                        push_error(app.console(), "Albedo assign failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
                // Mip filter picker — rebinds the cached sampler, no re-upload.
                // The combo order mirrors rhi::MipMapMode (None=0, Nearest=1, Linear=2).
                {
                    const rhi::MipMapMode cur = (app.runtime() != nullptr)
                                                    ? app.material_mip_mode(ic.mat_path)
                                                    : rhi::MipMapMode::Linear;
                    ic.mip_choice = static_cast<int>(cur);
                }
                // Items are widget labels: owned AV strings joined with NULs so
                // the pointer outlives the Combo call.
                const std::string mip_items =
                    AV("none") + '\0' + AV("nearest") + '\0' + AV("linear") + '\0';
                ImGui::SetNextItemWidth(-185.0f);
                ImGui::Combo(AV("mip_filter").c_str(), &ic.mip_choice, mip_items.c_str());
                ImGui::SameLine();
                if (ImGui::Button((AV("apply") + "##mip").c_str())) {
                    std::string err;
                    if (!app.set_material_mip_mode(
                            ic.mat_path, static_cast<rhi::MipMapMode>(ic.mip_choice), err)) {
                        ic.error = err;
                        push_error(app.console(), "Mip filter assign failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
                if (ImGui::Button((AV("apply") + "##material_params").c_str())) {
                    std::string err;
                    if (!app.set_material_params(ic.mat_path, material_edit_from_cache(ic), err)) {
                        ic.error = err;
                        push_error(app.console(), "Material edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(AV("save_material").c_str())) {
                    std::string err;
                    if (!app.save_material(ic.mat_path, ic.mat_path, err)) {
                        ic.error = err;
                        push_error(app.console(), "Material save failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
            }
            if (const auto* pl = w->get<scene::PrefabLinkComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("prefab") + "###Prefab").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextWrapped("%s", pl->prefab_path.c_str());
                    if (ImGui::Button(AV("apply_to_prefab").c_str())) {
                        std::string err;
                        if (!app.apply_prefab(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Prefab apply failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(AV("revert_to_prefab").c_str())) {
                        std::string err;
                        if (!app.revert_prefab(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Prefab revert failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false; // reselects a fresh handle
                        }
                    }
                }
            }
            bool has_cam = w->has<runtime::CameraComponent>(sel);
            if (has_cam && ImGui::CollapsingHeader((AV("camera") + "###Camera").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox(AV("active").c_str(), &ic.cam_active);
                ImGui::DragFloat(AV("fov").c_str(), &ic.cam[0], 0.5f, 1.0f, 179.0f);
                ImGui::DragFloat(AV("near").c_str(), &ic.cam[1], 0.01f, 0.001f, 100.0f);
                ImGui::DragFloat(AV("far").c_str(), &ic.cam[2], 1.0f, 0.01f, 10000.0f);
                if (ImGui::Button((AV("apply") + "##camera").c_str())) {
                    CameraEdit e;
                    e.active = ic.cam_active;
                    e.fov_y = ic.cam[0];
                    e.near_plane = ic.cam[1];
                    e.far_plane = ic.cam[2];
                    std::string err;
                    if (!app.set_camera(sel, e, err)) {
                        ic.error = err;
                        push_error(app.console(), "Camera edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
            }
            // --- Weapon (Phase 44) -------------------------------------
            //
            // Two states, like every optional component: present = edit it,
            // absent = one button that adds a default rifle. The button goes
            // through the same validated command, so "add" and "edit" cannot
            // disagree about what a valid weapon is.
            if (!ic.weapon_has) {
                if (ImGui::Button((AV("add_weapon") + "##addweapon").c_str())) {
                    std::string err;
                    if (!app.set_weapon(sel, runtime::WeaponProfile{}, err)) {
                        ic.error = err;
                        push_error(app.console(), "Weapon add failed", err);
                    } else {
                        ic.valid = false; // re-read the cache from the new component
                    }
                }
            }
            if (ic.weapon_has &&
                ImGui::CollapsingHeader((AV("weapon") + "###Weapon").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                const std::string semi = AV("fire_semi");
                const std::string auto_fire = AV("fire_auto");
                const std::string burst = AV("fire_burst");
                const char* modes[] = {semi.c_str(), auto_fire.c_str(), burst.c_str()};
                int mode = static_cast<int>(ic.weapon.mode);
                if (ImGui::Combo(AV("fire_mode").c_str(), &mode, modes, 3)) {
                    ic.weapon.mode = static_cast<runtime::FireMode>(mode);
                }
                // Damage type (Phase 52): the same closed set the scene format
                // stores, so the editor cannot author a value the loader refuses.
                const std::string ballistic = AV("damage_ballistic");
                const std::string explosive = AV("damage_explosive");
                const std::string fire = AV("damage_fire");
                const char* damage_types[] = {ballistic.c_str(), explosive.c_str(),
                                              fire.c_str()};
                int damage_type = static_cast<int>(ic.weapon.damage_type);
                if (ImGui::Combo(AV("damage_type").c_str(), &damage_type, damage_types, 3)) {
                    ic.weapon.damage_type = static_cast<runtime::DamageType>(damage_type);
                }
                ImGui::DragFloat(AV("rpm").c_str(), &ic.weapon.rounds_per_minute, 5.0f, 1.0f,
                                 runtime::kMaxRoundsPerMinute);
                int magazine = static_cast<int>(ic.weapon.magazine);
                if (ImGui::DragInt(AV("magazine").c_str(), &magazine, 1.0f, 1, 9999)) {
                    ic.weapon.magazine = static_cast<u32>(magazine);
                }
                int reserve = static_cast<int>(ic.weapon.reserve);
                if (ImGui::DragInt(AV("reserve").c_str(), &reserve, 1.0f, 0, 99999)) {
                    ic.weapon.reserve = static_cast<u32>(reserve);
                }
                ImGui::DragFloat(AV("reload_time").c_str(), &ic.weapon.reload_seconds, 0.02f,
                                 0.0f, 30.0f, "%.2f s");
                ImGui::DragFloat(AV("damage").c_str(), &ic.weapon.damage, 0.5f, 0.0f, 100000.0f);
                ImGui::DragFloat(AV("range").c_str(), &ic.weapon.range, 1.0f, 0.1f, 5000.0f);
                // Armor penetration (Phase 50): a fraction, so the slider is
                // the whole valid range and the loader refuses anything else.
                ImGui::DragFloat(AV("armor_penetration").c_str(), &ic.weapon.armor_penetration,
                                 0.01f, 0.0f, 1.0f, "%.2f");
                // Radial blast (Phase 51). Zero radius is the disabled state;
                // the damage and edge fields stay visible so an author can see
                // the whole blast while tuning it.
                ImGui::DragFloat(AV("blast_radius").c_str(), &ic.weapon.blast_radius, 0.1f,
                                 0.0f, 1000.0f, "%.1f m");
                ImGui::DragFloat(AV("blast_damage").c_str(), &ic.weapon.blast_damage, 1.0f,
                                 0.0f, 1000000.0f);
                ImGui::DragFloat(AV("blast_min").c_str(), &ic.weapon.blast_min_scale, 0.01f,
                                 0.0f, 1.0f, "%.2f");
                // Status on hit (Phase 54): none by default, so a weapon that
                // never authored it keeps the pre-Phase-54 behaviour.
                const std::string no_status = AV("status_none");
                const std::string burn = AV("status_burn");
                const std::string bleed = AV("status_bleed");
                const std::string corrode = AV("status_corrode");
                const char* statuses[] = {no_status.c_str(), burn.c_str(), bleed.c_str(),
                                          corrode.c_str()};
                int status = static_cast<int>(ic.weapon.status);
                if (ImGui::Combo(AV("status_effect").c_str(), &status, statuses, 4)) {
                    ic.weapon.status = static_cast<runtime::StatusKind>(status);
                }
                if (ic.weapon.status != runtime::StatusKind::None) {
                    ImGui::DragFloat(AV("status_dps").c_str(),
                                     &ic.weapon.status_damage_per_second, 0.1f, 0.01f,
                                     1000000.0f, "%.2f /s");
                    ImGui::DragFloat(AV("status_time").c_str(), &ic.weapon.status_seconds,
                                     0.1f, 0.01f, 3600.0f, "%.2f s");
                    ImGui::DragFloat(AV("status_tick").c_str(), &ic.weapon.status_tick_seconds,
                                     0.05f, 0.01f, 60.0f, "%.2f s");
                }
                // Delivery (Phase 46): the same trigger and magazine, two ways
                // for the round to reach the target. The flight keys are shown
                // only for a projectile because a hitscan line ignores them.
                const std::string hitscan = AV("delivery_hitscan");
                const std::string projectile = AV("delivery_projectile");
                const char* deliveries[] = {hitscan.c_str(), projectile.c_str()};
                int delivery = static_cast<int>(ic.weapon.delivery);
                if (ImGui::Combo(AV("delivery").c_str(), &delivery, deliveries, 2)) {
                    ic.weapon.delivery = static_cast<runtime::WeaponDelivery>(delivery);
                }
                if (ic.weapon.delivery == runtime::WeaponDelivery::Projectile) {
                    ImGui::DragFloat(AV("projectile_speed").c_str(),
                                     &ic.weapon.projectile_speed, 1.0f, 1.0f, 5000.0f,
                                     "%.1f m/s");
                    ImGui::DragFloat(AV("projectile_gravity").c_str(),
                                     &ic.weapon.projectile_gravity, 0.05f, 0.0f, 100.0f,
                                     "%.2f m/s2");
                }
                int pellets = static_cast<int>(ic.weapon.pellets);
                if (ImGui::DragInt(AV("pellets").c_str(), &pellets, 1.0f, 1, 64)) {
                    ic.weapon.pellets = static_cast<u32>(pellets);
                }
                ImGui::DragFloat(AV("spread").c_str(), &ic.weapon.spread_deg, 0.02f, 0.0f, 45.0f,
                                 "%.2f deg");
                ImGui::DragFloat(AV("spread_max").c_str(), &ic.weapon.spread_max_deg, 0.02f, 0.0f,
                                 45.0f, "%.2f deg");
                ImGui::DragFloat(AV("recoil_pitch").c_str(), &ic.weapon.recoil_pitch, 0.02f, 0.0f,
                                 20.0f, "%.2f deg");
                ImGui::DragFloat(AV("recoil_yaw").c_str(), &ic.weapon.recoil_yaw, 0.02f, 0.0f,
                                 20.0f, "%.2f deg");
                ImGui::Checkbox((AV("auto_reload") + "##weapon").c_str(), &ic.weapon.auto_reload);
                ImGui::Checkbox((AV("enabled") + "##weapon").c_str(), &ic.weapon.enabled);
                if (ImGui::Button((AV("apply") + "##weapon").c_str())) {
                    std::string err;
                    if (!app.set_weapon(sel, ic.weapon, err)) {
                        ic.error = err;
                        push_error(app.console(), "Weapon edit failed", err);
                    } else {
                        ic.error.clear();
                        ic.valid = false; // the command rebuilt the live state
                    }
                }
            }
            // --- Hit points (Phase 44) ---------------------------------
            if (!ic.health_has) {
                if (ImGui::Button((AV("add_health") + "##addhealth").c_str())) {
                    std::string err;
                    if (!app.set_health(sel, runtime::HealthComponent{}, err)) {
                        ic.error = err;
                        push_error(app.console(), "Health add failed", err);
                    } else {
                        ic.valid = false;
                    }
                }
            }
            if (ic.health_has &&
                ImGui::CollapsingHeader((AV("health") + "###Health").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat(AV("max_health").c_str(), &ic.health.max_health, 1.0f, 0.001f,
                                 1000000.0f);
                ImGui::DragFloat(AV("health_now").c_str(), &ic.health.health, 1.0f, 0.0f,
                                 ic.health.max_health);
                // Armor (Phase 50). A maximum of zero means "no armor", which is
                // exactly the pre-Phase-50 component, so the dependent fields
                // stay hidden until an author creates a pool.
                ImGui::DragFloat(AV("armor_max").c_str(), &ic.health.max_armor, 1.0f, 0.0f,
                                 1000000.0f);
                if (ic.health.max_armor > 0.0f) {
                    ImGui::DragFloat(AV("armor_now").c_str(), &ic.health.armor, 1.0f, 0.0f,
                                     ic.health.max_armor);
                    ImGui::DragFloat(AV("armor_absorb").c_str(), &ic.health.armor_absorb, 0.01f,
                                     0.0f, 1.0f, "%.2f");
                }
                ImGui::Checkbox((AV("hit_zones") + "##health").c_str(), &ic.health.hit_zones);
                if (ic.health.hit_zones) {
                    ImGui::DragFloat(AV("headshot_multiplier").c_str(),
                                     &ic.health.headshot_multiplier, 0.05f, 0.001f, 100.0f,
                                     "x%.2f");
                    ImGui::DragFloat(AV("limb_multiplier").c_str(), &ic.health.limb_multiplier,
                                     0.01f, 0.0f, 100.0f, "x%.2f");
                }
                // Per-type resistance (Phase 52). A fraction, not a percentage:
                // the same 0..1 contract as armor absorption.
                ImGui::DragFloat(AV("resist_ballistic").c_str(),
                                 &ic.health.ballistic_resistance, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat(AV("resist_explosive").c_str(),
                                 &ic.health.explosive_resistance, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat(AV("resist_fire").c_str(), &ic.health.fire_resistance, 0.01f,
                                 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat(AV("regen").c_str(), &ic.health.regen_per_second, 0.1f, 0.0f,
                                 1000.0f, "%.2f /s");
                ImGui::DragFloat(AV("regen_delay").c_str(), &ic.health.regen_delay, 0.1f, 0.0f,
                                 600.0f, "%.1f s");
                ImGui::Checkbox((AV("invulnerable") + "##health").c_str(), &ic.health.invulnerable);
                ImGui::Checkbox((AV("enabled") + "##health").c_str(), &ic.health.enabled);
                if (ImGui::Button((AV("apply") + "##health").c_str())) {
                    std::string err;
                    if (!app.set_health(sel, ic.health, err)) {
                        ic.error = err;
                        push_error(app.console(), "Health edit failed", err);
                    } else {
                        ic.error.clear();
                        ic.valid = false;
                    }
                }
            }
            // --- Combat agent (Phase 46) -------------------------------
            //
            // Phase 45 made an NPC fight; this is the first place an author
            // can configure one without typing a scene line. The target is a
            // name field rather than an entity picker because the scene format
            // stores names, and a name survives save/load.
            if (!ic.agent_has) {
                if (ImGui::Button((AV("add_combat_agent") + "##addagent").c_str())) {
                    std::string err;
                    if (!app.set_combat_agent(sel, runtime::CombatAgentComponent{}, err)) {
                        ic.error = err;
                        push_error(app.console(), "Combat agent add failed", err);
                    } else {
                        ic.valid = false;
                    }
                }
            }
            if (ic.agent_has &&
                ImGui::CollapsingHeader((AV("combat_agent") + "###CombatAgent").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("auto_target") + "##agent").c_str(),
                                &ic.agent_auto_target);
                ImGui::Checkbox((AV("retaliate") + "##agent").c_str(),
                                &ic.agent.retaliate);
                ImGui::Text("%s: %.2f", AV("suppression").c_str(),
                            ic.agent.suppression.level);
                ImGui::BeginDisabled(ic.agent_auto_target);
                ImGui::InputText(AV("target").c_str(), ic.agent_target,
                                 sizeof(ic.agent_target));
                ImGui::EndDisabled();
                ImGui::DragFloat(AV("agent_eye").c_str(), &ic.agent.eye_height, 0.05f, 0.0f,
                                 10.0f, "%.2f m");
                ImGui::DragFloat(AV("agent_aim_height").c_str(), &ic.agent.target_height,
                                 0.05f, 0.0f, 10.0f, "%.2f m");
                ImGui::DragFloat(AV("agent_reaction").c_str(),
                                 &ic.agent.config.reaction_seconds, 0.02f, 0.0f, 10.0f,
                                 "%.2f s");
                ImGui::DragFloat(AV("agent_turn").c_str(),
                                 &ic.agent.config.aim_speed_deg_per_s, 5.0f, 0.0f, 3600.0f,
                                 "%.0f deg/s");
                ImGui::DragFloat(AV("agent_cone").c_str(), &ic.agent.config.fire_cone_deg,
                                 0.1f, 0.1f, 90.0f, "%.1f deg");
                ImGui::DragFloat(AV("agent_range").c_str(), &ic.agent.config.max_range, 1.0f,
                                 0.1f, 10000.0f, "%.1f m");
                ImGui::DragFloat(AV("agent_burst").c_str(),
                                 &ic.agent.config.burst_on_seconds, 0.05f, 0.0f, 60.0f,
                                 "%.2f s");
                ImGui::DragFloat(AV("agent_rest").c_str(),
                                 &ic.agent.config.burst_off_seconds, 0.05f, 0.0f, 60.0f,
                                 "%.2f s");
                ImGui::Checkbox((AV("enabled") + "##agent").c_str(), &ic.agent.enabled);
                if (ImGui::Button((AV("apply") + "##agent").c_str())) {
                    ic.agent.target = ic.agent_target;
                    ic.agent.auto_target = ic.agent_auto_target;
                    std::string err;
                    if (!app.set_combat_agent(sel, ic.agent, err)) {
                        ic.error = err;
                        push_error(app.console(), "Combat agent edit failed", err);
                    } else {
                        ic.error.clear();
                        ic.valid = false;
                    }
                }
            }
            // --- Faction (Phase 46) ------------------------------------
            //
            // A team is a short token, not an entity reference. Empty means
            // neutral, and the loader writes no line for it, so adding the
            // component is harmless until the author names a side.
            if (!ic.faction_has) {
                if (ImGui::Button((AV("add_faction") + "##addfaction").c_str())) {
                    std::string err;
                    if (!app.set_faction(sel, runtime::FactionComponent{}, err)) {
                        ic.error = err;
                        push_error(app.console(), "Faction add failed", err);
                    } else {
                        ic.valid = false;
                    }
                }
            }
            if (ic.faction_has &&
                ImGui::CollapsingHeader((AV("faction") + "###Faction").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::InputText(AV("team").c_str(), ic.faction_team,
                                 sizeof(ic.faction_team));
                ImGui::Checkbox((AV("friendly_fire") + "##faction").c_str(),
                                &ic.faction_friendly_fire);
                ImGui::Checkbox((AV("enabled") + "##faction").c_str(),
                                &ic.faction_enabled);
                if (ImGui::Button((AV("apply") + "##faction").c_str())) {
                    ic.faction.team = ic.faction_team;
                    ic.faction.friendly_fire = ic.faction_friendly_fire;
                    ic.faction.enabled = ic.faction_enabled;
                    std::string err;
                    if (!app.set_faction(sel, ic.faction, err)) {
                        ic.error = err;
                        push_error(app.console(), "Faction edit failed", err);
                    } else {
                        ic.error.clear();
                        ic.valid = false;
                    }
                }
            }
            if (w->has<runtime::DirectionalLight>(sel) &&
                ImGui::CollapsingHeader((AV("directional_light") + "###DirectionalLight").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3(AV("direction").c_str(), ic.light_dir, 0.02f);
                ImGui::ColorEdit3(AV("color").c_str(), ic.light_color);
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(245, 158, 11, 180));
                    if (ImGui::Button((AV("preset_sun") + "###psun_dl").c_str())) {
                        ic.light_color[0] = 1.0f; ic.light_color[1] = 0.95f; ic.light_color[2] = 0.82f;
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(239, 68, 68, 180));
                    if (ImGui::Button((AV("preset_fire") + "###pfire_dl").c_str())) {
                        ic.light_color[0] = 1.0f; ic.light_color[1] = 0.55f; ic.light_color[2] = 0.20f;
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(6, 182, 212, 180));
                    if (ImGui::Button((AV("preset_neon") + "###pneon_dl").c_str())) {
                        ic.light_color[0] = 0.15f; ic.light_color[1] = 0.85f; ic.light_color[2] = 1.0f;
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 185, 129, 180));
                    if (ImGui::Button((AV("preset_emerald") + "###pemerald_dl").c_str())) {
                        ic.light_color[0] = 0.20f; ic.light_color[1] = 0.95f; ic.light_color[2] = 0.45f;
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(99, 102, 241, 180));
                    if (ImGui::Button((AV("preset_moon") + "###pmoon_dl").c_str())) {
                        ic.light_color[0] = 0.65f; ic.light_color[1] = 0.78f; ic.light_color[2] = 1.0f;
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar();
                }
                ImGui::DragFloat(AV("intensity").c_str(), &ic.light_intensity, 0.05f, 0.0f, 16.0f);
                ImGui::Checkbox(AV("cast_shadows").c_str(), &ic.light_shadows);
                ImGui::SliderFloat(AV("shadow_strength").c_str(), &ic.light_shadow_strength, 0.0f, 1.0f);
                ImGui::DragFloat(AV("shadow_bias").c_str(), &ic.light_shadow_bias, 0.00005f, 0.0f, 0.01f,
                                 "%.5f");
                // Cascades are whole tiles of the atlas, so this is a slider, not
                // a drag: the only meaningful values are 1..4 and every step
                // changes how the atlas is spent. Distance 0 is the documented
                // "cast to the camera's far plane" sentinel, and the upper bound
                // is deliberately generous — the useful range depends on the
                // scene's scale, not on anything the editor can know.
                ImGui::SliderInt(AV("shadow_cascades").c_str(), &ic.light_shadow_cascades, 1,
                                 static_cast<int>(rendering::kMaxShadowCascades));
                ImGui::DragFloat(AV("shadow_distance").c_str(), &ic.light_shadow_distance, 1.0f,
                                 0.0f, 1000.0f, "%.1f");
                if (ImGui::Button((AV("apply") + "##light").c_str())) {
                    LightEdit e;
                    e.dir_x = ic.light_dir[0];
                    e.dir_y = ic.light_dir[1];
                    e.dir_z = ic.light_dir[2];
                    e.color_r = ic.light_color[0];
                    e.color_g = ic.light_color[1];
                    e.color_b = ic.light_color[2];
                    e.intensity = ic.light_intensity;
                    e.cast_shadows = ic.light_shadows;
                    e.shadow_strength = ic.light_shadow_strength;
                    e.shadow_bias = ic.light_shadow_bias;
                    e.shadow_cascades = ic.light_shadow_cascades;
                    e.shadow_distance = ic.light_shadow_distance;
                    std::string err;
                    if (!app.set_light(sel, e, err)) {
                        ic.error = err;
                        push_error(app.console(), "Light edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
            }
            // --- Day/night cycle (design doc §64) ---
if (ImGui::CollapsingHeader((AV("time_of_day") + "###TimeOfDay").c_str())) {
    if (!ic.tod_has) {
        ImGui::TextDisabled("%s", AV("no_day_night").c_str());
        if (ImGui::Button((AV("add_day_night") + "##addtod").c_str())) {
            TimeOfDayEdit e; // defaults; the command adds the component
            std::string err;
            if (!app.set_time_of_day(sel, e, err)) {
                ic.error = err;
                push_error(app.console(), "Add day/night failed", err);
            } else {
                ic.error.clear();
            }
        }
    } else {
        ImGui::Checkbox((AV("enabled") + "##tod_enabled").c_str(), &ic.tod_enabled);
        // 0..24 with a step of 0.25: an author setting a golden hour wants
        // "17.5", not a spinner they have to nudge to 17.5.
        ImGui::SliderFloat((AV("hour") + "##tod_hour").c_str(), &ic.tod_hours, 0.0f, 24.0f);
        ImGui::SliderFloat((AV("day_length") + "##tod_len").c_str(), &ic.tod_day_length, 0.0f, 600.0f);
        ImGui::Checkbox((AV("drive_light") + "##tod_drive").c_str(), &ic.tod_drive_light);
        if (ImGui::Button((AV("apply") + "##tod").c_str())) {
            TimeOfDayEdit e;
            e.time_hours = ic.tod_hours;
            e.day_length_seconds = ic.tod_day_length;
            e.enabled = ic.tod_enabled;
            e.drive_light = ic.tod_drive_light;
            std::string err;
            if (!app.set_time_of_day(sel, e, err)) {
                ic.error = err;
                push_error(app.console(), "Apply day/night failed", err);
            } else {
                ic.error.clear();
            }
        }
    }
}

// --- Sky / environment (Phase 13) ---
            if (ImGui::CollapsingHeader((AV("sky") + "###Sky").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.sky_has) {
                    ImGui::TextDisabled("%s", AV("no_sky_settings").c_str());
                    if (ImGui::Button((AV("add_sky_settings") + "##addsky").c_str())) {
                        SkyEdit e; // defaults; command adds the component
                        std::string err;
                        if (!app.set_sky(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Add sky failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##sky_enabled").c_str(), &ic.sky_enabled);
                    ImGui::ColorEdit3(AV("zenith").c_str(), ic.sky_zenith);
                    ImGui::ColorEdit3(AV("horizon").c_str(), ic.sky_horizon);
                    ImGui::ColorEdit3(AV("ground").c_str(), ic.sky_ground);
                    ImGui::ColorEdit3(AV("clear").c_str(), ic.sky_clear);
                    ImGui::SliderFloat(AV("sun_disk").c_str(), &ic.sky_sun_disk, 0.0f, 8.0f);
                    ImGui::SliderFloat(AV("sun_glow").c_str(), &ic.sky_sun_glow, 0.0f, 8.0f);
                    if (ImGui::Button((AV("apply") + "##sky").c_str())) {
                        SkyEdit e;
                        e.zenith[0] = ic.sky_zenith[0];
                        e.zenith[1] = ic.sky_zenith[1];
                        e.zenith[2] = ic.sky_zenith[2];
                        e.horizon[0] = ic.sky_horizon[0];
                        e.horizon[1] = ic.sky_horizon[1];
                        e.horizon[2] = ic.sky_horizon[2];
                        e.ground[0] = ic.sky_ground[0];
                        e.ground[1] = ic.sky_ground[1];
                        e.ground[2] = ic.sky_ground[2];
                        e.clear[0] = ic.sky_clear[0];
                        e.clear[1] = ic.sky_clear[1];
                        e.clear[2] = ic.sky_clear[2];
                        e.sun_disk = ic.sky_sun_disk;
                        e.sun_glow = ic.sky_sun_glow;
                        e.enabled = ic.sky_enabled;
                        std::string err;
                        if (!app.set_sky(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Sky edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // --- Post-processing (design §206) ---
            if (ImGui::CollapsingHeader((AV("post_process") + "###PostProcess").c_str())) {
                if (!ic.pp_has) {
                    ImGui::TextDisabled("%s", AV("no_post_process").c_str());
                    if (ImGui::Button((AV("add_post_process") + "##addpp").c_str())) {
                        // The component's defaults are the renderer's neutral
                        // values, so adding it changes nothing until a stage is
                        // switched on — the author sees the panel appear, not
                        // the picture change.
                        runtime::PostProcessComponent e{};
                        std::string err;
                        if (!app.set_post_process(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Add post-processing failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                } else {
                    // Bloom. The threshold and the knee are expressed in raw HDR
                    // units, so the slider ceilings are well above 1: a value of
                    // 1 is "only brighter than a fully lit white surface blooms",
                    // and the interesting range for a stylized look starts there.
                    ImGui::Checkbox((AV("enabled") + "##pp_bloom").c_str(), &ic.pp_bloom_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("bloom").c_str());
                    ImGui::SliderFloat((AV("bloom_threshold") + "##pp_bloom_threshold").c_str(),
                                       &ic.pp_bloom_threshold, 0.0f, 8.0f);
                    ImGui::SliderFloat((AV("bloom_knee") + "##pp_bloom_knee").c_str(), &ic.pp_bloom_knee,
                                       0.0f, 2.0f);
                    ImGui::SliderFloat((AV("bloom_intensity") + "##pp_bloom_intensity").c_str(),
                                       &ic.pp_bloom_intensity, 0.0f, 4.0f);
                    ImGui::SliderFloat((AV("bloom_radius") + "##pp_bloom_radius").c_str(), &ic.pp_bloom_radius,
                                       0.25f, 3.0f);

                    ImGui::Separator();
                    ImGui::Checkbox((AV("enabled") + "##pp_grade").c_str(), &ic.pp_grade_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("color_grade").c_str());
                    ImGui::SliderFloat((AV("contrast") + "##pp_grade_contrast").c_str(), &ic.pp_grade_contrast,
                                       0.0f, 4.0f);
                    ImGui::SliderFloat((AV("pivot") + "##pp_grade_pivot").c_str(), &ic.pp_grade_pivot, 0.0f, 4.0f);
                    ImGui::SliderFloat((AV("temperature") + "##pp_grade_temperature").c_str(),
                                       &ic.pp_grade_temperature, -1.0f, 1.0f);
                    ImGui::SliderFloat((AV("tint") + "##pp_grade_tint").c_str(), &ic.pp_grade_tint, -1.0f, 1.0f);
                    ImGui::SliderFloat((AV("gamma") + "##pp_grade_gamma").c_str(), &ic.pp_grade_gamma, 0.2f, 3.0f);

                    ImGui::Separator();
                    ImGui::Checkbox((AV("enabled") + "##pp_sharpen").c_str(), &ic.pp_sharpen_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("sharpen").c_str());
                    ImGui::SliderFloat((AV("sharpen_amount") + "##pp_sharpen_amount").c_str(),
                                       &ic.pp_sharpen_amount, 0.0f, 4.0f);
                    ImGui::SliderFloat((AV("sharpen_radius") + "##pp_sharpen_radius").c_str(),
                                       &ic.pp_sharpen_radius, 0.25f, 4.0f);

                    ImGui::Separator();
                    ImGui::SliderFloat((AV("saturation") + "##pp_saturation").c_str(), &ic.pp_saturation, 0.0f,
                                       4.0f);
                    ImGui::SliderFloat((AV("vignette") + "##pp_vignette").c_str(), &ic.pp_vignette, 0.0f, 1.0f);

                    ImGui::Separator();
                    ImGui::Checkbox((AV("enabled") + "##pp_lens").c_str(), &ic.pp_lens_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("lens_effects").c_str());
                    // Symmetric: barrel and pincushion are both legitimate, and
                    // the sign is what picks between them.
                    ImGui::SliderFloat((AV("distortion") + "##pp_lens_distortion").c_str(),
                                       &ic.pp_lens_distortion, -0.5f, 0.5f);
                    // Three decimals: a subtle fringe is around 0.01, so a
                    // two-decimal display would quantise the useful range away.
                    ImGui::SliderFloat((AV("chromatic_aberration") + "##pp_lens_chroma").c_str(),
                                       &ic.pp_lens_chroma, 0.0f, 0.1f, "%.3f");

                    ImGui::Separator();
                    ImGui::Checkbox((AV("enabled") + "##pp_dof").c_str(), &ic.pp_dof_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("depth_of_field").c_str());
                    // The focus distance reaches far on purpose: focusing on the
                    // far plane is how an author says "nothing is out of focus".
                    ImGui::SliderFloat((AV("focus_distance") + "##pp_dof_focus").c_str(),
                                       &ic.pp_dof_focus, 0.1f, 500.0f);
                    // A range at or below zero divides by zero in the ramp, so
                    // the slider's floor is a real bound, not a preference.
                    ImGui::SliderFloat((AV("focus_range") + "##pp_dof_range").c_str(),
                                       &ic.pp_dof_range, 0.01f, 50.0f);
                    ImGui::SliderFloat((AV("blur_radius") + "##pp_dof_radius").c_str(),
                                       &ic.pp_dof_radius, 0.0f, 24.0f);

                    ImGui::Separator();
                    ImGui::Checkbox((AV("enabled") + "##pp_motion").c_str(), &ic.pp_motion_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("motion_blur").c_str());
                    ImGui::SliderFloat((AV("intensity") + "##pp_motion_intensity").c_str(),
                                       &ic.pp_motion_intensity, 0.0f, 4.0f);
                    // In uv units, so the useful range is small: 0.05 is a
                    // smear across a twentieth of the frame.
                    ImGui::SliderFloat((AV("max_length") + "##pp_motion_length").c_str(),
                                       &ic.pp_motion_length, 0.001f, 0.3f, "%.3f");

                    ImGui::Separator();
                    // Anti-aliasing. The two sliders are the FXAA 3.11 console
                    // knobs: an edge must span `threshold` of the local luma to
                    // be filtered, and `floor` keeps a dark gradient from being
                    // read as one. Lower thresholds filter more and soften more.
                    ImGui::Checkbox((AV("enabled") + "##pp_fxaa").c_str(), &ic.pp_fxaa_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("anti_aliasing").c_str());
                    ImGui::SliderFloat((AV("edge_threshold") + "##pp_fxaa_threshold").c_str(),
                                       &ic.pp_fxaa_threshold, 0.01f, 1.0f, "%.3f");
                    ImGui::SliderFloat((AV("edge_floor") + "##pp_fxaa_floor").c_str(),
                                       &ic.pp_fxaa_threshold_min, 0.0f, 0.5f, "%.4f");

                    ImGui::Separator();
                    // Internal resolution. The same bounds the loader and the
                    // renderer use; the slider steps in hundredths because a
                    // scale change rebuilds every resolution-dependent target.
                    ImGui::TextUnformatted(AV("render_scale").c_str());
                    ImGui::SliderFloat((AV("render_scale") + "##pp_render_scale").c_str(),
                                       &ic.pp_render_scale, rendering::kMinRenderScale,
                                       rendering::kMaxRenderScale, "%.2f");
                    if (ic.pp_render_scale > 1.0f) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", AV("supersampling").c_str());
                    }
                    ImGui::TextDisabled("%s", AV("render_scale_hint").c_str());

                    // Dynamic resolution. The static scale is the ceiling, so
                    // the floor slider cannot exceed it; clamping here keeps
                    // the UI from presenting a pair the command will reject.
                    if (ic.pp_min_render_scale > ic.pp_render_scale) {
                        ic.pp_min_render_scale = ic.pp_render_scale;
                    }
                    ImGui::Checkbox((AV("dynamic_resolution") + "##pp_dynamic").c_str(),
                                    &ic.pp_dynamic_resolution);
                    if (ic.pp_dynamic_resolution) {
                        ImGui::SliderFloat((AV("target_fps") + "##pp_target_fps").c_str(),
                                           &ic.pp_target_fps, rendering::kMinTargetFps,
                                           rendering::kMaxTargetFps, "%.0f");
                        const float floor_max =
                            std::max(rendering::kMinRenderScale, ic.pp_render_scale);
                        ImGui::SliderFloat((AV("min_scale") + "##pp_min_scale").c_str(),
                                           &ic.pp_min_render_scale,
                                           rendering::kMinRenderScale, floor_max, "%.2f");
                        // The load clock is scene data: GPU is a preference
                        // with a CPU fallback, not a requirement that could
                        // leave a scene unscaled on a device without timestamps.
                        const std::string timing_cpu = AV("timing_cpu");
                        const std::string timing_gpu = AV("timing_gpu");
                        const char* timing_items[] = {timing_cpu.c_str(),
                                                      timing_gpu.c_str()};
                        ImGui::Combo((AV("timing_source") + "##pp_dynamic_timing").c_str(),
                                     &ic.pp_dynamic_resolution_timing, timing_items, 2);
                        ImGui::TextDisabled("%s",
                                            AV("dynamic_resolution_timing_hint").c_str());
                        // The window policy replaces the per-frame governor as
                        // the decision source; it is not a third clock.
                        ImGui::Checkbox((AV("frame_policy") + "##pp_frame_policy").c_str(),
                                        &ic.pp_frame_policy);
                        ImGui::TextDisabled("%s", AV("frame_policy_hint").c_str());
                    }
                    ImGui::TextDisabled("%s", AV("dynamic_resolution_hint").c_str());

                    ImGui::Separator();
                    // Exposure and the tonemap operator are OPT-IN, and their
                    // sentinels are the UI: a slider at 0 means "the renderer's
                    // own exposure", and the combo's first entry is "Not set".
                    // An unconditional write would make every scene claim an
                    // exposure and an operator it never chose.
                    ImGui::SliderFloat((AV("exposure") + "##pp_exposure").c_str(),
                                       &ic.pp_exposure, 0.0f, 8.0f);
                    if (ic.pp_exposure <= 0.0f) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", AV("renderer_default").c_str());
                    }
                    {
                        const std::string t0 = AV("tonemap_not_set");
                        const std::string t1 = AV("tonemap_exponential");
                        const std::string t2 = AV("tonemap_aces");
                        const std::string t3 = AV("tonemap_reinhard");
                        const std::string t4 = AV("tonemap_linear");
                        const char* names[] = {t0.c_str(), t1.c_str(), t2.c_str(), t3.c_str(),
                                               t4.c_str()};
                        int combo = ic.pp_tonemap + 1; // -1 -> index 0
                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::Combo((AV("tonemap") + "##pp_tonemap").c_str(), &combo, names,
                                         5)) {
                            ic.pp_tonemap = combo - 1;
                        }
                    }
                    // The LUT is a kLutSize^3 cube laid out as a strip image.
                    // The path is not validated in the panel: the runtime
                    // resolves it through the VFS and says so when it cannot,
                    // which is the only place that knows whether it exists.
                    ImGui::SetNextItemWidth(280.0f);
                    ImGui::InputText((AV("color_lut") + "##pp_lut_path").c_str(), ic.pp_lut_path,
                                     sizeof(ic.pp_lut_path));
                    ImGui::SliderFloat((AV("lut_strength") + "##pp_lut_strength").c_str(),
                                       &ic.pp_lut_strength, 0.0f, 1.0f);

                    ImGui::Separator();
                    ImGui::Checkbox((AV("enabled") + "##pp_fog").c_str(), &ic.pp_fog_enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(AV("distance_fog").c_str());
                    ImGui::ColorEdit3((AV("fog_color") + "##pp_fog_color").c_str(),
                                      ic.pp_fog_color);
                    ImGui::SliderFloat((AV("fog_start") + "##pp_fog_start").c_str(),
                                       &ic.pp_fog_start, 0.0f, 500.0f);
                    ImGui::SliderFloat((AV("fog_end") + "##pp_fog_end").c_str(), &ic.pp_fog_end,
                                       1.0f, 1000.0f);
                    // Keep the band ordered while the user drags: the Inspector
                    // refuses end <= start, and a slider that creates a value the
                    // same panel rejects is a trap.
                    if (ic.pp_fog_end <= ic.pp_fog_start) {
                        ic.pp_fog_end = ic.pp_fog_start + 1.0f;
                    }

                    ImGui::TextDisabled("%s", AV("post_process_hint").c_str());
                    if (ImGui::Button((AV("apply") + "##pp_apply").c_str())) {
                        runtime::PostProcessComponent e{};
                        e.bloom_enabled = ic.pp_bloom_enabled;
                        e.bloom_threshold = ic.pp_bloom_threshold;
                        e.bloom_knee = ic.pp_bloom_knee;
                        e.bloom_intensity = ic.pp_bloom_intensity;
                        e.bloom_radius = ic.pp_bloom_radius;
                        e.grade_enabled = ic.pp_grade_enabled;
                        e.grade_contrast = ic.pp_grade_contrast;
                        e.grade_pivot = ic.pp_grade_pivot;
                        e.grade_temperature = ic.pp_grade_temperature;
                        e.grade_tint = ic.pp_grade_tint;
                        e.grade_gamma = ic.pp_grade_gamma;
                        e.sharpen_enabled = ic.pp_sharpen_enabled;
                        e.sharpen_amount = ic.pp_sharpen_amount;
                        e.sharpen_radius = ic.pp_sharpen_radius;
                        e.saturation = ic.pp_saturation;
                        e.vignette = ic.pp_vignette;
                        e.lens_enabled = ic.pp_lens_enabled;
                        e.lens_distortion = ic.pp_lens_distortion;
                        e.lens_chromatic_aberration = ic.pp_lens_chroma;
                        e.dof_enabled = ic.pp_dof_enabled;
                        e.dof_focus_distance = ic.pp_dof_focus;
                        e.dof_focus_range = ic.pp_dof_range;
                        e.dof_max_radius = ic.pp_dof_radius;
                        e.motion_enabled = ic.pp_motion_enabled;
                        e.motion_intensity = ic.pp_motion_intensity;
                        e.motion_max_length = ic.pp_motion_length;
                        e.fxaa_enabled = ic.pp_fxaa_enabled;
                        e.fxaa_threshold = ic.pp_fxaa_threshold;
                        e.fxaa_threshold_min = ic.pp_fxaa_threshold_min;
                        e.render_scale = ic.pp_render_scale;
                        e.dynamic_resolution = ic.pp_dynamic_resolution;
                        e.target_fps = ic.pp_target_fps;
                        e.min_render_scale = ic.pp_min_render_scale;
                        e.dynamic_resolution_timing = ic.pp_dynamic_resolution_timing;
                        e.frame_policy = ic.pp_frame_policy;
                        e.exposure = ic.pp_exposure;
                        e.tonemap = ic.pp_tonemap;
                        e.lut_path = ic.pp_lut_path;
                        e.lut_strength = ic.pp_lut_strength;
                        e.fog_enabled = ic.pp_fog_enabled;
                        e.fog_r = ic.pp_fog_color[0];
                        e.fog_g = ic.pp_fog_color[1];
                        e.fog_b = ic.pp_fog_color[2];
                        e.fog_start = ic.pp_fog_start;
                        e.fog_end = ic.pp_fog_end;
                        std::string err;
                        if (!app.set_post_process(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Post-processing edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // --- 2D Sprite / Billboard (Phase 73) ---
            if (auto* sp = w->get<runtime::SpriteComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("sprite_2d") + "###Sprite2D").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    static char tex_buf[256];
                    std::strncpy(tex_buf, sp->texture.c_str(), sizeof(tex_buf) - 1);
                    tex_buf[sizeof(tex_buf) - 1] = '\0';
                    if (ImGui::InputText(AV("texture").c_str(), tex_buf, sizeof(tex_buf))) {
                        sp->texture = tex_buf;
                    }
                    ImGui::DragFloat2(AV("size").c_str(), &sp->width, 0.05f, 0.01f, 1000.0f);
                    float col[4] = {sp->color_r, sp->color_g, sp->color_b, sp->color_a};
                    if (ImGui::ColorEdit4(AV("color").c_str(), col)) {
                        sp->color_r = col[0]; sp->color_g = col[1]; sp->color_b = col[2]; sp->color_a = col[3];
                    }
                    ImGui::Checkbox(AV("flip_x").c_str(), &sp->flip_x);
                    ImGui::SameLine();
                    ImGui::Checkbox(AV("flip_y").c_str(), &sp->flip_y);
                    ImGui::DragInt(AV("z_order").c_str(), &sp->z_order, 1, -1000, 1000);
                    ImGui::Checkbox(AV("billboard").c_str(), &sp->billboard);
                    ImGui::SameLine();
                    ImGui::Checkbox(AV("enabled").c_str(), &sp->enabled);
                }
            }
            // --- 2D Tilemap (Phase 74) ---
            if (auto* tm = w->get<runtime::TilemapComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("tilemap_2d") + "###Tilemap2D").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    static char tm_buf[256];
                    std::strncpy(tm_buf, tm->tileset_texture.c_str(), sizeof(tm_buf) - 1);
                    tm_buf[sizeof(tm_buf) - 1] = '\0';
                    if (ImGui::InputText(AV("texture").c_str(), tm_buf, sizeof(tm_buf))) {
                        tm->tileset_texture = tm_buf;
                    }
                    ImGui::DragFloat(AV("tile_size").c_str(), &tm->tile_size, 1.0f, 1.0f, 512.0f);
                    int cols = static_cast<int>(tm->columns);
                    int rws = static_cast<int>(tm->rows);
                    if (ImGui::DragInt(AV("columns").c_str(), &cols, 1, 1, 1024)) {
                        if (cols > 0) tm->columns = static_cast<uint32_t>(cols);
                    }
                    if (ImGui::DragInt(AV("rows").c_str(), &rws, 1, 1, 1024)) {
                        if (rws > 0) tm->rows = static_cast<uint32_t>(rws);
                    }
                    ImGui::DragInt(AV("z_order").c_str(), &tm->z_order, 1, -1000, 1000);
                    ImGui::Checkbox(AV("generate_collider").c_str(), &tm->generate_collider);
                    ImGui::SameLine();
                    ImGui::Checkbox(AV("enabled").c_str(), &tm->enabled);
                }
            }
            // --- 2D RigidBody (Phase 74) ---
            if (auto* rb2 = w->get<runtime::RigidBody2DComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("rigidbody_2d") + "###RigidBody2D").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    const char* body_types[] = {"Dynamic", "Static", "Kinematic"};
                    int cur_type = rb2->body_type;
                    if (cur_type < 0 || cur_type > 2) cur_type = 0;
                    if (ImGui::Combo(AV("type").c_str(), &cur_type, body_types, 3)) {
                        rb2->body_type = cur_type;
                    }
                    if (cur_type == 0) {
                        ImGui::DragFloat(AV("mass").c_str(), &rb2->mass, 0.1f, 0.01f, 10000.0f);
                    }
                    ImGui::DragFloat(AV("linear_damping").c_str(), &rb2->linear_damping, 0.01f, 0.0f, 100.0f);
                    ImGui::DragFloat(AV("angular_damping").c_str(), &rb2->angular_damping, 0.01f, 0.0f, 100.0f);
                    ImGui::Checkbox(AV("fixed_rotation").c_str(), &rb2->fixed_rotation);
                    ImGui::SameLine();
                    ImGui::Checkbox(AV("enabled").c_str(), &rb2->enabled);
                }
            }
            // --- 2D Collider (Phase 74) ---
            if (auto* col2 = w->get<runtime::Collider2DComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("collider_2d") + "###Collider2D").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    const char* shapes[] = {"Box", "Circle"};
                    int cur_shape = col2->shape;
                    if (cur_shape < 0 || cur_shape > 1) cur_shape = 0;
                    if (ImGui::Combo(AV("shape").c_str(), &cur_shape, shapes, 2)) {
                        col2->shape = cur_shape;
                    }
                    if (cur_shape == 0) {
                        ImGui::DragFloat2(AV("size").c_str(), &col2->size_x, 0.05f, 0.01f, 1000.0f);
                    } else {
                        ImGui::DragFloat(AV("radius").c_str(), &col2->radius, 0.05f, 0.01f, 500.0f);
                    }
                    ImGui::DragFloat(AV("friction").c_str(), &col2->friction, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat(AV("restitution").c_str(), &col2->restitution, 0.01f, 0.0f, 1.0f);
                    ImGui::Checkbox(AV("is_sensor").c_str(), &col2->is_sensor);
                    ImGui::SameLine();
                    ImGui::Checkbox(AV("enabled").c_str(), &col2->enabled);
                }
            }
            if (auto* pl = w->get<runtime::PointLightComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("point_light") + "###PointLight").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    float col[3] = {pl->color_r, pl->color_g, pl->color_b};
                    if (ImGui::ColorEdit3(AV("color").c_str(), col)) {
                        pl->color_r = col[0]; pl->color_g = col[1]; pl->color_b = col[2];
                    }
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(245, 158, 11, 180));
                        if (ImGui::Button((AV("preset_sun") + "###psun_pl").c_str())) {
                            pl->color_r = 1.0f; pl->color_g = 0.95f; pl->color_b = 0.82f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(239, 68, 68, 180));
                        if (ImGui::Button((AV("preset_fire") + "###pfire_pl").c_str())) {
                            pl->color_r = 1.0f; pl->color_g = 0.55f; pl->color_b = 0.20f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(6, 182, 212, 180));
                        if (ImGui::Button((AV("preset_neon") + "###pneon_pl").c_str())) {
                            pl->color_r = 0.15f; pl->color_g = 0.85f; pl->color_b = 1.0f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 185, 129, 180));
                        if (ImGui::Button((AV("preset_emerald") + "###pemerald_pl").c_str())) {
                            pl->color_r = 0.20f; pl->color_g = 0.95f; pl->color_b = 0.45f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(99, 102, 241, 180));
                        if (ImGui::Button((AV("preset_moon") + "###pmoon_pl").c_str())) {
                            pl->color_r = 0.65f; pl->color_g = 0.78f; pl->color_b = 1.0f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopStyleVar();
                    }
                    ImGui::DragFloat(AV("intensity").c_str(), &pl->intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat(AV("radius").c_str(), &pl->radius, 0.2f, 0.1f, 1000.0f);
                    ImGui::Checkbox(AV("cast_shadows").c_str(), &pl->cast_shadows);
                    ImGui::SameLine();
                    ImGui::Checkbox(AV("enabled").c_str(), &pl->enabled);
                }
            }
            if (auto* sl = w->get<runtime::SpotLightComponent>(sel)) {
                if (ImGui::CollapsingHeader((AV("spot_light") + "###SpotLight").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    float col[3] = {sl->color_r, sl->color_g, sl->color_b};
                    if (ImGui::ColorEdit3(AV("color").c_str(), col)) {
                        sl->color_r = col[0]; sl->color_g = col[1]; sl->color_b = col[2];
                    }
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(245, 158, 11, 180));
                        if (ImGui::Button((AV("preset_sun") + "###psun_sl").c_str())) {
                            sl->color_r = 1.0f; sl->color_g = 0.95f; sl->color_b = 0.82f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(239, 68, 68, 180));
                        if (ImGui::Button((AV("preset_fire") + "###pfire_sl").c_str())) {
                            sl->color_r = 1.0f; sl->color_g = 0.55f; sl->color_b = 0.20f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(6, 182, 212, 180));
                        if (ImGui::Button((AV("preset_neon") + "###pneon_sl").c_str())) {
                            sl->color_r = 0.15f; sl->color_g = 0.85f; sl->color_b = 1.0f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 185, 129, 180));
                        if (ImGui::Button((AV("preset_emerald") + "###pemerald_sl").c_str())) {
                            sl->color_r = 0.20f; sl->color_g = 0.95f; sl->color_b = 0.45f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(99, 102, 241, 180));
                        if (ImGui::Button((AV("preset_moon") + "###pmoon_sl").c_str())) {
                            sl->color_r = 0.65f; sl->color_g = 0.78f; sl->color_b = 1.0f;
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopStyleVar();
                    }
                    ImGui::DragFloat(AV("intensity").c_str(), &sl->intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat(AV("range").c_str(), &sl->range, 0.2f, 0.1f, 1000.0f);
                    float inner_deg = sl->inner_angle_rad * (180.0f / 3.14159265f);
                    float outer_deg = sl->outer_angle_rad * (180.0f / 3.14159265f);
                    if (ImGui::SliderFloat(AV("inner_angle").c_str(), &inner_deg, 1.0f, 89.0f, "%.1f deg")) {
                        sl->inner_angle_rad = inner_deg * (3.14159265f / 180.0f);
                    }
                    if (ImGui::SliderFloat(AV("outer_angle").c_str(), &outer_deg, 1.0f, 90.0f, "%.1f deg")) {
                        sl->outer_angle_rad = outer_deg * (3.14159265f / 180.0f);
                    }
                    ImGui::Checkbox(AV("cast_shadows").c_str(), &sl->cast_shadows);
                    ImGui::SameLine();
                    ImGui::Checkbox(AV("enabled").c_str(), &sl->enabled);
                }
            }

            // --- Projected decal (Phase 36) ---
            // A decal has no mesh and no light: it projects the entity's local
            // +/-Z box onto whatever the GBuffer already drew. Placement and
            // facing therefore stay in the Transform section above; this panel
            // owns the projection extent and the blend trim.
            if (ImGui::CollapsingHeader((AV("decal") + "###Decal").c_str())) {
                if (!ic.decal_has) {
                    ImGui::TextDisabled("%s", AV("no_decal").c_str());
                    if (ImGui::Button((AV("add_decal") + "##adddecal").c_str())) {
                        DecalEdit e; // defaults: 1x1x1, fully opaque, enabled
                        std::string err;
                        if (!app.set_decal(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Add decal failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##decal_enabled").c_str(),
                                    &ic.decal_enabled);
                    ImGui::InputText((AV("material") + "##decal_material").c_str(),
                                     ic.decal_material, sizeof(ic.decal_material));
                    ImGui::DragFloat3((AV("decal_size") + "##decal_size").c_str(),
                                      ic.decal_size, 0.05f, 0.001f, 1000.0f, "%.3f");
                    ImGui::SliderFloat((AV("decal_opacity") + "##decal_opacity").c_str(),
                                       &ic.decal_opacity, 0.0f, 1.0f);
                    ImGui::TextDisabled("%s", AV("decal_hint").c_str());
                    if (ImGui::Button((AV("apply") + "##decal").c_str())) {
                        DecalEdit e;
                        std::strncpy(e.material, ic.decal_material, sizeof(e.material) - 1);
                        e.material[sizeof(e.material) - 1] = '\0';
                        e.size_x = ic.decal_size[0];
                        e.size_y = ic.decal_size[1];
                        e.size_z = ic.decal_size[2];
                        e.opacity = ic.decal_opacity;
                        e.enabled = ic.decal_enabled;
                        std::string err;
                        if (!app.set_decal(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Decal edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // --- Scene audio environment: reverb zone / music / ambience ------
            // Three sections rather than one: a scene can have a cave that
            // echoes and no music, or music and no zone. Each component is
            // absent until the author asks for it, which is what keeps its line
            // out of the saved file — the same contract the Sky and
            // PostProcess sections above follow.
            if (ImGui::CollapsingHeader((AV("reverb_zone") + "###ReverbZone").c_str())) {
                if (!ic.rz_has) {
                    ImGui::TextDisabled("%s", AV("no_reverb_zone").c_str());
                    if (ImGui::Button((AV("add_reverb_zone") + "##addrz").c_str())) {
                        std::string err;
                        if (!app.set_reverb_zone(sel, audio::ReverbZoneComponent{}, err)) {
                            ic.error = err;
                            push_error(app.console(), "Add reverb zone failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##rz_enabled").c_str(), &ic.rz_edit.enabled);
                    ImGui::SliderFloat((AV("radius") + "##rz_radius").c_str(), &ic.rz_edit.radius,
                                       0.1f, 200.0f);
                    // The inner radius is bounded by the outer one, so its
                    // ceiling follows the slider above rather than a constant:
                    // an inner radius past the outer inverts the falloff, and
                    // the setter refuses it rather than silently clamping.
                    ImGui::SliderFloat((AV("inner_radius") + "##rz_inner").c_str(),
                                       &ic.rz_edit.inner_radius, 0.0f, ic.rz_edit.radius);
                    ImGui::SliderFloat((AV("wet_gain") + "##rz_wet").c_str(), &ic.rz_edit.wet_gain,
                                       0.0f, 1.0f);
                    ImGui::SliderFloat((AV("decay") + "##rz_decay").c_str(),
                                       &ic.rz_edit.decay_seconds, 0.0f, 10.0f);
                    ImGui::SliderFloat((AV("predelay") + "##rz_predelay").c_str(),
                                       &ic.rz_edit.pre_delay_seconds, 0.0f, 0.5f);
                    ImGui::SliderFloat((AV("echo_spacing") + "##rz_spacing").c_str(),
                                       &ic.rz_edit.echo_spacing_seconds, 0.0f, 1.0f);
                    if (ImGui::Button((AV("apply") + "##rz_apply").c_str())) {
                        std::string err;
                        if (!app.set_reverb_zone(sel, ic.rz_edit, err)) {
                            ic.error = err;
                            push_error(app.console(), "Reverb zone edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // Music and ambience need a buffer path, so their empty state offers
            // the path field rather than an add button that could only fail.
            if (ImGui::CollapsingHeader((AV("music") + "###Music").c_str())) {
                if (!ic.mus_has) {
                    ImGui::TextDisabled("%s", AV("no_music").c_str());
                    ImGui::SetNextItemWidth(280.0f);
                    ImGui::InputText((AV("music_buffer") + "##mus_add_path").c_str(), ic.mus_buffer,
                                     sizeof(ic.mus_buffer));
                    if (ImGui::Button((AV("add_music") + "##addmus").c_str())) {
                        MusicEdit e{};
                        std::strncpy(e.buffer, ic.mus_buffer, sizeof(e.buffer) - 1);
                        e.buffer[sizeof(e.buffer) - 1] = '\0';
                        e.volume = ic.mus_volume;
                        e.fade_in_seconds = ic.mus_fade;
                        std::string err;
                        if (!app.set_music(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Add music failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##mus_enabled").c_str(), &ic.mus_enabled);
                    ImGui::SetNextItemWidth(280.0f);
                    ImGui::InputText((AV("music_buffer") + "##mus_path").c_str(), ic.mus_buffer,
                                     sizeof(ic.mus_buffer));
                    ImGui::SliderFloat((AV("volume") + "##mus_volume").c_str(), &ic.mus_volume, 0.0f,
                                       1.0f);
                    ImGui::SliderFloat((AV("fade_in") + "##mus_fade").c_str(), &ic.mus_fade, 0.0f,
                                       10.0f);
                    if (ImGui::Button((AV("apply") + "##mus_apply").c_str())) {
                        MusicEdit e{};
                        std::strncpy(e.buffer, ic.mus_buffer, sizeof(e.buffer) - 1);
                        e.buffer[sizeof(e.buffer) - 1] = '\0';
                        e.volume = ic.mus_volume;
                        e.fade_in_seconds = ic.mus_fade;
                        e.enabled = ic.mus_enabled;
                        std::string err;
                        if (!app.set_music(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Music edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            if (ImGui::CollapsingHeader((AV("ambience") + "###Ambience").c_str())) {
                if (!ic.amb_has) {
                    ImGui::TextDisabled("%s", AV("no_ambience").c_str());
                    ImGui::SetNextItemWidth(280.0f);
                    ImGui::InputText((AV("ambience_buffer") + "##amb_add_path").c_str(),
                                     ic.amb_buffer, sizeof(ic.amb_buffer));
                    if (ImGui::Button((AV("add_ambience") + "##addamb").c_str())) {
                        AmbienceEdit e{};
                        std::strncpy(e.buffer, ic.amb_buffer, sizeof(e.buffer) - 1);
                        e.buffer[sizeof(e.buffer) - 1] = '\0';
                        e.fade_in_seconds = ic.amb_fade;
                        std::string err;
                        if (!app.set_ambience(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Add ambience failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##amb_enabled").c_str(), &ic.amb_enabled);
                    ImGui::SetNextItemWidth(280.0f);
                    ImGui::InputText((AV("ambience_buffer") + "##amb_path").c_str(), ic.amb_buffer,
                                     sizeof(ic.amb_buffer));
                    ImGui::SliderFloat((AV("fade_in") + "##amb_fade").c_str(), &ic.amb_fade, 0.0f,
                                       10.0f);
                    if (ImGui::Button((AV("apply") + "##amb_apply").c_str())) {
                        AmbienceEdit e{};
                        std::strncpy(e.buffer, ic.amb_buffer, sizeof(e.buffer) - 1);
                        e.buffer[sizeof(e.buffer) - 1] = '\0';
                        e.fade_in_seconds = ic.amb_fade;
                        e.enabled = ic.amb_enabled;
                        std::string err;
                        if (!app.set_ambience(sel, e, err)) {
                            ic.error = err;
                            push_error(app.console(), "Ambience edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // --- Physics: RigidBody ---
            // Always visible: the setter is add-or-replace, so Attach authors
            // the component with defaults instead of merely editing it.
            if (ImGui::CollapsingHeader((AV("rigid_body") + "###RigidBody").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!w->has<physics::RigidBodyComponent>(sel)) {
                    ImGui::TextDisabled("%s", AV("no_rigid_body").c_str());
                    if (ImGui::Button((AV("attach") + "##rigidbody").c_str())) {
                        std::string err;
                        if (!app.set_rigid_body(sel, physics::RigidBodyComponent{}, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach rigid body failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                // Order mirrors the BodyType enum — index IS the value.
                const std::string type_items[3] = {AV("rb_static"), AV("rb_dynamic"),
                                                   AV("rb_kinematic")};
                const char* types[] = {type_items[0].c_str(), type_items[1].c_str(),
                                       type_items[2].c_str()};
                ImGui::Combo(AV("type").c_str(), &ic.rb_type, types, 3);
                if (ic.rb_type != 0) { // mass only matters for Dynamic/Kinematic
                    ImGui::DragFloat(AV("mass").c_str(), &ic.rb_mass, 0.1f, 0.001f, 10000.0f);
                }
                ImGui::SliderFloat(AV("friction").c_str(), &ic.rb_friction, 0.0f, 2.0f);
                ImGui::SliderFloat(AV("restitution").c_str(), &ic.rb_restitution, 0.0f, 1.0f);
                ImGui::SliderFloat(AV("linear_damping").c_str(), &ic.rb_lin_damp, 0.0f, 1.0f);
                ImGui::SliderFloat(AV("angular_damping").c_str(), &ic.rb_ang_damp, 0.0f, 1.0f);
                ImGui::Checkbox(AV("allow_sleep").c_str(), &ic.rb_allow_sleep);
                if (ImGui::Button((AV("apply") + "##rigidbody").c_str())) {
                    auto* rb = w->get<physics::RigidBodyComponent>(sel);
                    if (rb) {
                        physics::RigidBodyComponent edited = *rb;
                        edited.type = static_cast<physics::BodyType>(ic.rb_type);
                        edited.mass = ic.rb_mass;
                        edited.friction = ic.rb_friction;
                        edited.restitution = ic.rb_restitution;
                        edited.linear_damping = ic.rb_lin_damp;
                        edited.angular_damping = ic.rb_ang_damp;
                        edited.allow_sleep = ic.rb_allow_sleep;
                        std::string err;
                        if (!app.set_rigid_body(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Rigid body edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
                }
            }
            // --- Physics: Collider ---
            // Always visible, same add-or-replace contract as the body above.
            if (ImGui::CollapsingHeader((AV("collider") + "###Collider").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!w->has<physics::ColliderComponent>(sel)) {
                    ImGui::TextDisabled("%s", AV("no_collider").c_str());
                    if (ImGui::Button((AV("attach") + "##collider").c_str())) {
                        std::string err;
                        if (!app.set_collider(sel, physics::ColliderComponent{}, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach collider failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                // Owned AV strings (not temporaries); order mirrors the shape
                // switch below (0 = sphere, 1 = box, 2 = plane).
                const std::string shape_items[3] = {AV("col_sphere"), AV("col_box"),
                                                    AV("col_plane")};
                const char* shapes[] = {shape_items[0].c_str(), shape_items[1].c_str(),
                                        shape_items[2].c_str()};
                ImGui::Combo(AV("shape").c_str(), &ic.col_shape, shapes, 3);
                if (ic.col_shape == 0) {
                    ImGui::DragFloat(AV("radius").c_str(), &ic.col_radius, 0.05f, 0.001f, 100.0f);
                } else if (ic.col_shape == 1) {
                    ImGui::DragFloat3(AV("half_extents").c_str(), ic.col_half, 0.05f, 0.001f, 100.0f);
                } else {
                    ImGui::DragFloat3(AV("normal").c_str(), ic.col_normal, 0.05f, -1.0f, 1.0f);
                }
                if (ImGui::Button((AV("apply") + "##collider").c_str())) {
                    auto* col = w->get<physics::ColliderComponent>(sel);
                    if (col) {
                        physics::ColliderComponent edited;
                        if (ic.col_shape == 0) {
                            edited.shape = physics::Shape::make_sphere(ic.col_radius);
                        } else if (ic.col_shape == 1) {
                            edited.shape = physics::Shape::make_box(
                                Vec3(ic.col_half[0], ic.col_half[1], ic.col_half[2]));
                        } else {
                            edited.shape = physics::Shape::make_plane(
                                Vec3(ic.col_normal[0], ic.col_normal[1], ic.col_normal[2]));
                        }
                        std::string err;
                        if (!app.set_collider(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Collider edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
                }
            }
            // --- Destruction (Phase 19 authoring, Phase 25 panel) ---
            // Always visible like Sky: Attach authors a default spec, Apply
            // forwards the cached fields through the validated setter.
            if (ImGui::CollapsingHeader((AV("destructible") + "###Destructible").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!w->has<runtime::DestructibleComponent>(sel)) {
                    ImGui::TextDisabled("%s", AV("no_destructible").c_str());
                    if (ImGui::Button((AV("attach") + "##destructible").c_str())) {
                        std::string err;
                        if (!app.set_destructible(sel, runtime::DestructibleComponent{}, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach destructible failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::DragInt(AV("chunks").c_str(), &ic.dst_chunks, 0.2f, 2, 64);
                    ImGui::DragScalar(AV("seed").c_str(), ImGuiDataType_U32, &ic.dst_seed, 1.0f);
                    ImGui::DragFloat(AV("strength").c_str(), &ic.dst_strength, 0.5f, 0.01f,
                                     10000.0f);
                    ImGui::DragFloat(AV("damage_threshold").c_str(), &ic.dst_threshold, 0.2f, 0.0f,
                                     1000.0f);
                    ImGui::DragFloat(AV("blast_radius").c_str(), &ic.dst_blast, 0.05f, 0.01f,
                                     100.0f);
                    ImGui::Checkbox((AV("enabled") + "##dst_enabled").c_str(), &ic.dst_enabled);
                    if (ImGui::Button((AV("apply") + "##destructible").c_str())) {
                        runtime::DestructibleComponent edited;
                        edited.chunks = static_cast<u32>(ic.dst_chunks);
                        edited.seed = ic.dst_seed;
                        edited.strength = ic.dst_strength;
                        edited.damage_threshold = ic.dst_threshold;
                        edited.blast_radius = ic.dst_blast;
                        edited.enabled = ic.dst_enabled;
                        std::string err;
                        if (!app.set_destructible(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Destructible edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##destructible").c_str())) {
                        std::string err;
                        if (!app.detach_destructible(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach destructible failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Combat feedback (Phase 59 authoring) ---
            // The last component that was authorable only by hand-editing the
            // scene file. Always visible like Destruction: Attach authors a
            // default spec, Apply forwards the cached fields through the
            // validated setter, Detach removes the component (and with it the
            // `Feedback:` line the saver writes).
            if (ImGui::CollapsingHeader((AV("feedback") + "###Feedback").c_str())) {
                if (!ic.fb_has) {
                    ImGui::TextDisabled("%s", AV("no_feedback").c_str());
                    if (ImGui::Button((AV("add_feedback") + "##feedback").c_str())) {
                        std::string err;
                        if (!app.set_feedback(sel, runtime::FeedbackComponent{}, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach feedback failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##fb_enabled").c_str(), &ic.fb_enabled);
                    ImGui::SameLine();
                    ImGui::Checkbox((AV("audio") + "##fb_audio").c_str(), &ic.fb_audio);
                    ImGui::SameLine();
                    ImGui::Checkbox((AV("muzzle_light") + "##fb_light").c_str(), &ic.fb_light);

                    ImGui::DragFloat((AV("flash_time") + "##fb").c_str(), &ic.fb_flash_time,
                                     0.002f, 0.002f, 10.0f, "%.3f s");
                    ImGui::DragFloat((AV("flash_size") + "##fb").c_str(), &ic.fb_flash_size,
                                     0.01f, 0.001f, 10.0f, "%.3f m");
                    ImGui::DragFloat((AV("blast_time") + "##fb").c_str(), &ic.fb_blast_time,
                                     0.005f, 0.002f, 10.0f, "%.3f s");
                    ImGui::DragFloat((AV("blast_size") + "##fb").c_str(), &ic.fb_blast_size,
                                     0.02f, 0.001f, 10.0f, "%.3f m");
                    ImGui::DragFloat((AV("tracer_time") + "##fb").c_str(), &ic.fb_tracer_time,
                                     0.005f, 0.002f, 10.0f, "%.3f s");
                    ImGui::DragFloat((AV("tracer_length") + "##fb").c_str(), &ic.fb_tracer_length,
                                     0.1f, 0.01f, 1000.0f, "%.2f m");
                    ImGui::DragFloat((AV("tracer_width") + "##fb").c_str(), &ic.fb_tracer_width,
                                     0.001f, 0.0005f, 1.0f, "%.4f m");
                    ImGui::DragFloat((AV("impact_time") + "##fb").c_str(), &ic.fb_impact_time,
                                     0.05f, 0.01f, 600.0f, "%.2f s");
                    ImGui::DragFloat((AV("impact_size") + "##fb").c_str(), &ic.fb_impact_size,
                                     0.005f, 0.001f, 10.0f, "%.3f m");

                    ImGui::DragFloat((AV("shot_volume") + "##fb").c_str(), &ic.fb_shot_volume,
                                     0.01f, 0.0f, 4.0f, "%.2f");
                    ImGui::DragFloat((AV("impact_volume") + "##fb").c_str(), &ic.fb_impact_volume,
                                     0.01f, 0.0f, 4.0f, "%.2f");
                    ImGui::DragFloat((AV("impact_volume_explosive") + "##fb").c_str(),
                                     &ic.fb_impact_volume_explosive, 0.01f, 0.0f, 4.0f, "%.2f");
                    ImGui::DragFloat((AV("impact_volume_fire") + "##fb").c_str(),
                                     &ic.fb_impact_volume_fire, 0.01f, 0.0f, 4.0f, "%.2f");

                    ImGui::DragFloat((AV("light_intensity") + "##fb").c_str(),
                                     &ic.fb_light_intensity, 0.05f, 0.0f, 1000.0f, "%.2f");
                    ImGui::DragFloat((AV("light_radius") + "##fb").c_str(), &ic.fb_light_radius,
                                     0.1f, 0.01f, 1000.0f, "%.2f m");
                    ImGui::ColorEdit3((AV("light_color") + "##fb").c_str(), ic.fb_light_color);

                    // Caps. The director clamps to 1..4096; DragInt's own range
                    // is the same, so a typed value cannot reach the setter out
                    // of range and be rejected on Apply.
                    ImGui::DragInt((AV("max_flashes") + "##fb").c_str(), &ic.fb_max_flashes,
                                   0.5f, 1, 4096);
                    ImGui::DragInt((AV("max_tracers") + "##fb").c_str(), &ic.fb_max_tracers,
                                   0.5f, 1, 4096);
                    ImGui::DragInt((AV("max_impacts") + "##fb").c_str(), &ic.fb_max_impacts,
                                   0.5f, 1, 4096);
                    ImGui::DragInt((AV("max_sounds") + "##fb").c_str(), &ic.fb_max_sounds,
                                   0.5f, 1, 4096);
                    ImGui::DragInt((AV("max_lights") + "##fb").c_str(), &ic.fb_max_lights,
                                   0.5f, 1, 4096);

                    if (ImGui::Button((AV("apply") + "##feedback").c_str())) {
                        runtime::FeedbackComponent edited;
                        edited.enabled = ic.fb_enabled;
                        edited.audio = ic.fb_audio;
                        edited.flash_light = ic.fb_light;
                        edited.flash_seconds = ic.fb_flash_time;
                        edited.flash_size = ic.fb_flash_size;
                        edited.blast_flash_seconds = ic.fb_blast_time;
                        edited.blast_flash_size = ic.fb_blast_size;
                        edited.tracer_seconds = ic.fb_tracer_time;
                        edited.tracer_length = ic.fb_tracer_length;
                        edited.tracer_width = ic.fb_tracer_width;
                        edited.impact_seconds = ic.fb_impact_time;
                        edited.impact_size = ic.fb_impact_size;
                        edited.shot_volume = ic.fb_shot_volume;
                        edited.impact_volume = ic.fb_impact_volume;
                        edited.impact_volume_explosive = ic.fb_impact_volume_explosive;
                        edited.impact_volume_fire = ic.fb_impact_volume_fire;
                        edited.flash_light_intensity = ic.fb_light_intensity;
                        edited.flash_light_radius = ic.fb_light_radius;
                        edited.flash_light_r = ic.fb_light_color[0];
                        edited.flash_light_g = ic.fb_light_color[1];
                        edited.flash_light_b = ic.fb_light_color[2];
                        edited.max_flashes = static_cast<unsigned>(ic.fb_max_flashes);
                        edited.max_tracers = static_cast<unsigned>(ic.fb_max_tracers);
                        edited.max_impacts = static_cast<unsigned>(ic.fb_max_impacts);
                        edited.max_sounds = static_cast<unsigned>(ic.fb_max_sounds);
                        edited.max_lights = static_cast<unsigned>(ic.fb_max_lights);
                        std::string err;
                        if (!app.set_feedback(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Feedback edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##feedback").c_str())) {
                        std::string err;
                        if (!app.detach_feedback(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach feedback failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Perception (Phase 60) ---
            // The senses an NPC fights with. Always visible like Destruction and
            // Feedback: Attach authors a default sensor, Apply forwards the
            // cached fields through the validated setter, Detach removes the
            // component (and with it the `Perception:` line the saver writes).
            if (ImGui::CollapsingHeader((AV("perception") + "###Perception").c_str())) {
                if (!ic.perc_has) {
                    ImGui::TextDisabled("%s", AV("no_perception").c_str());
                    if (ImGui::Button((AV("add_perception") + "##perception").c_str())) {
                        std::string err;
                        if (!app.set_perception(sel, runtime::PerceptionComponent{}, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach perception failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##perc_enabled").c_str(), &ic.perc_enabled);
                    ImGui::SameLine();
                    ImGui::Checkbox((AV("pain_sense") + "##perc").c_str(), &ic.perc_pain);
                    ImGui::SameLine();
                    ImGui::Checkbox((AV("combat_senses") + "##perc").c_str(), &ic.perc_governs);

                    ImGui::DragFloat((AV("sight_range") + "##perc").c_str(), &ic.perc_sight, 0.2f,
                                     0.0f, 10000.0f, "%.1f m");
                    ImGui::DragFloat((AV("sight_fov") + "##perc").c_str(), &ic.perc_fov, 0.5f, 0.0f,
                                     180.0f, "%.0f deg");
                    ImGui::DragFloat((AV("sight_gain") + "##perc").c_str(), &ic.perc_gain, 0.05f,
                                     0.0f, 1000.0f, "%.2f");
                    ImGui::DragFloat((AV("sight_loss") + "##perc").c_str(), &ic.perc_loss, 0.05f,
                                     0.0f, 1000.0f, "%.2f");
                    ImGui::DragFloat((AV("sight_linger") + "##perc").c_str(), &ic.perc_linger, 0.1f,
                                     0.0f, 3600.0f, "%.1f s");
                    ImGui::DragFloat((AV("hearing_range") + "##perc").c_str(), &ic.perc_hearing,
                                     0.2f, 0.0f, 10000.0f, "%.1f m");
                    ImGui::DragFloat((AV("touch_range") + "##perc").c_str(), &ic.perc_touch, 0.05f,
                                     0.0f, 1000.0f, "%.2f m");
                    ImGui::DragFloat((AV("emit_noise") + "##perc").c_str(), &ic.perc_noise, 0.01f,
                                     0.0f, 1.0f, "%.2f");
                    ImGui::DragFloat((AV("visibility") + "##perc").c_str(), &ic.perc_visible, 0.01f,
                                     0.0f, 1.0f, "%.2f");
                    ImGui::DragFloat((AV("body_radius") + "##perc").c_str(), &ic.perc_radius, 0.02f,
                                     0.0f, 100.0f, "%.2f m");
                    ImGui::DragFloat((AV("eye_height") + "##perc").c_str(), &ic.perc_eye, 0.02f,
                                     0.0f, 100.0f, "%.2f m");

                    // The same cross-field rule the validator calls an error: an
                    // agent whose senses govern its combat with all of them off
                    // can never acquire a target, so the panel says so before
                    // Apply refuses it.
                    if (ic.perc_enabled && ic.perc_governs && ic.perc_pain == false &&
                        ic.perc_sight <= 0.0f && ic.perc_hearing <= 0.0f && ic.perc_touch <= 0.0f) {
                        rt_colored_str(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), AV("perception_no_senses"));
                    }

                    if (ImGui::Button((AV("apply") + "##perception").c_str())) {
                        runtime::PerceptionComponent edited;
                        edited.enabled = ic.perc_enabled;
                        edited.pain = ic.perc_pain;
                        edited.governs_combat = ic.perc_governs;
                        edited.sight_range = ic.perc_sight;
                        edited.sight_half_fov_deg = ic.perc_fov;
                        edited.sight_gain = ic.perc_gain;
                        edited.sight_loss = ic.perc_loss;
                        edited.sight_linger = ic.perc_linger;
                        edited.hearing_range = ic.perc_hearing;
                        edited.touch_range = ic.perc_touch;
                        edited.noise = ic.perc_noise;
                        edited.visibility = ic.perc_visible;
                        edited.radius = ic.perc_radius;
                        edited.eye_height = ic.perc_eye;
                        std::string err;
                        if (!app.set_perception(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Perception edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##perception").c_str())) {
                        std::string err;
                        if (!app.detach_perception(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach perception failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Squad radio (Phase 68) ---
            // The radio shares the contacts this entity personally senses with
            // same-token members inside range. Always visible like Perception:
            // Attach authors a default token and range, Apply validates through
            // the loader's own rules, Detach removes the `Squad:` line.
            if (ImGui::CollapsingHeader((AV("squad") + "###Squad").c_str())) {
                if (!ic.squad_has) {
                    ImGui::TextDisabled("%s", AV("no_squad").c_str());
                    if (ImGui::Button((AV("add_squad") + "##squad").c_str())) {
                        std::string err;
                        if (!app.attach_squad(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach squad radio failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##squad_enabled").c_str(),
                                    &ic.squad_enabled);
                    ImGui::SetNextItemWidth(170.0f);
                    ImGui::InputText(AV("squad_token").c_str(), ic.squad_token,
                                     sizeof(ic.squad_token));
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", AV("squad_token_hint").c_str());
                    }
                    ImGui::DragFloat((AV("squad_range") + "##squad").c_str(), &ic.squad_radio,
                                     0.5f, 0.0f, 10000.0f, "%.1f m");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", AV("squad_range_hint").c_str());
                    }
                    ImGui::DragFloat((AV("squad_lost") + "##squad").c_str(), &ic.squad_lost,
                                     0.1f, 0.0f, 10000.0f, "%.2f s");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", AV("squad_lost_hint").c_str());
                    }
                    ImGui::SliderFloat((AV("squad_cone") + "##squad").c_str(), &ic.squad_cone,
                                       0.0f, 180.0f, "%.0f deg");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", AV("squad_cone_hint").c_str());
                    }
                    if (ImGui::Button((AV("apply") + "##squad").c_str())) {
                        runtime::SquadComponent edited;
                        edited.enabled = ic.squad_enabled;
                        edited.radio = ic.squad_radio;
                        edited.lost = ic.squad_lost;
                        edited.cone = ic.squad_cone;
                        edited.token = std::string{ic.squad_token};
                        std::string err;
                        if (!app.set_squad(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Squad radio edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##squad").c_str())) {
                        std::string err;
                        if (!app.detach_squad(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach squad radio failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Radio jammer (Phase 70) ---
            // The field that degrades squad radios inside its radius. Always
            // visible like Squad: Attach authors the default 12 m field,
            // Apply validates through the loader's own bounds, Detach removes
            // the `Jammer:` line.
            if (ImGui::CollapsingHeader((AV("jammer") + "###Jammer").c_str())) {
                if (!ic.jammer_has) {
                    ImGui::TextDisabled("%s", AV("no_jammer").c_str());
                    if (ImGui::Button((AV("add_jammer") + "##jammer").c_str())) {
                        std::string err;
                        if (!app.attach_jammer(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach jammer failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##jammer_enabled").c_str(),
                                    &ic.jammer_enabled);
                    ImGui::DragFloat((AV("jammer_radius") + "##jammer").c_str(),
                                     &ic.jammer_radius, 0.25f, 0.0f, 10000.0f, "%.1f m");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", AV("jammer_radius_hint").c_str());
                    }
                    ImGui::SliderFloat((AV("jammer_strength") + "##jammer").c_str(),
                                       &ic.jammer_strength, 0.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", AV("jammer_strength_hint").c_str());
                    }
                    if (ImGui::Button((AV("apply") + "##jammer").c_str())) {
                        runtime::JammerComponent edited;
                        edited.enabled = ic.jammer_enabled;
                        edited.radius = ic.jammer_radius;
                        edited.strength = ic.jammer_strength;
                        std::string err;
                        if (!app.set_jammer(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Jammer edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##jammer").c_str())) {
                        std::string err;
                        if (!app.detach_jammer(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach jammer failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Decision layer (Phase 61) ---
            // The considerations an NPC weighs. Always visible like Destruction,
            // Feedback and Perception: Attach authors a default brain, Apply
            // forwards the list through the validated setter, Detach removes it.
            if (ImGui::CollapsingHeader((AV("brain") + "###Brain").c_str())) {
                if (!ic.brain_has) {
                    ImGui::TextDisabled("%s", AV("no_brain").c_str());
                    if (ImGui::Button((AV("add_brain") + "##brain").c_str())) {
                        runtime::BrainComponent fresh;
                        // One consideration to start from, because a brain with an
                        // empty list decides nothing and the author would be
                        // editing a panel with no rows in it.
                        runtime::BrainAction first;
                        first.name = "advance";
                        first.input = runtime::BrainInput::Distance;
                        first.slope = -1.0f;
                        first.output_offset = 1.0f;
                        fresh.actions.push_back(std::move(first));
                        std::string err;
                        if (!app.set_brain(sel, fresh, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach brain failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##brain_enabled").c_str(),
                                    &ic.brain_enabled);
                    ImGui::SameLine();
                    ImGui::DragFloat((AV("brain_rate") + "##brain").c_str(), &ic.brain_rate,
                                     0.01f, 0.0f, 60.0f, "%.2f s");

                    // Combo items are widget labels, and the tokens ARE the scene
                    // format — so they are translated like every other label and
                    // the Arabic UI still shows no Latin.
                    const std::string input_labels[runtime::kBrainInputCount] = {
                        AV("brain_in_aware"), AV("brain_in_sensing"), AV("brain_in_distance"),
                        AV("brain_in_health"), AV("brain_in_ammo"), AV("brain_in_threat"),
                        AV("brain_in_enemies")};
                    const char* input_items[runtime::kBrainInputCount] = {
                        input_labels[0].c_str(), input_labels[1].c_str(), input_labels[2].c_str(),
                        input_labels[3].c_str(), input_labels[4].c_str(), input_labels[5].c_str(),
                        input_labels[6].c_str()};

                    for (usize i = 0; i < ic.brain_actions.size(); ++i) {
                        runtime::BrainAction& action = ic.brain_actions[i];
                        ImGui::PushID(static_cast<int>(i));
                        char name_buffer[64];
                        std::snprintf(name_buffer, sizeof(name_buffer), "%s", action.name.c_str());
                        ImGui::SetNextItemWidth(160.0f);
                        if (ImGui::InputText((AV("brain_action") + "##name").c_str(), name_buffer,
                                             sizeof(name_buffer))) {
                            action.name = name_buffer;
                        }
                        ImGui::SameLine();
                        int input_index = static_cast<int>(action.input);
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::Combo((AV("brain_input") + "##input").c_str(), &input_index,
                                         input_items, static_cast<int>(runtime::kBrainInputCount))) {
                            action.input = static_cast<runtime::BrainInput>(input_index);
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox((AV("hold_fire") + "##hold").c_str(), &action.hold_fire);

                        ImGui::SetNextItemWidth(110.0f);
                        ImGui::DragFloat((AV("brain_weight") + "##w").c_str(), &action.weight,
                                         0.05f, 0.0f, 1000.0f, "%.2f");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(110.0f);
                        ImGui::DragFloat((AV("brain_slope") + "##s").c_str(), &action.slope, 0.05f,
                                         -1000.0f, 1000.0f, "%.2f");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(110.0f);
                        ImGui::DragFloat((AV("brain_exponent") + "##e").c_str(), &action.exponent,
                                         0.05f, 0.0f, 16.0f, "%.2f");
                        ImGui::SetNextItemWidth(110.0f);
                        ImGui::DragFloat((AV("brain_input_offset") + "##io").c_str(),
                                         &action.input_offset, 0.05f, -1.0e6f, 1.0e6f, "%.2f");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(110.0f);
                        ImGui::DragFloat((AV("brain_output_offset") + "##oo").c_str(),
                                         &action.output_offset, 0.05f, -1000.0f, 1000.0f, "%.2f");
                        ImGui::PopID();
                    }

                    if (ImGui::Button((AV("add_brain_action") + "##brain").c_str())) {
                        runtime::BrainAction extra;
                        extra.name = "action" + std::to_string(ic.brain_actions.size() + 1u);
                        extra.input = runtime::BrainInput::Aware;
                        ic.brain_actions.push_back(std::move(extra));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("remove_brain_action") + "##brain").c_str()) &&
                        !ic.brain_actions.empty()) {
                        ic.brain_actions.pop_back();
                    }

                    if (ImGui::Button((AV("apply") + "##brain").c_str())) {
                        runtime::BrainComponent edited;
                        edited.enabled = ic.brain_enabled;
                        edited.rate = ic.brain_rate;
                        edited.actions = ic.brain_actions;
                        std::string err;
                        if (!app.set_brain(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Brain edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##brain").c_str())) {
                        std::string err;
                        if (!app.detach_brain(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach brain failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- State machine (Phase 62) ---
            // States and guarded edges: the machine's shape is the list of edges,
            // so they are edited in order and the order is what breaks a tie.
            if (ImGui::CollapsingHeader((AV("states") + "###States").c_str())) {
                if (!ic.sm_has) {
                    ImGui::TextDisabled("%s", AV("no_states").c_str());
                    if (ImGui::Button((AV("add_states") + "##sm").c_str())) {
                        runtime::StateMachineComponent fresh;
                        // A machine with one state and no edges ticks and does
                        // nothing, which is a visible starting point; an empty
                        // list would be a panel with no rows.
                        runtime::StateSpec idle;
                        idle.name = "idle";
                        fresh.states.push_back(std::move(idle));
                        fresh.initial = "idle";
                        std::string err;
                        if (!app.set_state_machine(sel, fresh, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach state machine failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##sm_enabled").c_str(), &ic.sm_enabled);
                    ImGui::SameLine();
                    ImGui::DragFloat((AV("states_rate") + "##sm").c_str(), &ic.sm_rate, 0.01f,
                                     0.0f, 60.0f, "%.2f s");

                    // The same input vocabulary the brain uses, translated on the
                    // same terms so the Arabic UI shows no Latin.
                    const std::string sm_input_labels[runtime::kBrainInputCount] = {
                        AV("brain_in_aware"), AV("brain_in_sensing"), AV("brain_in_distance"),
                        AV("brain_in_health"), AV("brain_in_ammo"), AV("brain_in_threat"),
                        AV("brain_in_enemies")};
                    const char* input_items[runtime::kBrainInputCount] = {
                        sm_input_labels[0].c_str(), sm_input_labels[1].c_str(),
                        sm_input_labels[2].c_str(), sm_input_labels[3].c_str(),
                        sm_input_labels[4].c_str(), sm_input_labels[5].c_str(),
                        sm_input_labels[6].c_str()};

                    // The state names as combo items: built once per frame, and
                    // owned for the whole of it so the pointers live.
                    std::vector<std::string> state_labels;
                    state_labels.reserve(ic.sm_states.size());
                    for (const runtime::StateSpec& state : ic.sm_states) {
                        state_labels.push_back(state.name);
                    }
                    std::vector<const char*> state_items;
                    state_items.reserve(state_labels.size());
                    for (const std::string& label : state_labels) {
                        state_items.push_back(label.c_str());
                    }

                    for (usize i = 0; i < ic.sm_states.size(); ++i) {
                        runtime::StateSpec& state = ic.sm_states[i];
                        ImGui::PushID(static_cast<int>(1000 + i));
                        char name_buffer[64];
                        std::snprintf(name_buffer, sizeof(name_buffer), "%s", state.name.c_str());
                        ImGui::SetNextItemWidth(160.0f);
                        if (ImGui::InputText((AV("state_name") + "##name").c_str(), name_buffer,
                                             sizeof(name_buffer))) {
                            state.name = name_buffer;
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox((AV("hold_fire") + "##state").c_str(), &state.hold_fire);
                        ImGui::PopID();
                    }
                    if (ImGui::Button((AV("add_state") + "##sm").c_str())) {
                        runtime::StateSpec extra;
                        extra.name = "state" + std::to_string(ic.sm_states.size() + 1u);
                        ic.sm_states.push_back(std::move(extra));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("remove_last") + "##state").c_str()) &&
                        !ic.sm_states.empty()) {
                        ic.sm_states.pop_back();
                    }

                    ImGui::Separator();
                    if (state_items.size() >= 2) {
                        for (usize i = 0; i < ic.sm_transitions.size(); ++i) {
                            runtime::TransitionSpec& transition = ic.sm_transitions[i];
                            ImGui::PushID(static_cast<int>(2000 + i));
                            int from = 0;
                            int to = 0;
                            for (usize s = 0; s < ic.sm_states.size(); ++s) {
                                if (ic.sm_states[s].name == transition.from) {
                                    from = static_cast<int>(s);
                                }
                                if (ic.sm_states[s].name == transition.to) {
                                    to = static_cast<int>(s);
                                }
                            }
                            ImGui::SetNextItemWidth(140.0f);
                            if (ImGui::Combo((AV("from_state") + "##t").c_str(), &from,
                                             state_items.data(),
                                             static_cast<int>(state_items.size()))) {
                                transition.from = ic.sm_states[static_cast<usize>(from)].name;
                            }
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(140.0f);
                            if (ImGui::Combo((AV("to_state") + "##t").c_str(), &to,
                                             state_items.data(),
                                             static_cast<int>(state_items.size()))) {
                                transition.to = ic.sm_states[static_cast<usize>(to)].name;
                            }
                            ImGui::SameLine();
                            int when_index = static_cast<int>(transition.when);
                            ImGui::SetNextItemWidth(120.0f);
                            if (ImGui::Combo((AV("guard_when") + "##t").c_str(), &when_index,
                                             input_items,
                                             static_cast<int>(runtime::kBrainInputCount))) {
                                transition.when = static_cast<runtime::BrainInput>(when_index);
                            }
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(90.0f);
                            ImGui::DragFloat((AV("guard_at") + "##t").c_str(), &transition.at, 0.1f,
                                             -1.0e6f, 1.0e6f, "%.2f");
                            ImGui::SameLine();
                            ImGui::Checkbox((AV("guard_below") + "##t").c_str(),
                                            &transition.below);
                            ImGui::PopID();
                        }
                        if (ImGui::Button((AV("transition") + "##add").c_str())) {
                            runtime::TransitionSpec extra;
                            extra.from = ic.sm_states.front().name;
                            extra.to = ic.sm_states.back().name;
                            extra.when = runtime::BrainInput::Aware;
                            extra.at = 0.5f;
                            ic.sm_transitions.push_back(std::move(extra));
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((AV("remove_last") + "##transition").c_str()) &&
                            !ic.sm_transitions.empty()) {
                            ic.sm_transitions.pop_back();
                        }
                    } else {
                        ImGui::TextDisabled("%s", AV("add_state").c_str());
                    }

                    if (ImGui::Button((AV("apply") + "##sm").c_str())) {
                        runtime::StateMachineComponent edited;
                        edited.enabled = ic.sm_enabled;
                        edited.rate = ic.sm_rate;
                        edited.initial = ic.sm_states.empty() ? std::string{}
                                                              : ic.sm_states.front().name;
                        edited.states = ic.sm_states;
                        edited.transitions = ic.sm_transitions;
                        std::string err;
                        if (!app.set_state_machine(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "State machine edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##sm").c_str())) {
                        std::string err;
                        if (!app.detach_state_machine(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach state machine failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Animation ---
            if (w->has<animation::AnimationComponent>(sel) &&
                ImGui::CollapsingHeader((AV("animation") + "###Animation").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (const auto* anim = w->get<animation::AnimationComponent>(sel)) {
                    // Readouts, not controls: single-line shaped strings that
                    // right-align in RTL (rt_*) and keep Latin/digit runs
                    // (clip names, numbers) in order via AVF().
                    rt_disabled_str(AVF("anim_bones", static_cast<int>(anim->skeleton.bones.size())));
                    if (anim->has_procedural) {
                        rt_disabled_str(AVF("anim_procedural", anim->procedural_clip_name.c_str(),
                                            ui::tr(anim->procedural.kind ==
                                                           animation::ProceduralClipSpec::Kind::Spin
                                                       ? "spin"
                                                       : "bob")
                                                .c_str(),
                                            static_cast<double>(anim->procedural.duration)));
                    }
                    rt_disabled_str(AVF("anim_time_state", static_cast<double>(anim->player.time()),
                                        anim->use_state_machine
                                            ? anim->state_machine.current_state().c_str()
                                            : "-"));
                }
                if (!ic.anim_clip_names.empty()) {
                    std::vector<const char*> names;
                    names.reserve(ic.anim_clip_names.size());
                    for (const auto& n : ic.anim_clip_names) {
                        names.push_back(n.c_str());
                    }
                    ImGui::Combo(AV("clip").c_str(), &ic.anim_clip, names.data(),
                                 static_cast<int>(names.size()));
                } else {
                    ImGui::TextDisabled(AV("no_clips").c_str());
                }
                ImGui::DragFloat(AV("speed").c_str(), &ic.anim_speed, 0.05f, -10.0f, 10.0f);
                // Combo items are widget labels: owned AV strings (not
                // temporaries) so the pointers outlive the Combo call.
                const std::string loop_items[3] = {AV("none"), AV("loop"), AV("ping_pong")};
                const char* loops[] = {loop_items[0].c_str(), loop_items[1].c_str(),
                                       loop_items[2].c_str()};
                ImGui::Combo(AV("loop").c_str(), &ic.anim_loop, loops, 3);
                ImGui::Checkbox(AV("paused").c_str(), &ic.anim_paused);
                ImGui::Checkbox(AV("state_machine").c_str(), &ic.anim_state_machine);
                if (ImGui::Button((AV("apply") + "##animation").c_str())) {
                    if (auto* src = w->get<animation::AnimationComponent>(sel)) {
                        animation::AnimationComponent edited = *src;
                        edited.speed = ic.anim_speed;
                        edited.paused = ic.anim_paused;
                        edited.use_state_machine = ic.anim_state_machine;
                        edited.player.set_loop_mode(static_cast<animation::LoopMode>(ic.anim_loop));
                        if (ic.anim_clip >= 0 &&
                            ic.anim_clip < static_cast<int>(ic.anim_clip_names.size())) {
                            edited.player.set_clip(
                                ic.anim_clip_names[static_cast<size_t>(ic.anim_clip)]);
                        }
                        std::string err;
                        if (!app.set_animation(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Animation edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // --- Audio ---
            // Always visible: the buffer row below is add-or-replace, so an
            // entity with no audio yet authors one by naming a file.
            if (ImGui::CollapsingHeader((AV("audio") + "###Audio").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (const auto* aud = w->get<audio::AudioComponent>(sel)) {
                    rt_disabled_str(AVF("audio_buffer",
                                        aud->buffer_name.empty()
                                            ? ui::tr("audio_generated").c_str()
                                            : aud->buffer_name.c_str(),
                                        aud->resolved_buffer() == nullptr
                                            ? ui::tr("audio_no_data").c_str()
                                            : ""));
                    rt_disabled_str(AVF("audio_cursor", static_cast<int>(aud->sample_cursor),
                                        ui::tr(aud->playing ? "yes" : "no").c_str()));
                    // Waveform preview: one min/max bar per pixel column, drawn
                    // from the loudest channel's envelope. Bucket count tracks
                    // the drawn width, so a wide panel costs no more decode
                    // work than a narrow one costs pixels.
                    if (const audio::AudioBuffer* buf = aud->resolved_buffer()) {
                        const ImVec2 wsize(ImGui::GetContentRegionAvail().x, 48.0f);
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const ImU32 bar_col = ImGui::GetColorU32(ImGuiCol_PlotLines);
                        const ImU32 mid_col = ImGui::GetColorU32(ImGuiCol_Border);
                        ImGui::Dummy(wsize);
                        const usize buckets = static_cast<usize>(std::max(1.0f, wsize.x));
                        const WaveformEnvelope env = compute_waveform(
                            buf->samples.data(), buf->samples.size(), buf->channels, buckets);
                        if (env.valid() && env.peak > 0.0f) {
                            const float mid = p0.y + wsize.y * 0.5f;
                            const float half = wsize.y * 0.5f - 1.0f;
                            const float inv_peak = 1.0f / env.peak;
                            const float bw = wsize.x / static_cast<float>(env.buckets());
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            dl->AddLine(ImVec2(p0.x, mid), ImVec2(p0.x + wsize.x, mid), mid_col);
                            for (usize b = 0; b < env.buckets(); ++b) {
                                const float x = p0.x + static_cast<float>(b) * bw;
                                // Normalise by peak so every clip uses the full
                                // height; an unnormalised -60 dB file would draw
                                // a flat line indistinguishable from silence.
                                const float y0 = mid - (env.max[b] * inv_peak) * half;
                                const float y1 = mid - (env.min[b] * inv_peak) * half;
                                dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + bw, y1), bar_col);
                            }
                        }
                        rt_disabled_str(AVF("audio_duration",
                                            static_cast<double>(buf->duration_seconds()),
                                            buf->sample_rate, buf->channels));
                    }
                }
                ImGui::SliderFloat(AV("volume").c_str(), &ic.aud_volume, 0.0f, 2.0f);
                ImGui::DragFloat(AV("pitch").c_str(), &ic.aud_pitch, 0.01f, 0.01f, 4.0f);
                ImGui::Checkbox(AV("looping").c_str(), &ic.aud_looping);
                ImGui::Checkbox(AV("autoplay").c_str(), &ic.aud_autoplay);
                ImGui::Checkbox(AV("spatial_3d").c_str(), &ic.aud_spatial);
                if (ic.aud_spatial) {
                    ImGui::DragFloat(AV("min_distance").c_str(), &ic.aud_min_dist, 0.1f, 0.01f, 1000.0f);
                    ImGui::DragFloat(AV("max_distance").c_str(), &ic.aud_max_dist, 0.5f, 0.01f, 10000.0f);
                }
                if (ImGui::Button((AV("apply") + "##audio").c_str())) {
                    if (auto* src = w->get<audio::AudioComponent>(sel)) {
                        audio::AudioComponent edited = *src;
                        edited.volume = ic.aud_volume;
                        edited.pitch = ic.aud_pitch;
                        edited.looping = ic.aud_looping;
                        edited.spatial = ic.aud_spatial;
                        edited.autoplay = ic.aud_autoplay;
                        edited.spatial_settings.min_distance = ic.aud_min_dist;
                        edited.spatial_settings.max_distance = ic.aud_max_dist;
                        std::string err;
                        if (!app.set_audio(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Audio edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
                // File-backed buffer: decodes now (add-or-replace), so the
                // waveform above and Play hear it without a scene reload.
                ImGui::InputText(AV("buffer").c_str(), ic.aud_buffer, sizeof(ic.aud_buffer));
                if (ImGui::Button((AV("attach_buffer") + "##audio").c_str())) {
                    std::string err;
                    if (!app.set_audio_buffer(sel, ic.aud_buffer, err)) {
                        ic.error = err;
                        push_error(app.console(), "Attach audio buffer failed", err);
                    } else {
                        ic.error.clear();
                        ic.valid = false;
                    }
                }
            }
            // --- Particles (Phase 25) ---
            // Always visible: Attach authors defaults, Apply forwards the
            // cached config through the validated setter.
            if (ImGui::CollapsingHeader((AV("particles") + "###Particles").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.part_has) {
                    ImGui::TextDisabled("%s", AV("no_particles").c_str());
                    if (ImGui::Button((AV("attach") + "##particles").c_str())) {
                        std::string err;
                        if (!app.attach_particles(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach particles failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##part_enabled").c_str(), &ic.part_enabled);
                    ImGui::DragFloat(AV("rate").c_str(), &ic.part_rate, 1.0f, 0.0f, 100000.0f);
                    ImGui::DragFloat(AV("lifetime").c_str(), &ic.part_lifetime, 0.05f, 0.01f,
                                     60.0f);
                    ImGui::SliderFloat(AV("lifetime_spread").c_str(), &ic.part_lifetime_spread,
                                       0.0f, 1.0f);
                    ImGui::DragFloat3(AV("velocity").c_str(), ic.part_velocity, 0.1f);
                    ImGui::DragFloat3(AV("spread").c_str(), ic.part_vel_spread, 0.1f, 0.0f,
                                      100.0f);
                    ImGui::DragFloat3(AV("gravity").c_str(), ic.part_gravity, 0.1f);
                    ImGui::DragFloat(AV("drag").c_str(), &ic.part_drag, 0.05f, 0.0f, 10.0f);
                    ImGui::DragFloat(AV("start_size").c_str(), &ic.part_start_size, 0.01f, 0.0f,
                                     10.0f);
                    ImGui::DragFloat(AV("end_size").c_str(), &ic.part_end_size, 0.01f, 0.0f,
                                     10.0f);
                    ImGui::ColorEdit3(AV("start_color").c_str(), ic.part_start_color);
                    ImGui::ColorEdit3(AV("end_color").c_str(), ic.part_end_color);
                    ImGui::DragInt(AV("max_particles").c_str(), &ic.part_max, 8.0f, 1, 1048576);
                    if (ImGui::Button((AV("apply") + "##particles").c_str())) {
                        vfx::ParticleComponent edited;
                        edited.enabled = ic.part_enabled;
                        edited.config.rate = ic.part_rate;
                        edited.config.lifetime = ic.part_lifetime;
                        edited.config.lifetime_spread = ic.part_lifetime_spread;
                        edited.config.velocity =
                            Vec3(ic.part_velocity[0], ic.part_velocity[1], ic.part_velocity[2]);
                        edited.config.velocity_spread = Vec3(
                            ic.part_vel_spread[0], ic.part_vel_spread[1], ic.part_vel_spread[2]);
                        edited.config.gravity =
                            Vec3(ic.part_gravity[0], ic.part_gravity[1], ic.part_gravity[2]);
                        edited.config.drag = ic.part_drag;
                        edited.config.start_size = ic.part_start_size;
                        edited.config.end_size = ic.part_end_size;
                        edited.config.start_color = Vec3(ic.part_start_color[0],
                                                         ic.part_start_color[1],
                                                         ic.part_start_color[2]);
                        edited.config.end_color = Vec3(ic.part_end_color[0], ic.part_end_color[1],
                                                       ic.part_end_color[2]);
                        edited.config.max_particles = static_cast<u32>(ic.part_max);
                        std::string err;
                        if (!app.set_particles(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Particles edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##particles").c_str())) {
                        std::string err;
                        if (!app.detach_particles(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach particles failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Cloth (Phase 25) ---
            if (ImGui::CollapsingHeader((AV("cloth") + "###Cloth").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.cloth_has) {
                    ImGui::TextDisabled("%s", AV("no_cloth").c_str());
                    if (ImGui::Button((AV("attach") + "##cloth").c_str())) {
                        std::string err;
                        if (!app.attach_cloth(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach cloth failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##cloth_enabled").c_str(), &ic.cloth_enabled);
                    ImGui::DragInt(AV("res_x").c_str(), &ic.cloth_res_x, 1.0f, 2, 128);
                    ImGui::DragInt(AV("res_z").c_str(), &ic.cloth_res_z, 1.0f, 2, 128);
                    ImGui::DragFloat(AV("spacing").c_str(), &ic.cloth_spacing, 0.01f, 0.001f,
                                     10.0f);
                    ImGui::DragFloat(AV("mass").c_str(), &ic.cloth_mass, 0.01f, 0.001f,
                                     1000.0f);
                    ImGui::SliderFloat(AV("damping").c_str(), &ic.cloth_damping, 0.0f, 0.999f);
                    ImGui::SliderFloat(AV("stiffness").c_str(), &ic.cloth_stiffness, 0.0f, 1.0f);
                    ImGui::DragInt(AV("iterations").c_str(), &ic.cloth_iterations, 1.0f, 1, 32);
                    ImGui::DragInt(AV("substeps").c_str(), &ic.cloth_substeps, 1.0f, 1, 8);
                    ImGui::DragFloat3(AV("gravity").c_str(), ic.cloth_gravity, 0.1f);
                    if (ImGui::Button((AV("apply") + "##cloth").c_str())) {
                        physics::ClothComponent edited;
                        edited.enabled = ic.cloth_enabled;
                        edited.config.res_x = ic.cloth_res_x;
                        edited.config.res_z = ic.cloth_res_z;
                        edited.config.spacing = ic.cloth_spacing;
                        edited.config.mass = ic.cloth_mass;
                        edited.config.damping = ic.cloth_damping;
                        edited.config.stiffness = ic.cloth_stiffness;
                        edited.config.iterations = ic.cloth_iterations;
                        edited.config.substeps = ic.cloth_substeps;
                        edited.config.gravity = Vec3(ic.cloth_gravity[0], ic.cloth_gravity[1],
                                                     ic.cloth_gravity[2]);
                        std::string err;
                        if (!app.set_cloth(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Cloth edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##cloth").c_str())) {
                        std::string err;
                        if (!app.detach_cloth(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach cloth failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Character (Phase 25) ---
            if (ImGui::CollapsingHeader((AV("character") + "###Character").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.char_has) {
                    ImGui::TextDisabled("%s", AV("no_character").c_str());
                    if (ImGui::Button((AV("attach") + "##character").c_str())) {
                        std::string err;
                        if (!app.attach_character(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach character failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##char_enabled").c_str(), &ic.char_enabled);
                    ImGui::DragFloat(AV("radius").c_str(), &ic.char_radius, 0.02f, 0.01f, 5.0f);
                    ImGui::DragFloat(AV("max_speed").c_str(), &ic.char_max_speed, 0.1f, 0.0f,
                                     100.0f);
                    ImGui::DragFloat(AV("acceleration").c_str(), &ic.char_acceleration, 0.5f,
                                     0.0f, 1000.0f);
                    ImGui::SliderFloat(AV("air_control").c_str(), &ic.char_air_control, 0.0f,
                                       1.0f);
                    ImGui::DragFloat(AV("jump_speed").c_str(), &ic.char_jump_speed, 0.1f, 0.0f,
                                     50.0f);
                    ImGui::DragFloat(AV("slope_limit").c_str(), &ic.char_slope, 0.5f, 0.0f,
                                     90.0f);
                    ImGui::DragFloat(AV("mass").c_str(), &ic.char_mass, 1.0f, 0.01f, 10000.0f);
                    ImGui::DragFloat(AV("friction").c_str(), &ic.char_friction, 0.05f, 0.0f,
                                     10.0f);
                    if (const auto* chc = w->get<physics::CharacterComponent>(sel)) {
                        rt_disabled_key(chc->grounded ? "grounded_yes" : "grounded_no");
                    }
                    if (ImGui::Button((AV("apply") + "##character").c_str())) {
                        physics::CharacterComponent edited;
                        edited.enabled = ic.char_enabled;
                        edited.config.radius = ic.char_radius;
                        edited.config.max_speed = ic.char_max_speed;
                        edited.config.acceleration = ic.char_acceleration;
                        edited.config.air_control = ic.char_air_control;
                        edited.config.jump_speed = ic.char_jump_speed;
                        edited.config.slope_limit_deg = ic.char_slope;
                        edited.config.mass = ic.char_mass;
                        edited.config.friction = ic.char_friction;
                        std::string err;
                        if (!app.set_character(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Character edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##character").c_str())) {
                        std::string err;
                        if (!app.detach_character(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach character failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                    // Debug drive: writes live input only (never persisted),
                    // so a level can be walked without gameplay code.
                    ImGui::DragFloat3(AV("wish_dir").c_str(), ic.char_wish, 0.05f, -1.0f, 1.0f);
                    ImGui::Checkbox(AV("jump").c_str(), &ic.char_jump);
                    if (ImGui::Button((AV("drive") + "##character").c_str())) {
                        std::string err;
                        if (!app.set_character_input(
                                sel, Vec3(ic.char_wish[0], ic.char_wish[1], ic.char_wish[2]),
                                ic.char_jump, err)) {
                            ic.error = err;
                            push_error(app.console(), "Drive character failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // --- Navigation volume (Phase 28) ---
            //
            // Placement is the entity Transform, so the section edits the bake
            // only. The empty state offers Attach like every other section;
            // the mesh itself is baked by the Runtime when the scene is
            // adopted, which is why there is no "rebuild" button — saving and
            // re-opening (or pressing Play) is the rebuild.
            if (ImGui::CollapsingHeader((AV("navmesh") + "###NavMesh").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.navm_has) {
                    ImGui::TextDisabled("%s", AV("no_navmesh").c_str());
                    if (ImGui::Button((AV("attach") + "##navmesh").c_str())) {
                        std::string err;
                        if (!app.attach_navmesh(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach navmesh failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##navm_enabled").c_str(), &ic.navm_enabled);
                    ImGui::DragFloat3(AV("nav_area").c_str(), ic.navm_area, 0.1f, 0.01f, 100000.0f);
                    ImGui::DragFloat(AV("cell_size").c_str(), &ic.navm_cell, 0.01f, 0.01f, 100.0f);
                    ImGui::DragFloat(AV("cell_height").c_str(), &ic.navm_cell_height, 0.01f, 0.01f,
                                     100.0f);
                    ImGui::DragFloat(AV("slope").c_str(), &ic.navm_slope, 0.5f, 0.0f, 90.0f);
                    ImGui::DragFloat(AV("climb").c_str(), &ic.navm_climb, 0.05f, 0.0f, 100.0f);
                    ImGui::DragFloat(AV("headroom").c_str(), &ic.navm_headroom, 0.05f, 0.01f,
                                     1000.0f);
                    ImGui::DragFloat(AV("min_area").c_str(), &ic.navm_min_area, 0.1f, 0.0f,
                                     1000000.0f);
                    ImGui::DragFloat(AV("agent_radius").c_str(), &ic.navm_radius, 0.05f, 0.0f,
                                     100.0f);
                    ImGui::DragFloat(AV("jump_distance").c_str(), &ic.navm_jump_distance, 0.1f,
                                     0.0f, 1000.0f);
                    ImGui::DragFloat(AV("jump_height").c_str(), &ic.navm_jump_height, 0.1f, 0.0f,
                                     1000.0f);
                    ImGui::DragInt(AV("max_verts").c_str(), &ic.navm_max_verts, 1.0f, 3, 64);
                    if (ImGui::Button((AV("apply") + "##navmesh").c_str())) {
                        runtime::NavMeshComponent edited;
                        edited.area_x = ic.navm_area[0];
                        edited.area_y = ic.navm_area[1];
                        edited.area_z = ic.navm_area[2];
                        edited.cell_size = ic.navm_cell;
                        edited.cell_height = ic.navm_cell_height;
                        edited.walkable_slope_deg = ic.navm_slope;
                        edited.walkable_climb = ic.navm_climb;
                        edited.walkable_height = ic.navm_headroom;
                        edited.min_region_area = ic.navm_min_area;
                        edited.agent_radius = ic.navm_radius;
                        edited.jump_distance = ic.navm_jump_distance;
                        edited.jump_height = ic.navm_jump_height;
                        edited.max_verts_per_poly = static_cast<u32>(ic.navm_max_verts);
                        edited.enabled = ic.navm_enabled;
                        std::string err;
                        if (!app.set_navmesh(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Navmesh edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##navmesh").c_str())) {
                        std::string err;
                        if (!app.detach_navmesh(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach navmesh failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                    // Authored links (Phase 63) are read here, written in the
                    // scene file: the list is geometry (two points per row) and
                    // a widget that added one in place would be a gizmo task,
                    // not an inspector one. What the author needs from this
                    // panel is the count the bake will use.
                    ImGui::TextDisabled("%s: %d", AV("nav_links").c_str(), ic.navm_link_count);
                }
            }
            // --- Terrain (Phase 63) ---
            //
            // The ground the bake samples and the mesh the viewport draws, from
            // one field. Apply re-runs the build and the bake together, which
            // is why the button says "apply" and not "save": nothing is
            // persisted until the scene is saved, and what changes immediately
            // is what the viewport shows.
            if (ImGui::CollapsingHeader((AV("terrain") + "###Terrain").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.terr_has) {
                    ImGui::TextDisabled("%s", AV("no_terrain").c_str());
                    if (ImGui::Button((AV("attach") + "##terrain").c_str())) {
                        std::string err;
                        if (!app.attach_terrain(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach terrain failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##terr_enabled").c_str(), &ic.terr_enabled);
                    ImGui::DragInt(AV("terrain_res").c_str(), &ic.terr_res, 1.0f, 2, 1024);
                    ImGui::DragFloat(AV("terrain_area").c_str(), &ic.terr_area, 1.0f, 0.1f,
                                     100000.0f);
                    ImGui::DragFloat(AV("terrain_height").c_str(), &ic.terr_height, 0.1f, 0.0f,
                                     100000.0f);
                    ImGui::DragFloat(AV("terrain_freq").c_str(), &ic.terr_freq, 0.001f, 0.0001f,
                                     10.0f);
                    ImGui::DragInt(AV("terrain_octaves").c_str(), &ic.terr_octaves, 1.0f, 0, 12);
                    ImGui::DragInt(AV("terrain_seed").c_str(), &ic.terr_seed, 1.0f, 0,
                                   0x7FFFFFFF);
                    ImGui::InputText(AV("terrain_heightmap").c_str(), ic.terr_heightmap,
                                     sizeof(ic.terr_heightmap));
                    ImGui::InputText(AV("terrain_material").c_str(), ic.terr_material,
                                     sizeof(ic.terr_material));
                    if (ImGui::Button((AV("apply") + "##terrain").c_str())) {
                        runtime::TerrainComponent edited;
                        edited.enabled = ic.terr_enabled;
                        edited.resolution = static_cast<unsigned>(ic.terr_res);
                        edited.area = ic.terr_area;
                        edited.height_scale = ic.terr_height;
                        edited.noise_frequency = ic.terr_freq;
                        edited.octaves = static_cast<unsigned>(ic.terr_octaves);
                        edited.seed = static_cast<unsigned>(ic.terr_seed);
                        edited.heightmap = ic.terr_heightmap;
                        edited.material = ic.terr_material;
                        std::string err;
                        if (!app.set_terrain(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Terrain edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##terrain").c_str())) {
                        std::string err;
                        if (!app.detach_terrain(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach terrain failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Navigation agent (Phase 28) ---
            //
            // Config plus the authored goal only. The live path is runtime
            // state: no widget shows it because none should edit it, and the
            // goal is the one field an author needs to move the walker.
            if (ImGui::CollapsingHeader((AV("nav_agent") + "###NavAgent").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.nava_has) {
                    ImGui::TextDisabled("%s", AV("no_nav_agent").c_str());
                    if (ImGui::Button((AV("attach") + "##nav_agent").c_str())) {
                        std::string err;
                        if (!app.attach_nav_agent(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach nav agent failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##nava_enabled").c_str(), &ic.nava_enabled);
                    ImGui::DragFloat(AV("speed").c_str(), &ic.nava_speed, 0.1f, 0.0f, 1000.0f);
                    ImGui::DragFloat3(AV("goal").c_str(), ic.nava_goal, 0.1f);
                    ImGui::DragFloat(AV("arrive_radius").c_str(), &ic.nava_arrive, 0.01f, 0.001f,
                                     100.0f);
                    if (ImGui::Button((AV("apply") + "##nav_agent").c_str())) {
                        runtime::NavAgentComponent edited;
                        edited.speed = ic.nava_speed;
                        edited.goal_x = ic.nava_goal[0];
                        edited.goal_y = ic.nava_goal[1];
                        edited.goal_z = ic.nava_goal[2];
                        edited.arrive_radius = ic.nava_arrive;
                        edited.enabled = ic.nava_enabled;
                        std::string err;
                        if (!app.set_nav_agent(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Nav agent edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##nav_agent").c_str())) {
                        std::string err;
                        if (!app.detach_nav_agent(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach nav agent failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                }
            }
            // --- Patrol route (Phase 64) ---
            //
            // The points are authored destinations; where the walker is on them
            // is live state and only shown, never edited. Apply forwards the
            // list through the validated setter, so a point that is not a
            // finite triple never reaches the file.
            if (ImGui::CollapsingHeader((AV("nav_route") + "###NavRoute").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.navr_has) {
                    ImGui::TextDisabled("%s", AV("no_nav_route").c_str());
                    if (ImGui::Button((AV("attach") + "##nav_route").c_str())) {
                        std::string err;
                        if (!app.attach_nav_route(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach nav route failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##navr_enabled").c_str(),
                                    &ic.navr_enabled);
                    ImGui::SameLine();
                    ImGui::Checkbox((AV("route_loop") + "##navr").c_str(), &ic.navr_loop);
                    ImGui::SameLine();
                    ImGui::Checkbox((AV("route_ping_pong") + "##navr").c_str(),
                                    &ic.navr_ping_pong);
                    ImGui::DragFloat(AV("route_wait").c_str(), &ic.navr_wait, 0.05f, 0.0f,
                                     3600.0f, "%.2f s");
                    // Perception gate (Phase 65). The combo is index-based so
                    // the enum never appears in the scene text; Apply maps it
                    // back to the policy the loader parses.
                    const std::string hold_labels[] = {
                        AV("route_hold_off"), AV("route_hold_aware"),
                        AV("route_hold_sensing")};
                    const char* hold_items[] = {hold_labels[0].c_str(), hold_labels[1].c_str(),
                                                hold_labels[2].c_str()};
                    int hold = std::clamp(ic.navr_hold, 0, 2);
                    ImGui::SetNextItemWidth(170.0f);
                    if (ImGui::Combo(AV("route_hold").c_str(), &hold, hold_items, 3)) {
                        ic.navr_hold = hold;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", AV("route_hold_hint").c_str());
                    }
                    if (ic.navr_hold != 0) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(130.0f);
                        ImGui::DragFloat(AV("route_resume").c_str(), &ic.navr_resume, 0.05f,
                                         0.0f, 3600.0f, "%.2f s");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", AV("route_resume_hint").c_str());
                        }
                    }
                    // Last-known-position search (Phase 66): opt-in, and only
                    // meaningful when a gate exists to release.
                    if (ic.navr_hold != 0) {
                        ImGui::SetNextItemWidth(130.0f);
                        ImGui::DragFloat(AV("route_investigate").c_str(), &ic.navr_investigate,
                                         0.05f, 0.0f, 3600.0f, "%.2f s");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", AV("route_investigate_hint").c_str());
                        }
                        // Squad token (Phase 67): opt-in like the search itself.
                        // Only a route that investigates can share a call.
                        if (ic.navr_investigate > 0.0f) {
                            ImGui::SetNextItemWidth(170.0f);
                            ImGui::InputText(AV("route_squad").c_str(), ic.navr_squad,
                                             sizeof(ic.navr_squad));
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", AV("route_squad_hint").c_str());
                            }
                        }
                    }
                    ImGui::TextDisabled("%s: %d", AV("route_points").c_str(),
                                        static_cast<int>(ic.navr_points.size()));
                    for (int i = 0; i < static_cast<int>(ic.navr_points.size()); ++i) {
                        runtime::NavPointSpec& point = ic.navr_points[static_cast<usize>(i)];
                        ImGui::PushID(i);
                        ImGui::Text("%s %d", AV("route_point").c_str(), i + 1);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(170.0f);
                        float pos[3]{point.x, point.y, point.z};
                        if (ImGui::DragFloat3("##route_pos", pos, 0.1f)) {
                            point.x = pos[0];
                            point.y = pos[1];
                            point.z = pos[2];
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80.0f);
                        float wait = point.wait;
                        if (ImGui::DragFloat("##route_point_wait", &wait, 0.05f, -1.0f,
                                             3600.0f, "%.2f s")) {
                            point.wait = wait;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", AV("route_point_wait_hint").c_str());
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton((AV("delete") + "##route_point").c_str())) {
                            ic.navr_points.erase(ic.navr_points.begin() + i);
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    if (ImGui::Button((AV("route_add_point") + "##navr").c_str())) {
                        runtime::NavPointSpec point;
                        if (!ic.navr_points.empty()) {
                            point = ic.navr_points.back();
                            point.x += 2.0f;
                            point.wait = -1.0f;
                        } else if (const auto* t = w->get<scene::Transform>(sel);
                                   t != nullptr) {
                            point = runtime::NavPointSpec{t->local_x, t->local_y, t->local_z,
                                                          -1.0f};
                        }
                        ic.navr_points.push_back(point);
                    }
                    if (ImGui::Button((AV("apply") + "##nav_route").c_str())) {
                        runtime::NavRouteComponent edited;
                        edited.points = ic.navr_points;
                        edited.wait = ic.navr_wait;
                        edited.loop = ic.navr_loop;
                        edited.ping_pong = ic.navr_ping_pong;
                        edited.enabled = ic.navr_enabled;
                        edited.hold = static_cast<runtime::NavRouteHoldPolicy>(
                            std::clamp(ic.navr_hold, 0, 2));
                        // A resume delay with no gate is dead data; the
                        // validator warns about it, so the inspector clears it
                        // when the author switches the gate off.
                        edited.resume = edited.hold == runtime::NavRouteHoldPolicy::Off
                                            ? 0.0f
                                            : std::max(0.0f, ic.navr_resume);
                        edited.investigate = edited.hold == runtime::NavRouteHoldPolicy::Off
                                                 ? 0.0f
                                                 : std::max(0.0f, ic.navr_investigate);
                        // A squad token with no gate is dead data, so switching
                        // the gate off clears it just like resume/investigate.
                        edited.squad = edited.hold == runtime::NavRouteHoldPolicy::Off
                                           ? std::string{}
                                           : std::string{ic.navr_squad};
                        std::string err;
                        if (!app.set_nav_route(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Nav route edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##nav_route").c_str())) {
                        std::string err;
                        if (!app.detach_nav_route(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach nav route failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                    // Live cursor: which point is being walked to or waited at,
                    // and whether a one-way route has ended. Session state, so
                    // it is read-only here.
                    if (runtime::Runtime* rt = app.runtime(); rt != nullptr) {
                        u32 index = 0;
                        u32 count = 0;
                        f32 wait = 0.0f;
                        bool finished = false;
                        bool held = false;
                        bool investigating = false;
                        if (rt->nav_route_state(sel, index, count, wait, finished, &held,
                                                &investigating)) {
                            if (finished) {
                                ImGui::TextDisabled("%s", AV("route_finished").c_str());
                            } else if (investigating) {
                                ImGui::TextDisabled("%s", AV("route_investigating").c_str());
                            } else if (held) {
                                ImGui::TextDisabled("%s", AV("route_held").c_str());
                            } else {
                                ImGui::TextDisabled("%s: %u/%u  %s %.2f", AV("route_live").c_str(),
                                                    index + 1, count,
                                                    AV("route_wait").c_str(), wait);
                            }
                        }
                    }
                }
            }
            // --- Vehicle (Step 11) ---
            //
            // The tune (chassis, wheels, suspension, tires, torques) plus the
            // debug drive. Config applies through set_vehicle (which re-binds
            // the car); the four inputs apply live-only through
            // set_vehicle_input, like the character's wish_dir/jump — driving
            // from the inspector never touches the file.
            if (ImGui::CollapsingHeader((AV("vehicle") + "###Vehicle").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!ic.veh_has) {
                    ImGui::TextDisabled("%s", AV("no_vehicle").c_str());
                    if (ImGui::Button((AV("attach") + "##vehicle").c_str())) {
                        std::string err;
                        if (!app.attach_vehicle(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Attach vehicle failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                } else {
                    ImGui::Checkbox((AV("enabled") + "##veh_enabled").c_str(), &ic.veh_enabled);
                    ImGui::DragFloat3(AV("chassis").c_str(), ic.veh_chassis, 0.05f, 0.01f,
                                      10.0f);
                    ImGui::DragFloat(AV("mass").c_str(), &ic.veh_mass, 10.0f, 1.0f, 100000.0f);
                    ImGui::DragFloat(AV("wheel_radius").c_str(), &ic.veh_wheel_radius, 0.01f,
                                      0.05f, 2.0f);
                    ImGui::DragFloat(AV("wheel_width").c_str(), &ic.veh_wheel_width, 0.01f,
                                      0.05f, 2.0f);
                    ImGui::DragFloat(AV("track_width").c_str(), &ic.veh_track, 0.05f, 0.1f,
                                      10.0f);
                    ImGui::DragFloat(AV("wheelbase").c_str(), &ic.veh_wheelbase, 0.05f, 0.1f,
                                      20.0f);
                    ImGui::DragFloat(AV("wheel_height").c_str(), &ic.veh_wheel_y, 0.05f, -5.0f,
                                      5.0f);
                    ImGui::DragFloat(AV("steer_angle").c_str(), &ic.veh_steer, 0.5f, 0.0f,
                                      60.0f);
                    ImGui::DragFloat(AV("engine_torque").c_str(), &ic.veh_torque, 10.0f, 0.0f,
                                      100000.0f);
                    ImGui::DragFloat(AV("susp_min").c_str(), &ic.veh_susp_min, 0.01f, 0.01f,
                                      5.0f);
                    ImGui::DragFloat(AV("susp_max").c_str(), &ic.veh_susp_max, 0.01f, 0.01f,
                                      5.0f);
                    ImGui::DragFloat(AV("susp_freq").c_str(), &ic.veh_susp_freq, 0.05f, 0.1f,
                                      20.0f);
                    ImGui::DragFloat(AV("damping").c_str(), &ic.veh_susp_damping, 0.05f, 0.0f,
                                      5.0f);
                    ImGui::DragFloat(AV("tire_peak").c_str(), &ic.veh_tire_peak, 0.05f, 0.0f,
                                      5.0f);
                    ImGui::DragFloat(AV("tire_limit").c_str(), &ic.veh_tire_limit, 0.05f, 0.0f,
                                      5.0f);
                    ImGui::DragFloat(AV("wheel_inertia").c_str(), &ic.veh_inertia, 0.05f,
                                      0.01f, 100.0f);
                    ImGui::DragFloat(AV("angular_damping").c_str(), &ic.veh_angular_damping,
                                      0.05f, 0.0f, 5.0f);
                    ImGui::DragFloat(AV("brake_torque").c_str(), &ic.veh_brake, 10.0f, 0.0f,
                                      50000.0f);
                    ImGui::DragFloat(AV("handbrake_torque").c_str(), &ic.veh_handbrake, 10.0f,
                                      0.0f, 100000.0f);
                    if (ImGui::Button((AV("apply") + "##vehicle").c_str())) {
                        physics::VehicleComponent edited;
                        edited.enabled = ic.veh_enabled;
                        edited.config.chassis_half_extents =
                            Vec3(ic.veh_chassis[0], ic.veh_chassis[1], ic.veh_chassis[2]);
                        edited.config.chassis_mass = ic.veh_mass;
                        edited.config.wheel_radius = ic.veh_wheel_radius;
                        edited.config.wheel_width = ic.veh_wheel_width;
                        edited.config.track_half_width = ic.veh_track;
                        edited.config.wheelbase_half_length = ic.veh_wheelbase;
                        edited.config.wheel_y = ic.veh_wheel_y;
                        edited.config.max_steer_deg = ic.veh_steer;
                        edited.config.engine_max_torque = ic.veh_torque;
                        edited.config.suspension_min_length = ic.veh_susp_min;
                        edited.config.suspension_max_length = ic.veh_susp_max;
                        edited.config.suspension_frequency_hz = ic.veh_susp_freq;
                        edited.config.suspension_damping = ic.veh_susp_damping;
                        edited.config.tire_peak_friction = ic.veh_tire_peak;
                        edited.config.tire_limit_friction = ic.veh_tire_limit;
                        edited.config.wheel_inertia = ic.veh_inertia;
                        edited.config.angular_damping = ic.veh_angular_damping;
                        edited.config.max_brake_torque = ic.veh_brake;
                        edited.config.max_handbrake_torque = ic.veh_handbrake;
                        std::string err;
                        if (!app.set_vehicle(sel, edited, err)) {
                            ic.error = err;
                            push_error(app.console(), "Vehicle edit failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##vehicle").c_str())) {
                        std::string err;
                        if (!app.detach_vehicle(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Detach vehicle failed", err);
                        } else {
                            ic.error.clear();
                            ic.valid = false;
                        }
                    }
                    // Debug drive: live inputs only (never persisted), so a
                    // level's road can be test-driven without gameplay code.
                    ImGui::SliderFloat(AV("throttle").c_str(), &ic.veh_throttle_in, -1.0f, 1.0f);
                    ImGui::SliderFloat(AV("steer").c_str(), &ic.veh_steer_in, -1.0f, 1.0f);
                    ImGui::SliderFloat(AV("brake").c_str(), &ic.veh_brake_in, 0.0f, 1.0f);
                    ImGui::SliderFloat(AV("handbrake").c_str(), &ic.veh_handbrake_in, 0.0f,
                                        1.0f);
                    if (ImGui::Button((AV("drive") + "##vehicle").c_str())) {
                        std::string err;
                        if (!app.set_vehicle_input(sel, ic.veh_throttle_in, ic.veh_steer_in,
                                                   ic.veh_brake_in, ic.veh_handbrake_in, err)) {
                            ic.error = err;
                            push_error(app.console(), "Drive vehicle failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button((AV("reset_vehicle") + "##vehicle").c_str())) {
                        std::string err;
                        if (!app.reset_vehicle_pose(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Reset vehicle failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                }
            }
            // --- Gameplay modules (reflection-driven) ---
            //
            // The *live module's* reflected state is what gets edited, not the
            // component's map. During a session the module owns its state and the
            // component is the saved snapshot, so editing the map would show the
            // user a value the module would immediately overwrite.
            if (ImGui::CollapsingHeader((AV("gameplay") + "###Gameplay").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                const std::vector<std::string> candidates = gameplay_module_candidates();
                if (candidates.empty()) {
                    ImGui::TextDisabled("%s", AV("no_gameplay_modules").c_str());
                } else {
                    static int chosen = 0;
                    std::vector<const char*> names;
                    names.reserve(candidates.size());
                    for (const std::string& candidate : candidates) names.push_back(candidate.c_str());
                    if (chosen >= static_cast<int>(names.size())) chosen = 0;

                    ImGui::Combo(AV("module").c_str(), &chosen, names.data(), static_cast<int>(names.size()));
                    ImGui::SameLine();
                    if (ImGui::Button((AV("attach") + "##gameplay").c_str())) {
                        std::string err;
                        if (!app.attach_gameplay_module(
                                sel, candidates[static_cast<usize>(chosen)], err)) {
                            push_error(app.console(), "Attach gameplay module failed", err);
                        } else {
                            ic.valid = false;
                        }
                    }
                }

                if (const auto* gmc = w->get<gameplay::GameplayModuleComponent>(sel)) {
                    rt_disabled_str(AVF("gfx_attached", gmc->module_name.c_str(),
                                        ui::tr(gmc->enabled ? "enabled" : "disabled").c_str()));
                    ImGui::SameLine();
                    if (ImGui::Button((AV("detach") + "##gameplay").c_str())) {
                        std::string err;
                        if (!app.detach_gameplay_module(sel, err)) {
                            push_error(app.console(), "Detach gameplay module failed", err);
                        } else {
                            ic.valid = false;
                        }
                    }

                    runtime::Runtime* rt = app.runtime();
                    gameplay::GameplayModule* module =
                        rt != nullptr ? rt->find_gameplay_module(gmc->module_name) : nullptr;
                    if (module == nullptr) {
                        rt_disabled_str(
                            AVF("module_not_registered", gmc->module_name.c_str()));
                    } else {
                        const gameplay::GameplayStateBinding binding = module->state();
                        if (!binding.valid()) {
                            ImGui::TextDisabled("%s", AV("module_no_state").c_str());
                        } else {
                            ReflectedObjectView view =
                                ReflectedObjectView::build(binding.instance, binding.meta);
                            if (view.empty()) {
                                ImGui::TextDisabled(AV("module_no_props").c_str());
                            }
                            for (const ReflectedGroup& group : view.groups()) {
                                if (!group.category.empty()) {
                                    ImGui::SeparatorText(group.category.c_str());
                                }
                                for (usize index : group.field_indices) {
                                    // One ID scope per field: the reflected
                                    // widget labels come from the property
                                    // names, and two properties that happen to
                                    // share a label would otherwise submit two
                                    // items with one ID.
                                    ImGui::PushID(static_cast<int>(index));
                                    (void)draw_reflected_field(view, index);
                                    ImGui::PopID();
                                }
                            }
                        }
                    }
                }
            }
            // --- Lua script (file-backed ScriptComponent) ---
            //
            // The path is the persisted field; the file is the source of truth.
            // Attach reads the file now (so Play works without a reload) and
            // the Runtime re-resolves it on every scene adopt. New files are
            // created from a minimal update(dt) template — an empty path or a
            // missing file is rejected here rather than stored as a component
            // that looks scripted but never ticks.
            if (ImGui::CollapsingHeader((AV("script") + "###Script").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                const bool has_script = w->has<scripting::ScriptComponent>(sel);
                ImGui::InputText(AV("script_path").c_str(), ic.script_path, sizeof(ic.script_path));
                ImGui::Checkbox((AV("enabled") + "##script_enabled").c_str(), &ic.script_enabled);
                if (const auto* sc = w->get<scripting::ScriptComponent>(sel)) {
                    const size_t lines = std::count(sc->source.begin(), sc->source.end(), '\n') +
                                         (sc->source.empty() ? 0 : 1);
                    rt_disabled_str(AVF("script_loaded", static_cast<int>(sc->source.size()),
                                        static_cast<int>(lines)));
                } else {
                    ImGui::TextDisabled("%s", AV("script_no_data").c_str());
                }
                if (ImGui::Button((AV("new_script") + "##script").c_str())) {
                    std::string err, created;
                    bool ok = false;
                    if (std::string(ic.script_path).empty()) {
                        // Empty field: first free name, never a clobber error.
                        ok = app.create_unique_script_file("content://Scripts/", "script",
                                                           created, err);
                    } else {
                        created = ic.script_path;
                        ok = app.create_script_file(created, err);
                    }
                    if (ok) {
                        // A new file belongs to this entity: attach it at
                        // once, so the section leaves [no script] behind and
                        // the VS button below appears.
                        ok = app.attach_script(sel, created, err);
                    }
                    if (!ok) {
                        ic.error = err;
                        push_error(app.console(), "Create script failed", err);
                    } else {
                        std::strncpy(ic.script_path, created.c_str(),
                                     sizeof(ic.script_path) - 1);
                        ic.script_path[sizeof(ic.script_path) - 1] = '\0';
                        ic.error.clear();
                        ic.valid = false;
                    }
                }
                ImGui::SameLine();
                if (has_script ? ImGui::Button((AV("apply") + "##script").c_str())
                               : ImGui::Button((AV("attach") + "##script").c_str())) {
                    std::string err;
                    bool ok = false;
                    if (has_script) {
                        ok = app.set_script_path(sel, ic.script_path, err) &&
                             app.set_script_enabled(sel, ic.script_enabled, err);
                    } else {
                        ok = app.attach_script(sel, ic.script_path, err);
                        if (ok && !ic.script_enabled) {
                            ok = app.set_script_enabled(sel, false, err);
                        }
                    }
                    if (!ok) {
                        ic.error = err;
                        push_error(app.console(), "Script edit failed", err);
                    } else {
                        ic.error.clear();
                        ic.valid = false;
                    }
                }
                ImGui::SameLine();
                if (has_script && ImGui::Button((AV("detach") + "##script").c_str())) {
                    std::string err;
                    if (!app.detach_script(sel, err)) {
                        ic.error = err;
                        push_error(app.console(), "Detach script failed", err);
                    } else {
                        ic.error.clear();
                        ic.valid = false;
                    }
                }
                // Unity-style: write the object's code where code is written.
                // Visible whenever a path is typed, not only when attached:
                // opening a file to edit it must not require attaching first.
                // Opens detached in Visual Studio / VS Code / the shell
                // default and names the pick in the console.
                if (ic.script_path[0] != '\0' &&
                    ImGui::Button((AV("open_in_vs") + "##script").c_str())) {
                    std::string kind, err;
                    if (!app.open_asset_in_ide(ic.script_path, kind, err)) {
                        ic.error = err;
                        push_error(app.console(), "Open in IDE failed", err);
                    } else {
                        ic.error.clear();
                        push_info(app.console(),
                                  std::string("Opened in ") + kind + ": " + ic.script_path);
                    }
                }
            }
            ImGui::Spacing();
            // --- Phases 115-122 Engine Feature Inspectors ---
            if (ic.cable_has && ImGui::CollapsingHeader((AV("cable_physics") + "###CablePhysics").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##cable_en").c_str(), &ic.cable_enabled);
                ImGui::DragFloat((AV("total_length") + "##cable_len").c_str(), &ic.cable_length, 0.1f, 0.5f, 500.0f, "%.2f m");
                ImGui::SliderInt((AV("particle_count") + "##cable_pts").c_str(), &ic.cable_particles, 2, 128);
                ImGui::SliderFloat((AV("cable_stiffness") + "##cable_stiff").c_str(), &ic.cable_stiffness, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat((AV("damping") + "##cable_damp").c_str(), &ic.cable_damping, 0.0f, 0.5f, "%.3f");
                ImGui::DragFloat((AV("linear_density") + "##cable_dens").c_str(), &ic.cable_density, 0.05f, 0.01f, 50.0f, "%.2f kg/m");
                ImGui::Checkbox((AV("breakable") + "##cable_brk").c_str(), &ic.cable_breakable);
                if (ic.cable_breakable) {
                    ImGui::DragFloat((AV("max_tension") + "##cable_maxt").c_str(), &ic.cable_tension_max, 50.0f, 10.0f, 100000.0f, "%.0f N");
                }
                ImGui::Checkbox((AV("pin_start") + "##cable_pins").c_str(), &ic.cable_pin_start);
                ImGui::SameLine();
                ImGui::Checkbox((AV("pin_end") + "##cable_pine").c_str(), &ic.cable_pin_end);
                draw_vec3_control(AV("end_offset").c_str(), ic.cable_end_offset, 0.0f, 0.1f);

                const float tension_ratio = ic.cable_tension_max > 0.0f ? std::clamp(ic.cable_live_tension / ic.cable_tension_max, 0.0f, 1.0f) : 0.0f;
                ImVec4 tension_col = ImVec4(0.2f + 0.8f * tension_ratio, 0.8f * (1.0f - tension_ratio), 0.2f, 1.0f);
                ImGui::TextColored(tension_col, "%s: %.1f N", AV("live_tension").c_str(), ic.cable_live_tension);
                ImGui::ProgressBar(tension_ratio, ImVec2(-FLT_MIN, 0.0f));

                if (ImGui::Button((AV("apply") + "##cable").c_str())) {
                    runtime::CableComponent cb;
                    cb.total_length = ic.cable_length;
                    cb.particle_count = static_cast<u32>(ic.cable_particles);
                    cb.stiffness = ic.cable_stiffness;
                    cb.damping = ic.cable_damping;
                    cb.linear_density = ic.cable_density;
                    cb.max_tension = ic.cable_tension_max;
                    cb.breakable = ic.cable_breakable;
                    cb.pin_start = ic.cable_pin_start;
                    cb.pin_end = ic.cable_pin_end;
                    cb.end_offset = Vec3{ic.cable_end_offset[0], ic.cable_end_offset[1], ic.cable_end_offset[2]};
                    cb.enabled = ic.cable_enabled;
                    std::string err;
                    if (!app.set_cable(sel, cb, err)) {
                        ic.error = err;
                        push_error(app.console(), "Cable edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##cable").c_str())) {
                    std::string err;
                    if (!app.detach_cable(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.cable_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ic.weather_has && ImGui::CollapsingHeader((AV("weather_system") + "###WeatherSystem").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##w_en").c_str(), &ic.weather_enabled);
                const char* w_types[7] = {
                    "weather_clear", "weather_cloudy", "weather_overcast",
                    "weather_rain", "weather_thunderstorm", "weather_foggy", "weather_snow"
                };
                if (ImGui::BeginCombo(AV("weather_preset").c_str(), AV(w_types[std::clamp(ic.weather_type, 0, 6)]).c_str())) {
                    for (int i = 0; i < 7; ++i) {
                        const bool is_sel = (ic.weather_type == i);
                        if (ImGui::Selectable(AV(w_types[i]).c_str(), is_sel)) {
                            ic.weather_type = i;
                            if (i == 0) { ic.w_fog_density = 0.005f; ic.w_ground_wetness = 0.0f; }
                            else if (i == 1) { ic.w_fog_density = 0.012f; }
                            else if (i == 2) { ic.w_fog_density = 0.025f; }
                            else if (i == 3) { ic.w_fog_density = 0.035f; ic.w_ground_wetness = 0.7f; ic.w_wind_strength = 6.0f; }
                            else if (i == 4) { ic.w_fog_density = 0.055f; ic.w_ground_wetness = 1.0f; ic.w_wind_strength = 14.0f; }
                            else if (i == 5) { ic.w_fog_density = 0.095f; ic.w_fog_falloff = 0.04f; }
                            else if (i == 6) { ic.w_fog_density = 0.030f; ic.w_ground_wetness = 0.3f; }
                        }
                        if (is_sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat((AV("fog_density") + "##w_dens").c_str(), &ic.w_fog_density, 0.0f, 0.15f, "%.4f");
                ImGui::SliderFloat((AV("fog_falloff") + "##w_fall").c_str(), &ic.w_fog_falloff, 0.005f, 0.3f, "%.3f");
                ImGui::DragFloat((AV("fog_base_height") + "##w_base").c_str(), &ic.w_fog_base, 0.5f, -100.0f, 500.0f, "%.1f m");
                ImGui::SliderFloat((AV("fog_max_opacity") + "##w_opac").c_str(), &ic.w_fog_max_opacity, 0.0f, 1.0f, "%.2f");
                ImGui::ColorEdit3(AV("fog_color").c_str(), ic.w_fog_color);
                ImGui::SliderFloat((AV("sun_inscattering") + "##w_scat").c_str(), &ic.w_sun_inscattering, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat((AV("ground_wetness") + "##w_wet").c_str(), &ic.w_ground_wetness, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat((AV("wind_strength") + "##w_wspd").c_str(), &ic.w_wind_strength, 0.0f, 30.0f, "%.1f m/s");
                draw_vec3_control(AV("wind_direction").c_str(), ic.w_wind_dir, 0.0f, 0.05f);

                if (ImGui::Button((AV("apply") + "##weather").c_str())) {
                    runtime::WeatherComponent wc;
                    wc.weather_type = ic.weather_type;
                    wc.fog_density = ic.w_fog_density;
                    wc.fog_height_falloff = ic.w_fog_falloff;
                    wc.fog_base_height = ic.w_fog_base;
                    wc.fog_max_opacity = ic.w_fog_max_opacity;
                    wc.fog_color[0] = ic.w_fog_color[0];
                    wc.fog_color[1] = ic.w_fog_color[1];
                    wc.fog_color[2] = ic.w_fog_color[2];
                    wc.sun_inscattering = ic.w_sun_inscattering;
                    wc.ground_wetness = ic.w_ground_wetness;
                    wc.wind_strength = ic.w_wind_strength;
                    wc.wind_direction[0] = ic.w_wind_dir[0];
                    wc.wind_direction[1] = ic.w_wind_dir[1];
                    wc.wind_direction[2] = ic.w_wind_dir[2];
                    wc.enabled = ic.weather_enabled;
                    std::string err;
                    if (!app.set_weather(sel, wc, err)) {
                        ic.error = err;
                        push_error(app.console(), "Weather edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##weather").c_str())) {
                    std::string err;
                    if (!app.detach_weather(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.weather_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ic.rule_tile_has && ImGui::CollapsingHeader((AV("rule_tile") + "###RuleTile").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##rt_en").c_str(), &ic.rt_enabled);
                ImGui::DragInt((AV("tile_size") + "##rt_def").c_str(), &ic.rt_default_id, 1.0f, 0, 1024);
                const char* rt_modes[3] = {"rule_preset_16", "rule_preset_47", "rule_preset_custom"};
                if (ImGui::BeginCombo(AV("rule_preset").c_str(), AV(rt_modes[std::clamp(ic.rt_preset, 0, 2)]).c_str())) {
                    for (int i = 0; i < 3; ++i) {
                        const bool is_sel = (ic.rt_preset == i);
                        if (ImGui::Selectable(AV(rt_modes[i]).c_str(), is_sel)) {
                            ic.rt_preset = i;
                            ic.rt_count = (i == 0) ? 16 : (i == 1 ? 47 : 8);
                        }
                        if (is_sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::Text("%s: %d", AV("rule_count").c_str(), ic.rt_count);
                ImGui::Checkbox((AV("auto_update_map") + "##rt_auto").c_str(), &ic.rt_auto_update);

                ImGui::TextDisabled("%s", AV("neighbor_pattern").c_str());
                static int grid_sample[3][3] = {{1, 1, 1}, {1, 1, 1}, {0, 1, 0}};
                for (int gy = 0; gy < 3; ++gy) {
                    for (int gx = 0; gx < 3; ++gx) {
                        if (gx > 0) ImGui::SameLine();
                        ImGui::PushID(gy * 3 + gx);
                        const bool active = grid_sample[gy][gx] != 0;
                        if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(37, 99, 235, 220));
                        else ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 55, 65, 200));
                        if (ImGui::Button((gy == 1 && gx == 1) ? "[T]" : (active ? "[X]" : "[ ]"), ImVec2(28.0f, 24.0f))) {
                            if (gy != 1 || gx != 1) grid_sample[gy][gx] = active ? 0 : 1;
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                }

                if (ImGui::Button((AV("apply") + "##ruletile").c_str())) {
                    runtime::RuleTileComponent rtc;
                    rtc.default_tile_id = static_cast<u32>(ic.rt_default_id);
                    rtc.rule_preset = ic.rt_preset;
                    rtc.rule_count = static_cast<u32>(ic.rt_count);
                    rtc.auto_update = ic.rt_auto_update;
                    rtc.enabled = ic.rt_enabled;
                    std::string err;
                    if (!app.set_rule_tile(sel, rtc, err)) {
                        ic.error = err;
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##ruletile").c_str())) {
                    std::string err;
                    if (!app.detach_rule_tile(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.rule_tile_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ic.flow_has && ImGui::CollapsingHeader((AV("flow_field") + "###FlowField").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##ff_en").c_str(), &ic.ff_enabled);
                ImGui::SliderInt((AV("grid_width") + "##ff_w").c_str(), &ic.ff_width, 8, 256);
                ImGui::SliderInt((AV("grid_height") + "##ff_h").c_str(), &ic.ff_height, 8, 256);
                ImGui::DragFloat((AV("cell_size") + "##ff_cs").c_str(), &ic.ff_cell_size, 0.1f, 0.2f, 20.0f, "%.2f m");
                ImGui::DragFloat((AV("target_x") + "##ff_tx").c_str(), &ic.ff_target[0], 0.5f, 0.0f, static_cast<float>(ic.ff_width));
                ImGui::DragFloat((AV("target_y") + "##ff_ty").c_str(), &ic.ff_target[1], 0.5f, 0.0f, static_cast<float>(ic.ff_height));

                if (ImGui::Button((AV("recalculate_flow") + "##ff_calc").c_str())) {
                    push_info(app.console(), "FlowField recomputed: goals=(" + std::to_string(static_cast<int>(ic.ff_target[0])) + ", " + std::to_string(static_cast<int>(ic.ff_target[1])) + ")");
                }

                if (ImGui::Button((AV("apply") + "##flow").c_str())) {
                    runtime::FlowFieldComponent ffc;
                    ffc.grid_width = static_cast<u32>(ic.ff_width);
                    ffc.grid_height = static_cast<u32>(ic.ff_height);
                    ffc.cell_size = ic.ff_cell_size;
                    ffc.target_x = ic.ff_target[0];
                    ffc.target_y = ic.ff_target[1];
                    ffc.enabled = ic.ff_enabled;
                    std::string err;
                    if (!app.set_flow_field(sel, ffc, err)) {
                        ic.error = err;
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##flow").c_str())) {
                    std::string err;
                    if (!app.detach_flow_field(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.flow_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ic.talent_has && ImGui::CollapsingHeader((AV("talent_tree") + "###TalentTree").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##tt_en").c_str(), &ic.talent_enabled);
                ImGui::InputText(AV("tree_id").c_str(), ic.tt_id, sizeof(ic.tt_id));
                ImGui::InputText(AV("tree_name").c_str(), ic.tt_name, sizeof(ic.tt_name));
                ImGui::DragInt((AV("available_points") + "##tt_pts").c_str(), &ic.tt_available, 1.0f, 0, 999);
                ImGui::Text("%s: %d", AV("spent_points").c_str(), ic.tt_spent);

                ImGui::Separator();
                ImGui::TextDisabled("Nodes Preview (3 Tiers)");
                const char* sample_nodes[3] = {"Combat Mastery (Tier 0)", "Armor Piercing (Tier 1)", "Berserker Rage (Tier 2 - Keystone)"};
                for (int ni = 0; ni < 3; ++ni) {
                    ImGui::BulletText("%s", sample_nodes[ni]);
                    ImGui::SameLine();
                    ImGui::PushID(ni);
                    if (ImGui::SmallButton("+")) {
                        if (ic.tt_available > 0) {
                            ic.tt_available--;
                            ic.tt_spent++;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("-")) {
                        if (ic.tt_spent > 0) {
                            ic.tt_spent--;
                            ic.tt_available++;
                        }
                    }
                    ImGui::PopID();
                }

                if (ImGui::Button((AV("apply") + "##talent").c_str())) {
                    runtime::TalentComponent tt;
                    tt.tree_id = ic.tt_id;
                    tt.tree_name = ic.tt_name;
                    tt.available_points = static_cast<u32>(std::max(0, ic.tt_available));
                    tt.spent_points = static_cast<u32>(std::max(0, ic.tt_spent));
                    tt.enabled = ic.talent_enabled;
                    std::string err;
                    if (!app.set_talent_tree(sel, tt, err)) {
                        ic.error = err;
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##talent").c_str())) {
                    std::string err;
                    if (!app.detach_talent_tree(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.talent_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ic.ragdoll_has && ImGui::CollapsingHeader((AV("ragdoll_blend") + "###RagdollBlend").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##rd_en").c_str(), &ic.ragdoll_enabled);
                ImGui::SliderFloat((AV("blend_weight") + "##rd_wt").c_str(), &ic.rd_weight, 0.0f, 1.0f, "%.2f (0=Anim, 1=Ragdoll)");
                const char* rd_poses[4] = {"pose_inactive", "pose_ragdoll", "pose_getup_face_up", "pose_getup_face_down"};
                if (ImGui::BeginCombo(AV("pose_state").c_str(), AV(rd_poses[std::clamp(ic.rd_pose, 0, 3)]).c_str())) {
                    for (int i = 0; i < 4; ++i) {
                        const bool is_sel = (ic.rd_pose == i);
                        if (ImGui::Selectable(AV(rd_poses[i]).c_str(), is_sel)) {
                            ic.rd_pose = i;
                            ic.rd_weight = (i == 1 ? 1.0f : (i == 0 ? 0.0f : 0.5f));
                        }
                        if (is_sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("Blend Duration##rd_dur", &ic.rd_duration, 0.05f, 3.0f, "%.2f s");
                ImGui::Checkbox((AV("preserve_momentum") + "##rd_mom").c_str(), &ic.rd_momentum);

                if (ImGui::Button((AV("apply") + "##ragdoll").c_str())) {
                    runtime::RagdollComponent rd;
                    rd.blend_weight = ic.rd_weight;
                    rd.pose_state = ic.rd_pose;
                    rd.blend_duration = ic.rd_duration;
                    rd.preserve_momentum = ic.rd_momentum;
                    rd.enabled = ic.ragdoll_enabled;
                    std::string err;
                    if (!app.set_ragdoll(sel, rd, err)) {
                        ic.error = err;
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##ragdoll").c_str())) {
                    std::string err;
                    if (!app.detach_ragdoll(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.ragdoll_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ic.inv_has && ImGui::CollapsingHeader((AV("grid_inventory") + "###GridInventory").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##inv_en").c_str(), &ic.inv_enabled);
                ImGui::SliderInt((AV("inv_columns") + "##inv_c").c_str(), &ic.inv_cols, 1, 32);
                ImGui::SliderInt((AV("inv_rows") + "##inv_r").c_str(), &ic.inv_rows, 1, 32);
                ImGui::DragFloat((AV("max_weight") + "##inv_mw").c_str(), &ic.inv_max_weight, 1.0f, 1.0f, 1000.0f, "%.1f kg");
                ImGui::DragFloat((AV("current_weight") + "##inv_cw").c_str(), &ic.inv_cur_weight, 0.5f, 0.0f, ic.inv_max_weight, "%.1f kg");
                ImGui::DragInt("Item Count##inv_cnt", &ic.inv_item_count, 1.0f, 0, 100);

                const float weight_fraction = ic.inv_max_weight > 0.0f ? std::clamp(ic.inv_cur_weight / ic.inv_max_weight, 0.0f, 1.0f) : 0.0f;
                ImGui::ProgressBar(weight_fraction, ImVec2(-FLT_MIN, 0.0f));

                if (ImGui::Button((AV("auto_pack") + "##inv_pack").c_str())) {
                    push_info(app.console(), "Inventory auto-packed: " + std::to_string(ic.inv_item_count) + " items aligned into " + std::to_string(ic.inv_cols) + "x" + std::to_string(ic.inv_rows) + " grid.");
                }

                if (ImGui::Button((AV("apply") + "##inv").c_str())) {
                    runtime::InventoryComponent inv;
                    inv.columns = static_cast<u32>(ic.inv_cols);
                    inv.rows = static_cast<u32>(ic.inv_rows);
                    inv.max_weight = ic.inv_max_weight;
                    inv.current_weight = ic.inv_cur_weight;
                    inv.item_count = static_cast<u32>(ic.inv_item_count);
                    inv.auto_pack = ic.inv_auto_pack;
                    inv.enabled = ic.inv_enabled;
                    std::string err;
                    if (!app.set_inventory(sel, inv, err)) {
                        ic.error = err;
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##inv").c_str())) {
                    std::string err;
                    if (!app.detach_inventory(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.inv_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ic.sa_has && ImGui::CollapsingHeader((AV("surface_audio") + "###SurfaceAudio").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox((AV("enabled") + "##sa_en").c_str(), &ic.sa_enabled);
                const char* sa_mats[7] = {
                    "mat_concrete", "mat_dirt", "mat_metal", "mat_wood",
                    "mat_water", "mat_grass", "mat_snow"
                };
                if (ImGui::BeginCombo(AV("surface_material").c_str(), AV(sa_mats[std::clamp(ic.sa_mat, 0, 6)]).c_str())) {
                    for (int i = 0; i < 7; ++i) {
                        const bool is_sel = (ic.sa_mat == i);
                        if (ImGui::Selectable(AV(sa_mats[i]).c_str(), is_sel)) {
                            ic.sa_mat = i;
                        }
                        if (is_sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("Volume Scale##sa_v", &ic.sa_vol, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat((AV("pitch_variation") + "##sa_p").c_str(), &ic.sa_pitch, 0.0f, 0.5f, "%.2f");
                ImGui::Checkbox((AV("wetness_modulation") + "##sa_w").c_str(), &ic.sa_wetness);

                if (ImGui::Button((AV("apply") + "##sa").c_str())) {
                    runtime::SurfaceAudioComponent sa;
                    sa.surface_material = ic.sa_mat;
                    sa.volume_scale = ic.sa_vol;
                    sa.pitch_variation = ic.sa_pitch;
                    sa.wetness_modulation = ic.sa_wetness;
                    sa.enabled = ic.sa_enabled;
                    std::string err;
                    if (!app.set_surface_audio(sel, sa, err)) {
                        ic.error = err;
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("detach") + "##sa").c_str())) {
                    std::string err;
                    if (!app.detach_surface_audio(sel, err)) {
                        ic.error = err;
                    } else {
                        ic.sa_has = false;
                        ic.valid = false;
                    }
                }
            }

            if (ImGui::Button(AV("undo").c_str()) && app.stack().can_undo()) {
                std::string err;
                if (!app.undo(err)) {
                    push_error(app.console(), "Undo failed", err);
                } else {
                    ic.valid = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(AV("redo").c_str()) && app.stack().can_redo()) {
                std::string err;
                if (!app.redo(err)) {
                    push_error(app.console(), "Redo failed", err);
                } else {
                    ic.valid = false;
                }
            }
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();

    // --- Dedicated editors (ScenePanels.cpp) -----------------------------------
    // One window per engine function, each bound to the live scene object the
    // renderer actually consumes (the light it lights with, the sky it paints,
    // the camera it looks through) plus what the renderer is doing right now.
    // Recorded here so they take part in the same dockspace as the panels.
    scene_panels(app, stats);

    // --- Bottom: FileSystem + Console ------------------------------------------
    // FileSystem dock lives in its own TU (Editor/src/ui/FileSystemPanel.cpp):
    // Godot-style favorites / history / breadcrumb / icons / grid. The window
    // ID stays "###Assets" so existing dock layouts survive the upgrade.
    if (ImGui::Begin((AV("filesystem") + "###Assets").c_str())) {
        filesystem_panel(app, intents);
    }
    ImGui::End();



    if (ImGui::Begin((AV("console") + "###Console").c_str())) {
        static int level_filter = 2; // Info+
        // A3b: the labels must OUTLIVE the Combo call. AV() returns a
        // std::string BY VALUE, so writing .c_str() of the temporary straight
        // into the array leaves five dangling pointers the moment the statement
        // ends — the same use-after-free the sky preset combo had. Own the
        // strings first, then point at them.
        const std::string level_labels[5] = {AV("console_trace"), AV("console_debug"),
                                             AV("console_info"), AV("console_warn"),
                                             AV("console_error")};
        const char* levels[5] = {level_labels[0].c_str(), level_labels[1].c_str(),
                                 level_labels[2].c_str(), level_labels[3].c_str(),
                                 level_labels[4].c_str()};

        // Live severity counts
        const std::vector<LogMessage> all_logs = app.console().filtered(LogLevel::Trace);
        const int count_all = static_cast<int>(all_logs.size());
        int count_info = 0;
        int count_warn = 0;
        int count_error = 0;
        for (const auto& log_m : all_logs) {
            if (log_m.level == LogLevel::Error) count_error++;
            else if (log_m.level == LogLevel::Warn) count_warn++;
            else count_info++;
        }

        // Row 1: Interactive quick severity filter pills
        auto draw_severity_pill = [&](const std::string& name, int lvl, int cnt, ImVec4 active_col) {
            const bool is_sel = (level_filter == lvl);
            if (is_sel) {
                ImGui::PushStyleColor(ImGuiCol_Button, active_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(active_col.x * 1.15f, active_col.y * 1.15f, active_col.z * 1.15f, 1.0f));
            }
            const std::string pill_text = name + " (" + std::to_string(cnt) + ")###pill_lvl_" + std::to_string(lvl);
            if (ImGui::Button(pill_text.c_str())) {
                level_filter = lvl;
            }
            if (is_sel) {
                ImGui::PopStyleColor(2);
            }
        };

        draw_severity_pill(AV("filter_all"), 0, count_all, ImVec4(0.25f, 0.45f, 0.85f, 0.9f));
        ImGui::SameLine();
        draw_severity_pill(level_labels[2], 2, count_info, ImVec4(0.18f, 0.58f, 0.88f, 0.9f));
        ImGui::SameLine();
        draw_severity_pill(level_labels[3], 3, count_warn, ImVec4(0.85f, 0.58f, 0.15f, 0.9f));
        ImGui::SameLine();
        draw_severity_pill(level_labels[4], 4, count_error, ImVec4(0.88f, 0.22f, 0.25f, 0.9f));

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        static int category_filter = 0;
        const char* categories[] = {"All",  "Core", "Render", "Physics", "Audio",
                                    "Network", "Asset", "Editor", "Script", "ECS",
                                    "Platform", "RHI",  "Jobs",   "Scene"};
        const LogCategory category_values[] = {
            LogCategory::All,     LogCategory::Core,    LogCategory::Render,
            LogCategory::Physics, LogCategory::Audio,   LogCategory::Network,
            LogCategory::Asset,   LogCategory::Editor,  LogCategory::Script,
            LogCategory::ECS,     LogCategory::Platform, LogCategory::RHI,
            LogCategory::Jobs,    LogCategory::Scene};
        const int cat_index =
            (category_filter >= 0 && category_filter < IM_ARRAYSIZE(category_values))
                ? category_filter
                : 0;
        const LogCategory selected = category_values[cat_index];
        static char search[128] = "";
        const nf::editor::ConsoleBuffer::Filter filter{
            static_cast<LogLevel>(level_filter),
            (selected == LogCategory::All) ? LogCategory::All : selected,
            std::string(search)};
        const std::vector<LogMessage> lines = app.console().filtered(filter);

        if (ImGui::Button((AV("copy_all_logs") + "###btn_copy_all_logs").c_str())) {
            std::string full_txt;
            for (const auto& log_m : lines) {
                full_txt += "[" + std::string(category_name(log_m.category)) + "] " + log_m.text + "\n";
            }
            ImGui::SetClipboardText(full_txt.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(AV("clear").c_str())) {
            app.console().clear();
        }
        ImGui::SameLine();
        rt_disabled_str(AVF("console_dropped", app.console().dropped()));

        // Row 2: Category, Search box, and Autoscroll
        ImGui::Combo(AV("category").c_str(), &category_filter, categories, 14);
        ImGui::SameLine();
        ImGui::PushItemWidth(220.0f);
        const std::string search_hint = AV("console_search_hint");
        ImGui::InputTextWithHint("##consolesearch", search_hint.c_str(), search,
                                 IM_ARRAYSIZE(search));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        static bool auto_scroll = true;
        ImGui::Checkbox(AV("autoscroll").c_str(), &auto_scroll);
        // --- Polished log rows: level dot + category chip + message ---------
        // Denser than the old "[LEVEL][Category] text" line: a colored dot
        // carries severity at a glance, the category is a muted chip, and the
        // message keeps full width. Same data, same filters, faster scanning.
        if (ImGui::BeginChild("##consolelines", ImVec2(0, 0), true)) {
            // Clipped like the outliner: shaping runs only for visible rows,
            // not for the whole backlog every frame.
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(lines.size()));
            while (clipper.Step()) {
                for (int li = clipper.DisplayStart; li < clipper.DisplayEnd; ++li) {
                    const LogMessage& m = lines[static_cast<size_t>(li)];
                    ImVec4 dot = level_color(m.level);
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float row_w = ImGui::GetContentRegionAvail().x;
                    const float row_h = ImGui::GetTextLineHeightWithSpacing();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    if (m.level == LogLevel::Error) {
                        dl->AddRectFilled(p, ImVec2(p.x + row_w, p.y + row_h), IM_COL32(239, 68, 68, 28), 3.0f);
                        dl->AddRect(p, ImVec2(p.x + row_w, p.y + row_h), IM_COL32(239, 68, 68, 60), 3.0f);
                    } else if (m.level == LogLevel::Warn) {
                        dl->AddRectFilled(p, ImVec2(p.x + row_w, p.y + row_h), IM_COL32(245, 158, 11, 22), 3.0f);
                        dl->AddRect(p, ImVec2(p.x + row_w, p.y + row_h), IM_COL32(245, 158, 11, 50), 3.0f);
                    }
                    ImGui::Dummy(ImVec2(1.0f, 1.0f));
                    dl->AddCircleFilled(
                        ImVec2(p.x + 6.0f, p.y + 8.0f), 4.0f,
                        IM_COL32(static_cast<int>(dot.x * 255.0f),
                                 static_cast<int>(dot.y * 255.0f),
                                 static_cast<int>(dot.z * 255.0f), 255));
                    ImGui::SameLine(0.0f, 2.0f);
                    draw_category_chip(category_name(m.category));
                    ImGui::SameLine();
                    // Message text is arbitrary (asset paths, engine errors, an
                    // Arabic entity name in a rename failure), so shape it for
                    // display and avoid the %s format path entirely.
                    const std::string shaped = ui::shape_arabic(m.text);
                    ImGui::TextColored(level_color(m.level), "%s", shaped.c_str());
                    if (ImGui::BeginPopupContextItem("##logctx")) {
                        if (ImGui::MenuItem(AV("copy_log_message").c_str())) {
                            ImGui::SetClipboardText(m.text.c_str());
                        }
                        ImGui::EndPopup();
                    }
                }
            }
            clipper.End();
            if (auto_scroll && !lines.empty()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    // --- Profiler (P4): live frame aggregates + chrome-trace export --
    // Technical diagnostics: Frame/GPU/CPU/Memory lines stay English on
    // purpose (same decision as the console level/category tags — they mirror
    // engine counters, not UI wording). Only the window TITLE translates (its
    // ###id keeps docking stable across the language flip).
    if (show_profiler_window()) {
        if (ImGui::Begin((AV("profiler") + "###ProfilerWindow").c_str())) {
            const auto& profiler = Profiler::instance();
            ImGui::Text("Frame %llu (%zu events)", profiler.frame_index(),
                        profiler.last_events().size());
            // Frame timings. GPU is submit->fence (see UiFrameStats); CPU
            // frame is dt, and the render breakdown is the renderer's own.
            ImGui::Text("GPU %.2f ms (avg %.2f)  frame %.2f ms",
                        static_cast<double>(stats.gpu_us) / 1000.0,
                        static_cast<double>(stats.gpu_avg_us) / 1000.0,
                        static_cast<double>(stats.dt_seconds) * 1000.0);
            ImGui::Text("Render CPU: cull %.1f us  draw prep %.1f us  draw calls %u  visible %u",
                        stats.cull_us, stats.draw_prep_us, stats.draw_calls, stats.visible_objects);
            ImGui::Text("Memory: %u live RHI objects  assets cached: %zu  last load %.1f ms",
                        stats.alive_objects, stats.assets_cached,
                        static_cast<double>(stats.scene_open_us) / 1000.0);
            if (ImGui::Button(AV("save_trace").c_str())) {
                namespace fs = std::filesystem;
                std::error_code ec;
                const fs::path path = nf::FileSystem::scratch_directory() / "nf_profile_trace.json";
                std::string err;
                // The session, not the last frame: the engine profiler's own
                // export would cover 1/60s, because end_frame() clears it.
                if (!ec && app.profiler_session().save_chrome_trace(path.string(), err)) {
                    push_info(app.console(),
                              std::string("Trace saved to ") + path.string() + " (" +
                                  std::to_string(app.profiler_session().event_count()) + " events)");
                } else {
                    push_error(app.console(), "Trace save failed", err.empty() ? "temp dir?" : err);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu session events)", app.profiler_session().event_count());
            if (ImGui::BeginTable("##profiletable", 4,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Zone");
                ImGui::TableSetupColumn("Calls");
                ImGui::TableSetupColumn("Incl us");
                ImGui::TableSetupColumn("Excl us");
                ImGui::TableHeadersRow();
                for (const ProfileAggregate& agg : profiler.last_aggregates()) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(agg.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", agg.calls);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", agg.inclusive_us);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", agg.exclusive_us);
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // --- Top: missing-arabic-font banner ---------------------------------------
    // Only ever visible when the UI language IS Arabic and the companion font
    // failed to load, which is the one condition that produces an entire screen
    // of tofu diamonds and no other symptom. A top strip rather than a console
    // line because the user reading the screen cannot see the console, and a
    // dismissal is a click away so it is never in the way of a fix.
    //
    // The wording is translated, which is of course ironic when the Arabic font
    // is the thing that is missing - but a Latin fallback still says something
    // useful, and the first run of this is exactly how a user learns what broke.
    if (ui_arabic_font_missing()) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float h = ImGui::GetFrameHeight() * 2.0f + 12.0f;
        if (ImGui::BeginViewportSideBar("##NFFontWarn", vp, ImGuiDir_Up, h,
                                        ImGuiWindowFlags_NoScrollbar |
                                            ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::danger());
            ImGui::PushStyleColor(ImGuiCol_Button, theme::surface_raised());
            ImGui::SetCursorPosX(12.0f);
            ImGui::TextWrapped("%s", AV("err_font_missing_ar").c_str());
            ImGui::PopStyleColor(2);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);
            if (ImGui::Button(AV("close").c_str(), ImVec2(64.0f, 0.0f))) {
                set_ui_arabic_font_missing(false);
            }
            ImGui::End();
        }
    }

    // --- Bottom: status bar ----------------------------------------------------
    // One strip that answers the three questions a user asks a hundred times a
    // session — am I editing or playing, is this saved, is anything broken —
    // without opening a panel. It reserves its own row through
    // BeginViewportSideBar, so the dockspace ends above it and the bottom panel
    // can never hide it.
    {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float bar_h = ImGui::GetFrameHeight() + 10.0f;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::surface_raised());
        if (ImGui::BeginViewportSideBar("##NFStatusBar", vp, ImGuiDir_Down, bar_h,
                                        ImGuiWindowFlags_NoScrollbar |
                                            ImGuiWindowFlags_NoSavedSettings)) {
            // A hairline on top separates the bar from the panel above it.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 wp = ImGui::GetWindowPos();
                const ImVec2 ws = ImGui::GetWindowSize();
                dl->AddLine(wp, ImVec2(wp.x + ws.x, wp.y),
                            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
            }

            // A filled dot: the colour IS the state, so "playing" is legible
            // from the corner of the eye without reading the word.
            const auto status_dot = [](const ImVec4& col) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float lh = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(p.x + lh * 0.30f, p.y + lh * 0.5f), lh * 0.22f,
                    ImGui::GetColorU32(col));
                ImGui::Dummy(ImVec2(lh * 0.68f, lh));
                ImGui::SameLine(0.0f, 6.0f);
            };
            const auto bar_sep = []() {
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::TextDisabled("|");
                ImGui::SameLine(0.0f, 12.0f);
            };

            ImGui::AlignTextToFramePadding();

            // 1. Mode.
            const bool playing = st.playing || app.playing();
            const ImVec4 mode_col = playing ? theme::danger() : theme::success();
            status_dot(mode_col);
            ImGui::TextColored(mode_col, "%s",
                               playing ? AV("status_playing").c_str()
                                       : AV("status_editing").c_str());
            bar_sep();

            // 2. Scene + save state.
            if (st.scene_label.empty()) {
                ImGui::TextDisabled("%s", AV("untitled").c_str());
            } else {
                std::string clean_label = st.scene_label;
                if (clean_label.size() >= 2 &&
                    clean_label.compare(clean_label.size() - 2, 2, " *") == 0) {
                    clean_label.erase(clean_label.size() - 2);
                }
                ImGui::TextUnformatted(ui::shape_arabic(clean_label).c_str());
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (st.dirty) {
                ImGui::TextColored(theme::warning(), "*");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", AV("status_unsaved").c_str());
                }
            } else {
                ImGui::TextColored(theme::success(), "%s", AV("status_saved").c_str());
            }

            // 3. Right group: counts, frame cost, validation. Anchored at a
            //    fraction of the width (the same trick the menu bar's status
            //    block uses) so it stays put as the window resizes.
            const float right_x = ImGui::GetWindowWidth() * 0.52f;
            if (ImGui::GetCursorPosX() < right_x) {
                ImGui::SetCursorPosX(right_x);
            } else {
                ImGui::SameLine(0.0f, 12.0f);
            }
            ImGui::TextDisabled("%s", AVF("stats_entities", st.entity_count).c_str());
            bar_sep();
            ImGui::TextDisabled("%s", AVF("selected_count", st.selected_count).c_str());
            if (st.fps > 0.0) {
                bar_sep();
                // Frame time next to FPS: the average hides the stutter, the
                // instantaneous number is what the user actually feels.
                ImGui::TextDisabled("%s %.0f  %.1f ms", AV("fps").c_str(), st.fps, st.frame_ms);
            }
            if (stats.validation_on) {
                bar_sep();
                if (stats.validation_errors == 0) {
                    ImGui::TextColored(theme::success(), "%s", AV("status_ok").c_str());
                } else {
                    ImGui::TextColored(theme::danger(), "%s",
                                       AVF("status_errors", stats.validation_errors).c_str());
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    draw_start_here(app);

    return intents;
}

} // namespace nf::editor
