// ScenePanels.cpp — the dedicated editors, one per engine function.
//
// See ScenePanels.hpp for why these exist next to the Inspector. The short
// version: the Inspector edits the SELECTION, these edit the SCENE OBJECT the
// renderer actually consumes, and each one says out loud which object that is.
//
// Two rules every panel here follows:
//   1. Writes go through EditorApp's validated command path (set_light /
//      set_sky / set_camera / set_transform), so an edit made here is undoable
//      and validated exactly like the same edit from the Inspector. Nothing
//      touches the ECS directly.
//   2. Read-only facts are `kv` rows, edits are labelled fields, and the one
//      committing action per section is a full-width primary button. That
//      rhythm is what makes a column of panels look like one product.

#define _CRT_SECURE_NO_WARNINGS
#include <NF/Editor/ScenePanels.hpp>

#include <NF/Editor/Inspector.hpp>
#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Editor/UiText.hpp>
#include <NF/Editor/UiTheme.hpp>
#include <NF/Editor/UiWidgets.hpp>

#include <NF/AI/AIWorld.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/Core/FileSystem.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Vfx/Components.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nf::editor {

namespace {

using uiw::chip;
using uiw::heading;
using uiw::hint;
using uiw::kv;
using uiw::primary_button;
using uiw::row_label;
using uiw::section;

constexpr float kPi = 3.14159265358979323846f;

/// snprintf into a std::string. Variadic because half the readouts are two or
/// three numbers, and a per-arity overload set is noise.
template <typename... Args>
std::string fmt(const char* f, Args... args) {
    char b[160] = {};
    std::snprintf(b, sizeof(b), f, args...);
    return b;
}

void push_info(ConsoleBuffer& console, const std::string& what) {
    console.push(LogMessage{LogLevel::Info, LogCategory::Editor, what,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

void push_error(ConsoleBuffer& console, const std::string& what, const std::string& err) {
    console.push(LogMessage{LogLevel::Error, LogCategory::Editor, what + ": " + err,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

/// Display name of a bound entity, or empty when there is nothing to bind to.
std::string entity_name(const ecs::World& world, ecs::Entity e) {
    if (!e.valid()) {
        return {};
    }
    if (const auto* n = world.get<scene::NameComponent>(e)) {
        if (!n->name.empty()) {
            return ui::shape_arabic(n->name);
        }
    }
    return "Entity " + std::to_string(e.id);
}

// --- Sun dial ---------------------------------------------------------------
//
// A light direction is a 3-vector, which is a terrible thing to type. Every DCC
// tool offers the same answer: azimuth around the horizon, elevation above it,
// on a dial you drag. This is that dial — the sky half is drawn as a disc, the
// sun as a dot, and dragging the dot writes both angles.
//
// The angle convention matches the component: `dir` is the direction the light
// TRAVELS, so the sun sits at -dir.
struct SunAngles {
    float azimuth_deg = 0.0f;   // 0..360, clockwise from +Z
    float elevation_deg = 45.0f; // 0 = horizon, 90 = straight up
};

SunAngles angles_from_dir(float x, float y, float z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len <= 1e-6f) {
        return SunAngles{};
    }
    const float nx = x / len;
    const float ny = y / len;
    const float nz = z / len;
    SunAngles a;
    const float up = -ny;
    a.elevation_deg = std::asin(up < -1.0f ? -1.0f : (up > 1.0f ? 1.0f : up)) * 180.0f / kPi;
    a.azimuth_deg = std::atan2(-nx, -nz) * 180.0f / kPi;
    if (a.azimuth_deg < 0.0f) {
        a.azimuth_deg += 360.0f;
    }
    return a;
}

void dir_from_angles(const SunAngles& a, float& x, float& y, float& z) {
    const float az = a.azimuth_deg * kPi / 180.0f;
    const float el = a.elevation_deg * kPi / 180.0f;
    const float ce = std::cos(el);
    x = -ce * std::sin(az);
    y = -std::sin(el);
    z = -ce * std::cos(az);
}

/// Draws the dial and returns true while the user is dragging it.
bool sun_dial(SunAngles& a, float size) {
    ImGui::PushID("##sundial");
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##dial", ImVec2(size, size));
    const bool dragging = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float r = size * 0.44f;

    // Sky dome: a vertical gradient disc reads as "above the horizon" without
    // a single label.
    dl->AddCircleFilled(c, r, IM_COL32(38, 44, 56, 255), 48);
    const ImU32 sky_top = ImGui::GetColorU32(theme::alpha(theme::accent(), 0.30f));
    dl->PathArcTo(c, r * 0.98f, kPi, 2.0f * kPi, 32);
    dl->PathStroke(sky_top, 0, 2.0f);
    // Horizon + compass ticks.
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(120, 128, 140, 160), 1.0f);
    for (int i = 0; i < 12; ++i) {
        const float ang = static_cast<float>(i) / 12.0f * 2.0f * kPi;
        const float inner = (i % 3 == 0) ? r * 0.86f : r * 0.93f;
        dl->AddLine(ImVec2(c.x + std::sin(ang) * inner, c.y + std::cos(ang) * inner),
                    ImVec2(c.x + std::sin(ang) * r, c.y + std::cos(ang) * r),
                    IM_COL32(110, 118, 130, 140), 1.0f);
    }

    // The sun: distance from centre = elevation, angle = azimuth. Below the
    // horizon it is drawn dimmed, because a light from under the floor is
    // almost always a mistake worth seeing.
    const float az = a.azimuth_deg * kPi / 180.0f;
    const float el = a.elevation_deg * kPi / 180.0f;
    const float rr = r * (1.0f - el / (kPi * 0.5f)) * 0.92f;
    const ImVec2 sun(c.x + std::sin(az) * rr, c.y + std::cos(az) * rr);
    const bool below = a.elevation_deg < 0.0f;
    const ImVec4 sun_col = below ? theme::warning() : ImVec4(1.0f, 0.86f, 0.45f, 1.0f);
    dl->AddLine(c, sun, ImGui::GetColorU32(theme::alpha(sun_col, 0.35f)), 1.5f);
    dl->AddCircleFilled(sun, 6.5f, ImGui::GetColorU32(sun_col), 20);
    dl->AddCircle(sun, 6.5f, IM_COL32(20, 22, 26, 200), 20, 1.5f);

    if (dragging) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float dx = m.x - c.x;
        const float dy = m.y - c.y;
        float deg = std::atan2(dx, dy) * 180.0f / kPi;
        if (deg < 0.0f) {
            deg += 360.0f;
        }
        a.azimuth_deg = deg;
        const float dist = std::sqrt(dx * dx + dy * dy);
        const float frac = (r > 0.0f) ? (dist / r) : 0.0f;
        a.elevation_deg = (1.0f - (frac > 1.0f ? 1.0f : frac)) * 90.0f;
    }
    ImGui::PopID();
    return dragging;
}

// --- Lighting ---------------------------------------------------------------

/// Cached edit state, reset when the bound light changes (same contract the
/// Inspector's cache keeps).
struct LightCache {
    ecs::Entity entity;
    LightEdit edit{};
    SunAngles angles{};
    bool valid = false;
};

LightCache& light_cache() {
    static LightCache c;
    return c;
}

void lighting_panel(EditorApp& app) {
    ecs::World* w = app.world();
    if (w == nullptr) {
        return;
    }
    const ecs::Entity light = scene_light_entity(*w);
    LightCache& c = light_cache();
    if (!c.valid || c.entity != light) {
        c.entity = light;
        c.valid = true;
        if (light.valid()) {
            bool has = false;
            c.edit = read_light(*w, light, has);
            c.angles = angles_from_dir(c.edit.dir_x, c.edit.dir_y, c.edit.dir_z);
        }
    }

    uiw::bound_header(AV("panel_lighting"), entity_name(*w, light));
    if (!light.valid()) {
        hint(AV("lighting_none_hint"));
        if (primary_button(AV("add_light"))) {
            std::string err;
            if (!app.create_directional_light(ecs::kInvalidEntity, err)) {
                push_error(app.console(), "Create light failed", err);
            }
        }
        return;
    }

    if (section(AV("sun").c_str())) {
        const float dial = 128.0f;
        const float x0 = ImGui::GetCursorPosX();
        sun_dial(c.angles, dial);
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("azimuth") + "##az").c_str(), &c.angles.azimuth_deg, 1.0f, 0.0f,
                         360.0f, AVF("deg_fmt", 0.0).c_str());
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("elevation") + "##el").c_str(), &c.angles.elevation_deg, 0.5f, -10.0f,
                         90.0f, AVF("deg_fmt", 0.0).c_str());
        ImGui::Spacing();
        // Recompute BEFORE printing. The dials above write the angles, and the
        // direction readout used to be printed first and derived second, so it
        // showed the PREVIOUS frame's angles: the numbers sat still while the
        // dial the user was holding moved, which reads as a broken display
        // rather than a one-frame lag.
        dir_from_angles(c.angles, c.edit.dir_x, c.edit.dir_y, c.edit.dir_z);
        ImGui::TextDisabled("%s", AV("direction").c_str());
        ImGui::Text("%.3f  %.3f  %.3f", c.edit.dir_x, c.edit.dir_y, c.edit.dir_z);
        ImGui::EndGroup();
        (void)x0;
        // Dragging the dial writes the direction immediately in the edit cache;
        // the Apply below is what commits it (and what makes it undoable).
    }

    if (section(AV("color").c_str())) {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::ColorEdit3("##lightcol", &c.edit.color_r,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat((AV("intensity") + "##li").c_str(), &c.edit.intensity, 0.02f, 0.0f, 20.0f,
                         "%.2f");
        (void)x0;
    }

    if (section(AV("shadows").c_str())) {
        ImGui::Checkbox((AV("cast_shadows") + "##lc").c_str(), &c.edit.cast_shadows);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("shadow_strength") + "##ls").c_str(), &c.edit.shadow_strength, 0.02f,
                         0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("shadow_bias") + "##lb").c_str(), &c.edit.shadow_bias, 0.00001f, 0.0f,
                         0.01f, "%.5f");
        const int max_cascades = static_cast<int>(rendering::kMaxShadowCascades);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderInt((AV("shadow_cascades") + "##ln").c_str(), &c.edit.shadow_cascades, 1,
                         max_cascades);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("shadow_distance") + "##ld").c_str(), &c.edit.shadow_distance, 1.0f,
                         0.0f, 2000.0f, "%.0f");
        hint(AV("shadow_distance_hint"));
    }

    ImGui::Spacing();
    if (primary_button(AV("apply"))) {
        std::string err;
        if (!app.set_light(light, c.edit, err)) {
            push_error(app.console(), "Apply light failed", err);
        } else {
            push_info(app.console(), "Lighting updated");
        }
    }
}

// --- Environment ------------------------------------------------------------

struct SkyCache {
    ecs::Entity entity;
    SkyEdit edit{};
    float time_of_day = 12.0f;
    bool valid = false;
};

SkyCache& sky_cache() {
    static SkyCache c;
    return c;
}

/// Palette for a time of day, blended between the same three presets the
/// Settings menu offers. The curve is deliberately simple and readable: night
/// until 5, dawn to 8, day until 17, dusk to 20, night after.
runtime::SkyComponent sky_at_hour(float hour) {
    const runtime::SkyComponent night = sky_preset_values(2);
    const runtime::SkyComponent golden = sky_preset_values(1);
    const runtime::SkyComponent day = sky_preset_values(0);
    auto lerp = [](const runtime::SkyComponent& a, const runtime::SkyComponent& b, float t) {
        runtime::SkyComponent o{};
        o.zenith_r = a.zenith_r + (b.zenith_r - a.zenith_r) * t;
        o.zenith_g = a.zenith_g + (b.zenith_g - a.zenith_g) * t;
        o.zenith_b = a.zenith_b + (b.zenith_b - a.zenith_b) * t;
        o.horizon_r = a.horizon_r + (b.horizon_r - a.horizon_r) * t;
        o.horizon_g = a.horizon_g + (b.horizon_g - a.horizon_g) * t;
        o.horizon_b = a.horizon_b + (b.horizon_b - a.horizon_b) * t;
        o.ground_r = a.ground_r + (b.ground_r - a.ground_r) * t;
        o.ground_g = a.ground_g + (b.ground_g - a.ground_g) * t;
        o.ground_b = a.ground_b + (b.ground_b - a.ground_b) * t;
        o.sun_disk = a.sun_disk + (b.sun_disk - a.sun_disk) * t;
        o.sun_glow = a.sun_glow + (b.sun_glow - a.sun_glow) * t;
        return o;
    };
    const float h = (hour < 0.0f) ? 0.0f : ((hour > 24.0f) ? 24.0f : hour);
    if (h < 5.0f) {
        return night;
    }
    if (h < 8.0f) {
        return lerp(night, golden, (h - 5.0f) / 3.0f);
    }
    if (h < 10.0f) {
        return lerp(golden, day, (h - 8.0f) / 2.0f);
    }
    if (h < 16.0f) {
        return day;
    }
    if (h < 18.0f) {
        return lerp(day, golden, (h - 16.0f) / 2.0f);
    }
    if (h < 21.0f) {
        return lerp(golden, night, (h - 18.0f) / 3.0f);
    }
    return night;
}

void sky_to_edit(const runtime::SkyComponent& s, SkyEdit& e) {
    e.enabled = true;
    e.zenith[0] = s.zenith_r; e.zenith[1] = s.zenith_g; e.zenith[2] = s.zenith_b;
    e.horizon[0] = s.horizon_r; e.horizon[1] = s.horizon_g; e.horizon[2] = s.horizon_b;
    e.ground[0] = s.ground_r; e.ground[1] = s.ground_g; e.ground[2] = s.ground_b;
    e.sun_disk = s.sun_disk;
    e.sun_glow = s.sun_glow;
}

void environment_panel(EditorApp& app) {
    ecs::World* w = app.world();
    if (w == nullptr) {
        return;
    }
    const ecs::Entity sky = scene_sky_entity(*w);
    SkyCache& c = sky_cache();
    if (!c.valid || c.entity != sky) {
        c.entity = sky;
        c.valid = true;
        if (sky.valid()) {
            bool has = false;
            c.edit = read_sky(*w, sky, has);
        }
    }

    uiw::bound_header(AV("panel_environment"), entity_name(*w, sky));
    if (!sky.valid()) {
        hint(AV("environment_none_hint"));
        if (primary_button(AV("add_sky"))) {
            std::string err;
            if (!app.create_sky_entity(ecs::kInvalidEntity, err)) {
                push_error(app.console(), "Create sky failed", err);
            }
        }
        return;
    }

    // Presets: one click each, applied through the same path the Settings menu
    // uses so a preset here and a preset there cannot drift apart.
    if (section(AV("presets").c_str())) {
        const char* keys[3] = {"sky_day", "sky_sunset", "sky_night"};
        const float w3 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) /
                         3.0f;
        for (int i = 0; i < 3; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            if (ImGui::Button(AV(keys[i]).c_str(), ImVec2(w3, 0.0f))) {
                std::string err;
                const SkyEdit edit = sky_preset_edit(i);
                if (!app.apply_sky_edit(edit, err)) {
                    push_error(app.console(), "Apply sky preset failed", err);
                } else {
                    c.edit = edit;
                    c.valid = false; // re-read next frame
                    push_info(app.console(), std::string("Sky preset: ") + keys[i]);
                }
            }
        }
    }

    if (section(AV("time_of_day").c_str())) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool moved = ImGui::SliderFloat("##tod", &c.time_of_day, 0.0f, 24.0f, "%.1f h");
        const int hh = static_cast<int>(c.time_of_day);
        const int mm = static_cast<int>((c.time_of_day - static_cast<float>(hh)) * 60.0f);
        kv(AV("clock"), fmt("%02d:%02d", static_cast<double>(hh), static_cast<double>(mm)));
        // Live preview while dragging: the sky is the one thing a user judges by
        // looking at it, and waiting for a button to see it is the wrong feel.
        if (moved || ImGui::IsItemDeactivatedAfterEdit()) {
            runtime::SkyComponent s = sky_at_hour(c.time_of_day);
            s.clear_r = c.edit.clear[0];
            s.clear_g = c.edit.clear[1];
            s.clear_b = c.edit.clear[2];
            SkyEdit e{};
            sky_to_edit(s, e);
            e.clear[0] = c.edit.clear[0];
            e.clear[1] = c.edit.clear[1];
            e.clear[2] = c.edit.clear[2];
            std::string err;
            if (!app.apply_sky_edit(e, err)) {
                push_error(app.console(), "Time of day failed", err);
            } else {
                c.edit = e;
            }
        }
        hint(AV("time_of_day_hint"));
    }

    if (section(AV("palette").c_str())) {
        ImGui::Checkbox((AV("enabled") + "##sky_en").c_str(), &c.edit.enabled);
        const float x0 = ImGui::GetCursorPosX();
        row_label(AV("zenith"), 110.0f);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::ColorEdit3("##zen", c.edit.zenith, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SetCursorPosX(x0);
        row_label(AV("horizon"), 110.0f);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::ColorEdit3("##hor", c.edit.horizon, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SetCursorPosX(x0);
        row_label(AV("ground"), 110.0f);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::ColorEdit3("##gnd", c.edit.ground, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SetCursorPosX(x0);
        row_label(AV("clear_color"), 110.0f);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::ColorEdit3("##clr", c.edit.clear, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    }

    if (section(AV("sun_disk").c_str())) {
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("sun_disk") + "##sd").c_str(), &c.edit.sun_disk, 0.02f, 0.0f, 5.0f,
                         "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("sun_glow") + "##sg").c_str(), &c.edit.sun_glow, 0.02f, 0.0f, 5.0f,
                         "%.2f");
    }

    if (section(AV("weather_system").c_str())) {
        static int env_weather_preset = 0;
        const char* w_names[7] = {
            "weather_clear", "weather_cloudy", "weather_overcast",
            "weather_rain", "weather_thunderstorm", "weather_foggy", "weather_snow"
        };
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        for (int i = 0; i < 7; ++i) {
            if (i % 3 != 0) ImGui::SameLine();
            if (ImGui::Button(AV(w_names[i]).c_str(), ImVec2(bw, 0.0f))) {
                env_weather_preset = i;
                push_info(app.console(), std::string("Atmospheric weather set to: ") + w_names[i]);
            }
        }
        static float env_fog_density = 0.015f;
        static float env_fog_falloff = 0.08f;
        static float env_sun_scattering = 0.35f;
        static float env_ground_wetness = 0.0f;
        static float env_wind_speed = 2.5f;

        ImGui::SliderFloat((AV("fog_density") + "##env_fd").c_str(), &env_fog_density, 0.0f, 0.15f, "%.4f");
        ImGui::SliderFloat((AV("fog_falloff") + "##env_ff").c_str(), &env_fog_falloff, 0.005f, 0.3f, "%.3f");
        ImGui::SliderFloat((AV("sun_inscattering") + "##env_ss").c_str(), &env_sun_scattering, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat((AV("ground_wetness") + "##env_gw").c_str(), &env_ground_wetness, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat((AV("wind_strength") + "##env_ws").c_str(), &env_wind_speed, 0.0f, 30.0f, "%.1f m/s");
    }

    ImGui::Spacing();
    if (primary_button(AV("apply"))) {
        std::string err;
        if (!app.set_sky(sky, c.edit, err)) {
            push_error(app.console(), "Apply sky failed", err);
        } else {
            push_info(app.console(), "Environment updated");
        }
    }
}

// --- Camera -----------------------------------------------------------------

struct CameraCache {
    ecs::Entity entity;
    CameraEdit edit{};
    TransformEdit xf{};
    bool valid = false;
};

CameraCache& camera_cache() {
    static CameraCache c;
    return c;
}

void camera_panel(EditorApp& app) {
    ecs::World* w = app.world();
    if (w == nullptr) {
        return;
    }
    const std::vector<ecs::Entity> cams = w->query<runtime::CameraComponent>();
    const ecs::Entity cam = cams.empty() ? ecs::kInvalidEntity : cams.front();
    CameraCache& c = camera_cache();
    if (!c.valid || c.entity != cam) {
        c.entity = cam;
        c.valid = true;
        if (cam.valid()) {
            bool has = false;
            c.edit = read_camera(*w, cam, has);
            c.xf = read_transform(*w, cam);
        }
    }

    uiw::bound_header(AV("panel_camera"), entity_name(*w, cam));
    if (!cam.valid()) {
        hint(AV("camera_none_hint"));
        if (primary_button(AV("add_camera"))) {
            std::string err;
            if (!app.create_camera(ecs::kInvalidEntity, err)) {
                push_error(app.console(), "Create camera failed", err);
            }
        }
        return;
    }

    // The renderer's own camera, so the numbers here are the ones being used.
    if (runtime::Runtime* rt = app.runtime(); rt != nullptr) {
        rendering::Camera live{};
        if (rt->extract_camera(std::max<uint32_t>(1u, app.viewport().width),
                               std::max<uint32_t>(1u, app.viewport().height), live)) {
            kv(AV("aspect"), fmt("%.3f", static_cast<double>(live.aspect)));
            kv(AV("position"),
               fmt("%.2f  %.2f  %.2f", static_cast<double>(live.position.x),
                   static_cast<double>(live.position.y), static_cast<double>(live.position.z)));
            kv(AV("target"),
               fmt("%.2f  %.2f  %.2f", static_cast<double>(live.target.x),
                   static_cast<double>(live.target.y), static_cast<double>(live.target.z)));
            ImGui::Spacing();
        }
    }

    if (section(AV("lens").c_str())) {
        ImGui::Checkbox((AV("active") + "##cam_act").c_str(), &c.edit.active);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("fov") + "##cf").c_str(), &c.edit.fov_y, 0.5f, 5.0f, 150.0f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("near") + "##cn").c_str(), &c.edit.near_plane, 0.005f, 0.001f, 10.0f,
                         "%.3f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("far") + "##cfa").c_str(), &c.edit.far_plane, 1.0f, 1.0f, 100000.0f,
                         "%.0f");
    }

    // The follow camera. A camera that always looks at the origin cannot
    // show a third-person game, so the panel exposes the orbit: the entity
    // to follow, the pivot on it, the distance and the angles, how fast the
    // camera catches up, and what it does when a tree is in the way.
    if (section(AV("follow").c_str())) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText((AV("follow_target") + "##cft").c_str(), c.edit.follow_target.data(),
                         c.edit.follow_target.capacity());
        hint(AV("follow_hint"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat3((AV("follow_pivot") + "##cpv").c_str(), &c.edit.pivot_x, 0.05f);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("follow_distance") + "##cd").c_str(), &c.edit.distance, 0.1f, 0.0f,
                         100.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("follow_yaw") + "##cy").c_str(), &c.edit.yaw, 0.5f, -360.0f, 360.0f,
                         "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("follow_pitch") + "##cpi").c_str(), &c.edit.pitch, 0.5f, -89.0f,
                         89.0f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("follow_smoothing") + "##cs").c_str(), &c.edit.smoothing, 0.1f, 0.0f,
                         60.0f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("follow_collision") + "##cc").c_str(), &c.edit.collision_radius, 0.05f,
                         0.0f, 10.0f, "%.2f");
        ImGui::Checkbox((AV("follow_avoid") + "##ca").c_str(), &c.edit.avoid_collision);
    }

    // Aiming (0.4.0). The follow camera above decides where the camera is
    // when nobody touches it; these decide how it answers a hand. They are
    // the difference between a camera the player watches and a camera the
    // player turns, and every one of them is a taste — which is why they are
    // scene data and not constants in the runtime.
    if (section(AV("aim").c_str())) {
        ImGui::Checkbox((AV("aim_mouse") + "##cam_mouse").c_str(), &c.edit.mouse_look);
        hint(AV("aim_hint"));
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_sensitivity") + "##cam_sens").c_str(), &c.edit.look_sensitivity,
                         0.005f, 0.001f, 5.0f, "%.3f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_stick") + "##cam_stick").c_str(), &c.edit.stick_sensitivity, 1.0f,
                         0.0f, 1080.0f, "%.0f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_pitch_min") + "##cam_pmin").c_str(), &c.edit.pitch_min, 0.5f,
                         -89.9f, 89.9f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_pitch_max") + "##cam_pmax").c_str(), &c.edit.pitch_max, 0.5f,
                         -89.9f, 89.9f, "%.1f");
        ImGui::Checkbox((AV("aim_invert_y") + "##cam_inv").c_str(), &c.edit.invert_look_y);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_zoom_min") + "##cam_zmin").c_str(), &c.edit.zoom_min, 0.05f, 0.1f,
                         100.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_zoom_max") + "##cam_zmax").c_str(), &c.edit.zoom_max, 0.1f, 0.1f,
                         100.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_zoom_step") + "##cam_zstep").c_str(), &c.edit.zoom_step, 0.02f,
                         0.01f, 10.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_align") + "##cam_align").c_str(), &c.edit.align_speed, 0.5f, 0.0f,
                         720.0f, "%.0f");

        // Aiming down sights (0.5.0). Holding `aim` pulls the camera to
        // `ads_distance` and scales the look by `ads_sensitivity`; the speed
        // is how fast the blend gets there, and 0 snaps.
        ImGui::Separator();
        ImGui::Checkbox((AV("aim_ads") + "##cam_ads").c_str(), &c.edit.ads_enabled);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_ads_distance") + "##cam_adsd").c_str(), &c.edit.ads_distance,
                         0.05f, 0.1f, 50.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_ads_sensitivity") + "##cam_adss").c_str(),
                         &c.edit.ads_sensitivity, 0.01f, 0.05f, 2.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_ads_speed") + "##cam_adsp").c_str(), &c.edit.ads_speed, 0.5f,
                         0.0f, 60.0f, "%.1f");

        // Camera shake trim. The trauma itself comes from scripts
        // (nf.shake), so these two say how much of it this camera shows and
        // how fast it forgets.
        ImGui::Separator();
        ImGui::TextUnformatted(AV("aim_shake").c_str());
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_shake_strength") + "##cam_shs").c_str(), &c.edit.shake_strength,
                         0.01f, 0.0f, 4.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_shake_decay") + "##cam_shd").c_str(), &c.edit.shake_decay, 0.05f,
                         0.0f, 20.0f, "%.2f");

        // Recoil. The kick itself comes from scripts (nf.recoil), so these
        // two say how fast the view settles and how far it may be kicked.
        ImGui::Separator();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_recoil") + "##cam_rec").c_str(), &c.edit.recoil_recover, 0.1f,
                         0.0f, 60.0f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("aim_recoil_max") + "##cam_recm").c_str(), &c.edit.recoil_max, 0.5f,
                         0.5f, 90.0f, "%.1f");
    }

    // Player settings (0.5.0). NOT scene data: these are the overrides the
    // player owns, stored in a file outside the project and applied on top of
    // whatever the camera above says. A scale of 1.0 and an FOV of 0 mean
    // "the scene's", which is why an untouched panel changes nothing.
    if (section(AV("player_settings").c_str())) {
        // Load once, so the panel shows the FILE rather than the defaults.
        if (!app.player_settings_loaded()) {
            std::string ignored;
            app.reload_player_settings(ignored); // a missing file is normal
        }
        runtime::PlayerSettings& ps = app.player_settings();
        ImGui::TextDisabled("%s: %s%s", AV("settings_file").c_str(),
                            runtime::Runtime::kDefaultPlayerSettingsPath,
                            app.player_settings_file_exists() ? "" : " (none)");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("look_scale") + "##ps_look").c_str(), &ps.look_scale, 0.01f, 0.05f,
                         10.0f, "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("stick_scale") + "##ps_stick").c_str(), &ps.stick_scale, 0.01f, 0.05f,
                         10.0f, "%.2f");
        {
            // Three states, not a checkbox: "auto" keeps the scene's choice,
            // and a bool cannot say "the player has no opinion".
            const std::string auto_label = AV("invert_auto");
            const std::string on_label = AV("invert_on");
            const std::string off_label = AV("invert_off");
            const char* items[] = {auto_label.c_str(), on_label.c_str(), off_label.c_str()};
            int mode = static_cast<int>(ps.invert_y);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo((AV("invert_y") + "##ps_inv").c_str(), &mode, items, 3)) {
                ps.invert_y = static_cast<runtime::InvertY>(mode);
            }
        }
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("fov") + "##ps_fov").c_str(), &ps.fov_y, 0.5f, 0.0f, 140.0f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("ads_scale") + "##ps_ads").c_str(), &ps.ads_scale, 0.01f, 0.05f, 10.0f,
                         "%.2f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("shake_scale") + "##ps_shake").c_str(), &ps.shake_scale, 0.01f, 0.0f,
                         4.0f, "%.2f");

        // Mixer volumes, one per bus. The mixer reads them every block, so
        // moving a slider is audible on the next block — no reload, no scene
        // rebuild. The five names are the mixer's own, so a settings file and
        // an audio log call "master" the same thing.
        ImGui::Separator();
        auto volume_slider = [&](const char* label, const char* id, audio::BusId bus) {
            f32 value = ps.volumes.volume(bus);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderFloat((AV(label) + "##ps_" + id).c_str(), &value, 0.0f, 1.0f, "%.2f")) {
                ps.volumes.set_volume(bus, value);
            }
        };
        volume_slider("master_volume", "vol_master", audio::BusId::Master);
        volume_slider("music_volume", "vol_music", audio::BusId::Music);
        volume_slider("sfx_volume", "vol_sfx", audio::BusId::Sfx);
        volume_slider("ambience_volume", "vol_amb", audio::BusId::Ambience);
        volume_slider("voice_volume", "vol_voice", audio::BusId::Voice);

        // Master dynamics. OFF by default, and each stage is independent:
        // a player may want the limiter's protection without a compressor
        // changing the mix's punch. Keeping them here (not in the scene) is
        // the whole point — the level's master stays exactly as authored.
        ImGui::Separator();
        ImGui::TextDisabled("%s", AV("audio_dynamics").c_str());
        ImGui::Checkbox((AV("compressor") + "##ps_comp").c_str(),
                        &ps.dynamics.compressor.enabled);
        {
            const bool on = ps.dynamics.compressor.enabled;
            ImGui::BeginDisabled(!on);
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_threshold") + "##ps_comp_th").c_str(),
                             &ps.dynamics.compressor.threshold_db, 0.5f, -96.0f, 0.0f,
                             "%.1f dB");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_ratio") + "##ps_comp_ratio").c_str(),
                             &ps.dynamics.compressor.ratio, 0.05f, 1.0f, 20.0f, "%.2f:1");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_attack") + "##ps_comp_atk").c_str(),
                             &ps.dynamics.compressor.attack_ms, 0.5f, 0.0f, 500.0f,
                             "%.1f ms");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_release") + "##ps_comp_rel").c_str(),
                             &ps.dynamics.compressor.release_ms, 1.0f, 0.0f, 5000.0f,
                             "%.0f ms");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_knee") + "##ps_comp_knee").c_str(),
                             &ps.dynamics.compressor.knee_db, 0.1f, 0.0f, 24.0f, "%.1f dB");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_makeup") + "##ps_comp_makeup").c_str(),
                             &ps.dynamics.compressor.makeup_db, 0.1f, -24.0f, 24.0f,
                             "%.1f dB");
            ImGui::EndDisabled();
        }
        ImGui::Checkbox((AV("limiter") + "##ps_lim").c_str(), &ps.dynamics.limiter.enabled);
        {
            const bool on = ps.dynamics.limiter.enabled;
            ImGui::BeginDisabled(!on);
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_ceiling") + "##ps_lim_ceil").c_str(),
                             &ps.dynamics.limiter.ceiling_db, 0.05f, -24.0f, 0.0f,
                             "%.2f dB");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_release") + "##ps_lim_rel").c_str(),
                             &ps.dynamics.limiter.release_ms, 1.0f, 0.0f, 5000.0f,
                             "%.0f ms");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat((AV("dyn_lookahead") + "##ps_lim_la").c_str(),
                             &ps.dynamics.limiter.lookahead_ms, 0.1f, 0.0f, 50.0f,
                             "%.1f ms");
            ImGui::EndDisabled();
        }

        // Player profiles. One machine, more than one player: a profile is a
        // named copy of everything above, and switching copies it over the
        // active file the game reads — so the runtime keeps its one-file
        // contract and a later session starts on the chosen profile.
        ImGui::Separator();
        ImGui::TextDisabled("%s", AV("profiles").c_str());
        static int selected_profile = 0;
        const std::vector<std::string> profiles = app.player_profiles();
        if (!profiles.empty()) {
            if (selected_profile >= static_cast<int>(profiles.size())) {
                selected_profile = 0;
            }
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo((AV("profiles") + "##ps_combo").c_str(),
                                  profiles[selected_profile].c_str())) {
                for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
                    const bool is_selected = (i == selected_profile);
                    if (ImGui::Selectable(profiles[i].c_str(), is_selected)) {
                        selected_profile = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button((AV("load_profile") + "##ps_load").c_str())) {
                std::string err;
                if (!app.load_player_profile(profiles[selected_profile], err)) {
                    push_error(app.console(), "Load profile failed", err);
                } else {
                    push_info(app.console(), "Profile loaded: " + profiles[selected_profile]);
                }
            }
        }
        static char profile_name[33] = "";
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputText((AV("profile_name") + "##ps_pname").c_str(), profile_name,
                         sizeof(profile_name));
        ImGui::SameLine();
        if (ImGui::Button((AV("save_profile") + "##ps_psave").c_str())) {
            std::string err;
            if (!app.save_player_profile(profile_name, err)) {
                push_error(app.console(), "Save profile failed", err);
            } else {
                push_info(app.console(), std::string("Profile saved: ") + profile_name);
            }
        }
        hint(AV("profile_hint"));

        if (ImGui::Button((AV("apply") + "##ps_apply").c_str())) {
            app.apply_player_settings();
        }
        ImGui::SameLine();
        if (ImGui::Button((AV("save") + "##ps_save").c_str())) {
            std::string err;
            if (!app.save_player_settings(err)) {
                push_error(app.console(), "Save player settings failed", err);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button((AV("reload") + "##ps_reload").c_str())) {
            std::string err;
            if (!app.reload_player_settings(err)) {
                push_error(app.console(), "Reload player settings failed", err);
            }
        }
    }

    if (section(AV("transform").c_str())) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat3((AV("position") + "##cp").c_str(), &c.xf.px, 0.05f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat3((AV("rotation") + "##cr").c_str(), &c.xf.rx, 0.5f);
        hint(AV("camera_transform_hint"));
    }

    ImGui::Spacing();
    if (primary_button(AV("apply"))) {
        std::string err;
        // Two validated commands, two undo steps — the same thing the Inspector
        // does when the user edits the camera and the transform in one pass.
        const bool xf_ok = app.set_transform(cam, c.xf, err);
        if (!xf_ok) {
            push_error(app.console(), "Apply camera transform failed", err);
        }
        if (!app.set_camera(cam, c.edit, err)) {
            push_error(app.console(), "Apply camera failed", err);
        } else if (xf_ok) {
            push_info(app.console(), "Camera updated");
        }
    }
}


// --- Input bindings ---------------------------------------------------------

namespace {

/// ImGui key -> Windows virtual-key code, for the keys a player actually
/// binds. Deliberately not exhaustive: a key that is not here simply cannot
/// be captured, which is better than guessing a code and writing a binding
/// that never fires.
bool imgui_key_to_vk(ImGuiKey key, nf::u16& out) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        out = static_cast<nf::u16>('A' + (key - ImGuiKey_A));
        return true;
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        out = static_cast<nf::u16>('0' + (key - ImGuiKey_0));
        return true;
    }
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
        out = static_cast<nf::u16>(0x70 + (key - ImGuiKey_F1));
        return true;
    }
    switch (key) {
    case ImGuiKey_Space: out = 0x20; return true;
    case ImGuiKey_Enter: out = 0x0D; return true;
    case ImGuiKey_Tab: out = 0x09; return true;
    case ImGuiKey_Backspace: out = 0x08; return true;
    case ImGuiKey_Escape: out = 0x1B; return true;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: out = 0x10; return true;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: out = 0x11; return true;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: out = 0x12; return true;
    case ImGuiKey_UpArrow: out = 0x26; return true;
    case ImGuiKey_DownArrow: out = 0x28; return true;
    case ImGuiKey_LeftArrow: out = 0x25; return true;
    case ImGuiKey_RightArrow: out = 0x27; return true;
    case ImGuiKey_Delete: out = 0x2E; return true;
    case ImGuiKey_Insert: out = 0x2D; return true;
    case ImGuiKey_Home: out = 0x24; return true;
    case ImGuiKey_End: out = 0x23; return true;
    case ImGuiKey_PageUp: out = 0x21; return true;
    case ImGuiKey_PageDown: out = 0x22; return true;
    case ImGuiKey_Minus: out = 0xBD; return true;
    case ImGuiKey_Equal: out = 0xBB; return true;
    case ImGuiKey_Comma: out = 0xBC; return true;
    case ImGuiKey_Period: out = 0xBE; return true;
    case ImGuiKey_Slash: out = 0xBF; return true;
    case ImGuiKey_Semicolon: out = 0xBA; return true;
    case ImGuiKey_Apostrophe: out = 0xDE; return true;
    case ImGuiKey_LeftBracket: out = 0xDB; return true;
    case ImGuiKey_RightBracket: out = 0xDD; return true;
    case ImGuiKey_Backslash: out = 0xDC; return true;
    case ImGuiKey_GraveAccent: out = 0xC0; return true;
    default: return false;
    }
}

/// One binding, as the file spells it: `-key:A`, `pad:lb`, `mouse:right`.
std::string binding_label(const nf::runtime::InputBinding& b) {
    std::string out;
    if (b.value < 0.0f) {
        out.push_back('-');
    }
    using nf::runtime::BindingSource;
    switch (b.source) {
    case BindingSource::Key: out += "key:" + std::string(nf::runtime::key_name_from_code(b.code)); break;
    case BindingSource::PadButton:
        out += "pad:" + std::string(nf::runtime::pad_button_name_from_code(b.code));
        break;
    case BindingSource::PadAxis:
        out += "axis:" + std::string(nf::runtime::pad_axis_name_from_code(b.code));
        break;
    case BindingSource::MouseButton:
        out += "mouse:" + std::string(nf::runtime::mouse_button_name_from_code(b.code));
        break;
    }
    return out;
}

} // namespace

void input_panel(EditorApp& app) {
    // Captured by reference below; one capture at a time, which is what a
    // "press a key" affordance means.
    static std::string s_capture_action;

    if (!app.input_bindings_loaded()) {
        std::string ignored;
        app.load_input_bindings(ignored); // no file is the normal case
    }

    ImGui::TextDisabled("%s: %s%s", AV("settings_file").c_str(),
                        EditorApp::kInputBindingsPath,
                        app.input_bindings_file_exists() ? "" : " (none)");
    hint(AV("bindings_hint"));

    // While a capture is armed, the next key or mouse click becomes the
    // binding. Escape cancels — the one key that must always be able to say
    // "not this".
    if (!s_capture_action.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_capture_action.clear();
        } else {
            nf::u16 code = 0;
            nf::runtime::BindingSource source = nf::runtime::BindingSource::Key;
            bool captured = false;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                code = 0x01;
                source = nf::runtime::BindingSource::MouseButton;
                captured = true;
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                code = 0x02;
                source = nf::runtime::BindingSource::MouseButton;
                captured = true;
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
                code = 0x04;
                source = nf::runtime::BindingSource::MouseButton;
                captured = true;
            } else {
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !captured; ++k) {
                    const auto key = static_cast<ImGuiKey>(k);
                    if (ImGui::IsKeyPressed(key, false) && imgui_key_to_vk(key, code)) {
                        captured = true;
                    }
                }
            }
            if (captured) {
                app.input_bindings().set(s_capture_action,
                                         {nf::runtime::InputBinding{source, code, 1.0f}});
                s_capture_action.clear();
            }
        }
    }

    if (ImGui::BeginChild("##bindings", ImVec2(0.0f, 320.0f), ImGuiChildFlags_Borders)) {
        for (const auto& [action, list] : nf::runtime::binding_rows(app.input_bindings())) {
            ImGui::PushID(action.c_str());
            std::string shown;
            for (size_t i = 0; i < list.size(); ++i) {
                shown += (i == 0) ? "" : ", ";
                shown += binding_label(list[i]);
            }
            if (shown.empty()) {
                shown = "—";
            }
            ImGui::TextUnformatted(action.c_str());
            ImGui::SameLine(170.0f);
            ImGui::TextDisabled("%s", shown.c_str());
            ImGui::SameLine(330.0f);
            if (s_capture_action == action) {
                ImGui::TextColored(theme::accent(), "%s", AV("press_a_key").c_str());
            } else if (ImGui::SmallButton((AV("rebind") + "##rb").c_str())) {
                s_capture_action = action;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton((AV("reset") + "##rs").c_str())) {
                app.reset_binding(action);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // One control, two actions: whichever fires first is the one the player
    // gets, and nothing in the runtime can explain that. The list is capped
    // because the point is to make the player look, not to enumerate.
    const auto conflicts = nf::runtime::find_binding_conflicts(app.input_bindings());
    if (!conflicts.empty()) {
        ImGui::TextColored(theme::warning(), "%s: %d", AV("binding_conflicts").c_str(),
                           static_cast<int>(conflicts.size()));
        for (size_t i = 0; i < conflicts.size() && i < 4; ++i) {
            const nf::runtime::InputBinding as_binding{conflicts[i].source, conflicts[i].code,
                                                       1.0f};
            ImGui::BulletText("%s / %s: %s", conflicts[i].action_a.c_str(),
                              conflicts[i].action_b.c_str(), binding_label(as_binding).c_str());
        }
        if (conflicts.size() > 4) {
            ImGui::BulletText("…");
        }
    }

    if (ImGui::Button((AV("save") + "##bind_save").c_str())) {
        std::string err;
        if (!app.save_input_bindings(err)) {
            push_error(app.console(), "Save input bindings failed", err);
        } else {
            push_info(app.console(), "Input bindings saved: " + std::string(EditorApp::kInputBindingsPath));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button((AV("reload") + "##bind_reload").c_str())) {
        std::string err;
        if (!app.load_input_bindings(err)) {
            push_error(app.console(), "Reload input bindings failed", err);
        }
    }
}

// --- Render & performance ---------------------------------------------------

void render_panel(EditorApp& app, const UiFrameStats& stats) {
    EditorUiSettings& st = ui_settings();

    if (section(AV("performance").c_str())) {
        const double frame_ms = static_cast<double>(stats.dt_seconds) * 1000.0;
        kv(AV("fps"), fmt("%.1f", app.status().fps));
        // Frame time next to FPS: the average hides a stutter, the instantaneous
        // number is what the user actually feels.
        kv(AV("frame_time"), fmt("%.2f ms", frame_ms));
        if (stats.frame_samples > 0) {
            // Phase 41 window: stable numbers over the last ~two seconds. The
            // instantaneous rows above still answer "what is happening now";
            // these answer "what did the session feel like".
            kv(AV("fps_avg"), fmt("%.1f", stats.fps_average));
            kv(AV("fps_1pct_low"), fmt("%.1f", stats.fps_1pct_low));
            kv(AV("frame_jitter"), fmt("%.2f ms", stats.frame_stddev_ms));
            const char* bound_key = "bound_unknown";
            switch (stats.frame_bound) {
            case rendering::FramePacingBound::Cpu:
                bound_key = "bound_cpu";
                break;
            case rendering::FramePacingBound::Gpu:
                bound_key = "bound_gpu";
                break;
            case rendering::FramePacingBound::Balanced:
                bound_key = "bound_balanced";
                break;
            case rendering::FramePacingBound::Unknown:
                break;
            }
            kv(AV("frame_bound"), AV(bound_key));
            hint(AV("frame_pacing_hint"));
        }
        const bool gpu_real = stats.gpu_timing_valid;
        kv(AV(gpu_real ? "gpu_timestamp" : "gpu_time"),
           fmt("%.2f ms", gpu_real ? stats.gpu_frame_ms
                                   : static_cast<double>(stats.gpu_us) / 1000.0));
        kv(AV("gpu_avg"), fmt("%.2f ms", gpu_real ? stats.gpu_frame_ms_avg
                                                 : static_cast<double>(stats.gpu_avg_us) / 1000.0));
        if (gpu_real) {
            kv(AV("gpu_passes"), std::to_string(stats.gpu_passes.size()));
            for (const UiGpuPassTiming& pass : stats.gpu_passes) {
                // Pass names are the developer-facing identifiers from
                // RenderGraph, not UI copy: leave them unshaped and LTR.
                kv("    " + pass.name, fmt("%.3f ms", pass.milliseconds));
            }
            hint(AV("gpu_timing_note"));
        } else if (!stats.gpu_timing_supported) {
            hint(AV("gpu_timing_unavailable"));
        }
        kv(AV("cull_time"), fmt("%.1f us", stats.cull_us));
        kv(AV("draw_prep"), fmt("%.1f us", stats.draw_prep_us));
        kv(AV("draw_calls"), std::to_string(stats.draw_calls));
        kv(AV("visible_objects"), std::to_string(stats.visible_objects));
    }

    if (section(AV("statistics").c_str())) {
        kv(AV("entities"), std::to_string(app.status().entity_count));
        kv(AV("selected_count"), std::to_string(app.status().selected_count));
        kv(AV("assets_cached"), std::to_string(stats.assets_cached));
        kv(AV("scene_load"), fmt("%.1f ms", static_cast<double>(stats.scene_open_us) / 1000.0));
        kv(AV("rhi_objects"), std::to_string(stats.alive_objects));
        if (stats.validation_on) {
            const std::string v = std::to_string(stats.validation_errors);
            uiw::kv_colored(AV("validation").c_str(), v,
                            stats.validation_errors == 0 ? theme::success() : theme::danger());
        }
    }

    if (section(AV("viewport").c_str())) {
        ImGui::Checkbox((AV("grid_step").c_str()), &st.show_grid);
        ImGui::Checkbox((AV("show_navmesh") + "##rsnav").c_str(), &st.show_navmesh);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat((AV("grid_step_value") + "###rgrid").c_str(), &st.grid_step, 0.1f, 0.1f,
                         10.0f, "%.2f");
        ImGui::Checkbox((AV("fps") + "##rsfps").c_str(), &st.show_fps);
        ImGui::Checkbox((AV("validation") + "##rsval").c_str(), &st.show_validation);
        ImGui::Checkbox((AV("profiler") + "##rsprof").c_str(), &st.show_profiler);
    }

    if (section(AV("snap").c_str(), false)) {
        if (ImGui::Checkbox((AV("snap") + "##rsnap").c_str(), &st.snap_enabled)) {
            app.gizmo_snap() = GizmoSnap{st.snap_enabled ? st.snap_move : 0.0f,
                                         st.snap_enabled ? st.snap_rotate : 0.0f,
                                         st.snap_enabled ? st.snap_scale : 0.0f};
        }
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::DragFloat((AV("move") + "##rsm").c_str(), &st.snap_move, 0.05f, 0.0f, 10.0f,
                             "%.2f") && st.snap_enabled) {
            app.gizmo_snap().translate_step = st.snap_move;
        }
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::DragFloat((AV("rotate") + "##rsr").c_str(), &st.snap_rotate, 1.0f, 0.0f, 90.0f,
                             "%.0f") && st.snap_enabled) {
            app.gizmo_snap().rotate_step_deg = st.snap_rotate;
        }
    }

    ImGui::Spacing();
    if (primary_button(AV("save_trace"))) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path path = nf::FileSystem::scratch_directory() / "nf_profile_trace.json";
        std::string err;
        if (!ec && app.profiler_session().save_chrome_trace(path.string(), err)) {
            push_info(app.console(), std::string("Trace saved to ") + path.string() + " (" +
                                         std::to_string(app.profiler_session().event_count()) +
                                         " events)");
        } else {
            push_error(app.console(), "Trace save failed", err.empty() ? "temp dir?" : err);
        }
    }
}

// --- World ------------------------------------------------------------------

void world_panel(EditorApp& app) {
    ecs::World* w = app.world();
    const EditorStatus st = app.status();

    if (section(AV("panel_world").c_str())) {
        kv(AV("scene"), ui::shape_arabic(st.scene_label));
        uiw::hint_line(app.scene_path());
        kv(AV("status"),
           st.dirty ? ui::shape_arabic(ui::tr("status_unsaved")) : ui::shape_arabic(ui::tr("status_saved")));
    }

    if (w != nullptr && section(AV("census").c_str())) {
        const auto n = [&](auto tag) { return w->query<decltype(tag)>().size(); };
        kv(AV("entities"), std::to_string(st.entity_count));
        kv(AV("meshes"), std::to_string(n(runtime::MeshComponent{})));
        kv(AV("lights"), std::to_string(n(runtime::DirectionalLight{})));
        kv(AV("cameras"), std::to_string(n(runtime::CameraComponent{})));
        kv(AV("skies"), std::to_string(n(runtime::SkyComponent{})));
        kv(AV("rigid_bodies"), std::to_string(n(physics::RigidBodyComponent{})));
        kv(AV("colliders"), std::to_string(n(physics::ColliderComponent{})));
        kv(AV("destructibles"), std::to_string(n(runtime::DestructibleComponent{})));
        kv(AV("animations"), std::to_string(n(animation::AnimationComponent{})));
        kv(AV("audio"), std::to_string(n(audio::AudioComponent{})));
        kv(AV("particles"), std::to_string(n(vfx::ParticleComponent{})));
        kv(AV("cloths"), std::to_string(n(physics::ClothComponent{})));
        kv(AV("characters"), std::to_string(n(physics::CharacterComponent{})));
        kv(AV("navmesh"), std::to_string(n(runtime::NavMeshComponent{})));
        kv(AV("nav_agents"), std::to_string(n(runtime::NavAgentComponent{})));
    }

    if (const runtime::Runtime* rt = app.runtime(); rt != nullptr && section(AV("live_systems").c_str())) {
        kv(AV("scripts"), fmt("%d / %d", static_cast<double>(rt->scripts_runnable_count()),
                              static_cast<double>(rt->script_count())));
        kv(AV("particles_alive"),
           fmt("%d / %d", static_cast<double>(rt->particles_alive()),
               static_cast<double>(rt->particle_emitter_count())));
        kv(AV("cloths"), std::to_string(rt->cloth_count()));
        kv(AV("characters"), std::to_string(rt->character_count()));
        if (const ai::AIWorld* ai = rt->ai_world()) {
            kv(AV("ai_actors"), std::to_string(ai->actors().size()));
        }
        // Navigation reads the bake, not just the components: "40 polygons on
        // screen, 3 of 4 agents there" is the state a level author needs, and a
        // component count cannot tell an unbaked volume from a working one.
        kv(AV("navmesh_polys"), std::to_string(rt->navmesh_polygon_count()));
        kv(AV("nav_agents_arrived"),
           fmt("%d / %d", static_cast<double>(rt->nav_agents_at_goal()),
               static_cast<double>(rt->nav_agent_count())));
    }

    if (section(AV("selection").c_str())) {
        if (!app.selection().has_selection()) {
            hint(AV("nothing_selected"));
        } else {
            const size_t n_sel = app.selection().all().size();
            kv(AV("selected_count"), std::to_string(n_sel));
            if (w != nullptr) {
                const ecs::Entity primary = app.selection().primary();
                kv(AV("primary"), entity_name(*w, primary));
                // Component chips: the panel answers "what IS this" at a glance.
                std::vector<std::string> chips;
                if (w->has<runtime::CameraComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("camera")));
                if (w->has<runtime::DirectionalLight>(primary)) chips.push_back(ui::shape_arabic(ui::tr("directional_light")));
                if (w->has<runtime::SkyComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("sky")));
                if (w->has<runtime::MeshComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("mesh")));
                if (w->has<physics::RigidBodyComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("rigid_body")));
                if (w->has<physics::ColliderComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("collider")));
                if (w->has<runtime::DestructibleComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("destructible")));
                if (w->has<animation::AnimationComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("animation")));
                if (w->has<audio::AudioComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("audio")));
                if (w->has<scene::PrefabLinkComponent>(primary)) chips.push_back(ui::shape_arabic(ui::tr("prefab")));
                if (!chips.empty()) {
                    ImGui::Spacing();
                    uiw::chip_row(chips.data(), static_cast<int>(chips.size()));
                }
            }
        }
    }
}

} // namespace

void talent_tree_panel(EditorApp& app);
void flow_field_panel(EditorApp& app);

void scene_panels(EditorApp& app, const UiFrameStats& stats) {
    EditorUiSettings& st = ui_settings();
    // No window flags on purpose: DockBuilderDockWindow resolves a window that
    // does not exist yet through its ImGuiWindowSettings entry, and
    // ImGuiWindowFlags_NoSavedSettings is exactly what stops a window from
    // taking its dock id from there — the panel came up floating over the
    // outliner. The docked panels in Panels.cpp pass no flags either.

    if (st.show_lighting) {
        ImGui::SetNextWindowSize(ImVec2(340.0f, 460.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("tab_lighting") + "###NFLighting").c_str(), &st.show_lighting)) {
            lighting_panel(app);
        }
        ImGui::End();
    }
    if (st.show_environment) {
        ImGui::SetNextWindowSize(ImVec2(340.0f, 460.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("tab_environment") + "###NFEnvironment").c_str(), &st.show_environment)) {
            environment_panel(app);
        }
        ImGui::End();
    }
    if (st.show_camera_panel) {
        ImGui::SetNextWindowSize(ImVec2(340.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("tab_camera") + "###NFCameraPanel").c_str(), &st.show_camera_panel)) {
            camera_panel(app);
        }
        ImGui::End();
    }
    if (st.show_render) {
        ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("tab_render") + "###NFRender").c_str(), &st.show_render)) {
            render_panel(app, stats);
        }
        ImGui::End();
    }
    if (st.show_input) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 460.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("input_bindings") + "###NFInput").c_str(), &st.show_input)) {
            input_panel(app);
        }
        ImGui::End();
    }
    if (st.show_world) {
        ImGui::SetNextWindowSize(ImVec2(340.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("tab_world") + "###NFWorld").c_str(), &st.show_world)) {
            world_panel(app);
        }
        ImGui::End();
    }
    if (st.show_talent_tree) {
        ImGui::SetNextWindowSize(ImVec2(380.0f, 480.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("tab_talent_tree") + "###NFTalentTree").c_str(), &st.show_talent_tree)) {
            talent_tree_panel(app);
        }
        ImGui::End();
    }
    if (st.show_flow_field) {
        ImGui::SetNextWindowSize(ImVec2(400.0f, 480.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("tab_flow_field") + "###NFFlowField").c_str(), &st.show_flow_field)) {
            flow_field_panel(app);
        }
        ImGui::End();
    }
}

void talent_tree_panel(EditorApp& app) {
    (void)app;
    uiw::bound_header(AV("tab_talent_tree"), "Progression Graph");
    ImGui::Text("%s", AV("talent_tree").c_str());
    ImGui::Separator();

    static int sample_points = 5;
    static int spent_points = 0;
    ImGui::Text("%s: %d", AV("available_points").c_str(), sample_points);
    ImGui::SameLine();
    ImGui::Text("| %s: %d", AV("spent_points").c_str(), spent_points);

    const char* tiers[3] = {"Tier 0: Fundamentals", "Tier 1: Specialization", "Tier 2: Keystone Perks"};
    struct DemoTalent { const char* name; int rank; int max_rank; const char* type; };
    static DemoTalent demo_talents[3][2] = {
        { {"Iron Will", 1, 3, "PassiveStat"}, {"Eagle Eye", 0, 3, "PassiveStat"} },
        { {"Whirlwind Strike", 0, 1, "ActiveAbility"}, {"Shadow Step", 0, 1, "ActiveAbility"} },
        { {"Avatar of War", 0, 1, "KeystonePerk"}, {"Immortal Spirit", 0, 1, "KeystonePerk"} }
    };

    for (int t = 0; t < 3; ++t) {
        if (section(tiers[t])) {
            for (int i = 0; i < 2; ++i) {
                ImGui::PushID(t * 10 + i);
                ImGui::BulletText("%s [%s]", demo_talents[t][i].name, demo_talents[t][i].type);
                ImGui::SameLine();
                ImGui::Text("(Rank %d/%d)", demo_talents[t][i].rank, demo_talents[t][i].max_rank);
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    if (sample_points > 0 && demo_talents[t][i].rank < demo_talents[t][i].max_rank) {
                        demo_talents[t][i].rank++;
                        sample_points--;
                        spent_points++;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("-")) {
                    if (demo_talents[t][i].rank > 0) {
                        demo_talents[t][i].rank--;
                        sample_points++;
                        spent_points--;
                    }
                }
                ImGui::PopID();
            }
        }
    }
}

void flow_field_panel(EditorApp& app) {
    uiw::bound_header(AV("tab_flow_field"), "AI Crowd Navigation");
    ImGui::Text("%s", AV("flow_field").c_str());
    ImGui::Separator();

    static int grid_sz = 32;
    static float csize = 1.0f;
    static int goal_x = 16;
    static int goal_y = 16;
    ImGui::SliderInt((AV("grid_width") + "##ffp_w").c_str(), &grid_sz, 8, 128);
    ImGui::DragFloat((AV("cell_size") + "##ffp_cs").c_str(), &csize, 0.1f, 0.2f, 10.0f, "%.2f m");
    ImGui::SliderInt((AV("target_x") + "##ffp_gx").c_str(), &goal_x, 0, grid_sz - 1);
    ImGui::SliderInt((AV("target_y") + "##ffp_gy").c_str(), &goal_y, 0, grid_sz - 1);

    if (ImGui::Button(AV("recalculate_flow").c_str())) {
        push_info(app.console(), "Wavefront integration complete for 10,000+ agents towards (" + std::to_string(goal_x) + ", " + std::to_string(goal_y) + ")");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Grid Flow Vectors Preview (Goal at [%d, %d]):", goal_x / 4, goal_y / 4);
    const char* arrow_symbols[8] = { "^", "/", ">", "\\", "v", "/", "<", "\\" };
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            if (x > 0) ImGui::SameLine();
            if (x == goal_x / 4 && y == goal_y / 4) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "[*]");
            } else {
                int dx = (goal_x / 4) - x;
                int dy = (goal_y / 4) - y;
                float angle = std::atan2(static_cast<float>(dy), static_cast<float>(dx));
                int dir_idx = static_cast<int>(std::round(angle / (3.14159265f / 4.0f))) & 7;
                ImGui::TextDisabled("[%s]", arrow_symbols[dir_idx]);
            }
        }
    }
}

} // namespace nf::editor
