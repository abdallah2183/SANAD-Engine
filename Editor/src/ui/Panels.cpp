// Panels.cpp — ImGui panel recording: toolbar, outliner, viewport, inspector,
// assets, console. All scene mutations go through EditorApp (commands +
// validation); widgets only collect input and forward intents.

#define _CRT_SECURE_NO_WARNINGS
#include <NF/Editor/UiShell.hpp>
#include <NF/Editor/ToolbarUi.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Editor/AudioPreview.hpp>
#include <NF/Editor/ReflectedInspector.hpp>
#include <NF/Editor/UiRenderer.hpp>
#include <NF/Core/Profiler.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>
#include <NF/Editor/UiText.hpp> // AV()/AVF()/rt_* — the single display-string path

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: default layout only (first frame)
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <array>
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
    float sky_zenith[3]{0.20f, 0.42f, 0.85f};
    float sky_horizon[3]{0.62f, 0.72f, 0.82f};
    float sky_ground[3]{0.09f, 0.09f, 0.11f};
    float sky_sun_disk = 1.0f;
    float sky_sun_glow = 1.0f;
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
    const float ndc_x = clip.x / clip.w;
    const float ndc_y = clip.y / clip.w;
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    out.x = min.x + (ndc_x * 0.5f + 0.5f) * w;
    out.y = min.y + (0.5f - ndc_y * 0.5f) * h;
    return true;
}

/// Draws the y = 0 grid, `half_extent` metres either side, one line every
/// `spacing`. Axis lines (x = 0 and z = 0) are drawn in their axis colours so
/// the origin is unambiguous.
constexpr int kGridHalfExtent = 50; // cells either side; extent = 50 * grid_step
void draw_ground_grid(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const Mat4& view_proj,
                      float spacing, int half_extent) {
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
    for (int i = -half_extent; i <= half_extent; ++i) {
        const float o = static_cast<float>(i) * spacing;
        // Beyond 20 m only every other line survives; see grid_line_visible.
        if (!grid_line_visible(i, o)) {
            continue;
        }
        // Lines parallel to Z (varying x) and parallel to X (varying z).
        const float segs[2][4] = {
            {o, 0.0f, -extent, extent},  // x = o, z from -extent to extent
            {-extent, 0.0f, o, extent},  // z = o, x from -extent to extent
        };
        for (int s = 0; s < 2; ++s) {
            ImVec2 a{}, b{};
            const bool ax = (s == 0);
            const bool ok_a = project_to_viewport(view_proj, ax ? segs[s][0] : segs[s][1], 0.0f,
                                                  ax ? segs[s][2] : segs[s][3], min, max, a);
            const bool ok_b = project_to_viewport(view_proj, ax ? segs[s][0] : segs[s][1], 0.0f,
                                                  ax ? segs[s][3] : segs[s][2], min, max, b);
            if (!ok_a || !ok_b) {
                continue; // a segment with an endpoint behind the camera is skipped whole
            }
            const ImU32 col = (i == 0) ? (ax ? axis_z : axis_x) : line_col;
            dl->AddLine(a, b, col, (i == 0) ? 1.4f : 1.0f);
        }
    }
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

UiIntents ui_frame(EditorApp& app, const UiFrameStats& stats) {
    UiIntents intents;

    // Autosave runs off the frame clock, before any panel draws. Ticking it here
    // rather than from Runtime::update keeps the Runtime unaware of save slots.
    app.tick_autosave(stats.dt_seconds);

    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Full-window dockspace with a Unity-style default layout, built once:
    // Outliner left, Viewport center, Inspector right, Assets + Console
    // tabbed at the bottom. The layout is rebuilt every launch (IniFilename
    // is null, so there is nothing to load it from) but never again within
    // a session, so the user can still drag panels around freely.
    {
        const ImGuiID dockspace_id = ImGui::GetID("NOVAForgeDockSpace");
        ImGui::DockSpaceOverViewport(dockspace_id, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
        static bool dock_layout_built = false;
        if (!dock_layout_built) {
            dock_layout_built = true;
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

            ImGuiID dock_left = 0, dock_right = 0, dock_bottom = 0, dock_center = 0;
            ImGuiID dock_rest = dockspace_id;
            ImGui::DockBuilderSplitNode(dock_rest, ImGuiDir_Left, 0.18f, &dock_left, &dock_rest);
            ImGui::DockBuilderSplitNode(dock_rest, ImGuiDir_Right, 0.24f, &dock_right, &dock_rest);
            ImGui::DockBuilderSplitNode(dock_rest, ImGuiDir_Down, 0.30f, &dock_bottom, &dock_center);

            ImGui::DockBuilderDockWindow("Outliner", dock_left);
            ImGui::DockBuilderDockWindow("Viewport", dock_center);
            ImGui::DockBuilderDockWindow("Inspector", dock_right);
            ImGui::DockBuilderDockWindow("Assets", dock_bottom);
            ImGui::DockBuilderDockWindow("Console", dock_bottom);
            ImGui::DockBuilderFinish(dockspace_id);
        }
    }

    const EditorStatus st = app.status();

    // --- Top bar: menus + toolbar + dialogs (NF/Editor/ToolbarUi.hpp) --------
    // Moved out of this file so the panels stay "the docked panels". The menu
    // bar and the icon row are recorded first, so their window ordering keeps
    // the toolbar above the dockspace on the frame it is created.
    toolbar_ui(app, stats, intents);

    // --- Left: Scene Outliner --------------------------------------------------
    if (ImGui::Begin((AV("outliner") + "###Outliner").c_str())) {
        if (ImGui::Button(AV("create_empty").c_str())) {
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
        if (!has_sel) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!has_sel) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(AV("save_prefab").c_str())) {
            const ecs::Entity sel = app.selection().primary();
            std::string err;
            const std::string prefab_path =
                std::string("content://Prefabs/") + entity_label_ptr(app, sel) + ".nfscene";
            if (!app.create_prefab(sel, prefab_path, err)) {
                push_error(app.console(), "Save as prefab failed", err);
            }
        }
        if (!has_sel) {
            ImGui::EndDisabled();
        }
        const std::vector<OutlinerRow> rows = app.outliner_rows();
        if (rows.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", AV("empty_scene_hint").c_str());
        }
        int row_idx = 0;
        for (const OutlinerRow& row : rows) {
            ImGui::PushID(row_idx++);
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
            const std::string row_text =
                ui::shape_arabic(row.is_prefab ? ("[Prefab] " + row.label) : row.label);
            const std::string row_id = "###e" + std::to_string(row.entity.id);
            const bool open = ImGui::TreeNodeEx((row_text + row_id).c_str(), flags);
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
            if (open && row.has_children) {
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
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
    }
    ImGui::End();

    // --- Center: Viewport ------------------------------------------------------
    if (ImGui::Begin((AV("viewport") + "###Viewport").c_str())) {
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
        // Mode readout: skipped when the panel is too narrow to hold it (it
        // used to bleed past the panel edge and paint clipped garbage).
        if (ImGui::GetContentRegionAvail().x > 420.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s, %s, %s)",
                                (cur_mode == GizmoMode::Translate
                                     ? AV("move")
                                     : (cur_mode == GizmoMode::Rotate ? AV("rotate")
                                                                      : AV("scale_tool")))
                                    .c_str(),
                                space_label.c_str(), AV("shift_click_multi").c_str());
        }
        if (stats.viewport_lit != 0) {
            ImGui::SameLine();
            // Lit-pixel counter: a renderer diagnostic, English on purpose
            // (same decision as the profiler block below).
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
        // Ground grid over the image (drawn, not composited): world grid lines
        // projected through the camera the frame was rendered with, so the grid
        // sits in the scene rather than on the screen. Without it the viewport is
        // a gradient with no sense of scale or where the origin is.
        if (ui_settings().show_grid) {
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            rendering::Camera cam{};
            if (app.runtime() != nullptr &&
                app.runtime()->extract_camera(static_cast<uint32_t>(std::max(1.0f, img_size.x)),
                                              static_cast<uint32_t>(std::max(1.0f, img_size.y)), cam)) {
                // ImGui draws AFTER the scene image, so the grid paints over the
                // geometry where the two overlap. That is the standard editor
                // compromise (the alternative is a depth-aware overlay pass, which
                // needs the depth buffer in this draw list) and it is what keeps
                // the grid usable as a reference while orbiting.
                draw_ground_grid(ImGui::GetWindowDrawList(), mn, mx, cam.view_projection,
                                 ui_settings().grid_step, kGridHalfExtent);
            }
        }
        // Pointer gesture state machine (one viewport, so statics are fine):
        // press on the image arms a gizmo drag in the app, movement feeds it,
        // release folds one undoable command. NDC is recomputed from the live
        // mouse position every frame (the panel can resize mid-gesture).
        {
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
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
            if (!press_armed && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (w > 1.0f && h > 1.0f) {
                    press_armed = true;
                    intents.viewport_press = true;
                    // Ctrl/Shift at press time toggles the hit entity into the
                    // selection (group drag) instead of replacing it.
                    const ImGuiIO& press_io = ImGui::GetIO();
                    intents.viewport_press_additive = press_io.KeyShift || press_io.KeyCtrl;
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
            {
                const ImGuiIO& nav_io = ImGui::GetIO();
                const bool over_view = ImGui::IsItemHovered();
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
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NF_MESH")) {
                intents.viewport_drop = true;
                intents.dropped_mesh_path.assign(static_cast<const char*>(pl->Data), pl->DataSize);
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
        ecs::World* w = app.world();
        const ecs::Entity sel = app.selection().primary();
        if (w == nullptr || !sel.valid() || !w->is_alive(sel)) {
            ImGui::TextDisabled("%s", AV("nothing_selected").c_str());
        } else {
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
            }
            if (!ic.error.empty()) {
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s: %s", AV("error").c_str(),
                                   ic.error.c_str());
            }
            if (ImGui::CollapsingHeader((AV("name") + "###Name").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
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
                ImGui::DragFloat3(AV("position").c_str(), ic.pos, 0.05f);
                ImGui::DragFloat3(AV("rotation").c_str(), ic.rot, 0.5f);
                ImGui::DragFloat3(AV("scale").c_str(), ic.scl, 0.02f, 0.01f, 1000.0f);
                if (const auto* t = w->get<scene::Transform>(sel)) {
                    // A parent name is user-authored text (may be Arabic):
                    // compose LOGICAL then shape once (concatenating two
                    // shaped strings would freeze LTR order). TextUnformatted
                    // keeps a shaped '%' in the name from being read as a
                    // format specifier.
                    rt_text_str(ui::shape_arabic(
                        ui::tr("parent_prefix") + ": " +
                        std::string(t->parent.valid() ? entity_label_ptr(app, t->parent)
                                                      : ui::tr("inspector_root"))));
                }
                ImGui::SameLine();
                if (ImGui::Button((AV("apply") + "##transform").c_str())) {
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
                    ImGui::Combo(AV("material").c_str(), &ic.mat_choice, mat_combo_item, &mat_list,
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
                    std::string state = "state=unknown";
                    auto handle = app.asset_manager().find(m->mesh_id);
                    if (!handle) {
                        state = "state=not requested";
                    } else {
                        switch (handle->state) {
                            case assets::AssetState::Unloaded: state = "state=unloaded"; break;
                            case assets::AssetState::Loading: state = "state=loading..."; break;
                            case assets::AssetState::Ready: state = "state=ready"; break;
                            case assets::AssetState::Failed: state = std::string("state=failed: ") + handle->error; break;
                        }
                    }
                    ImGui::TextUnformatted(state.c_str());
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
            if (w->has<runtime::DirectionalLight>(sel) &&
                ImGui::CollapsingHeader((AV("directional_light") + "###DirectionalLight").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3(AV("direction").c_str(), ic.light_dir, 0.02f);
                ImGui::ColorEdit3(AV("color").c_str(), ic.light_color);
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
                    ImGui::Checkbox(AV("enabled").c_str(), &ic.sky_enabled);
                    ImGui::ColorEdit3(AV("zenith").c_str(), ic.sky_zenith);
                    ImGui::ColorEdit3(AV("horizon").c_str(), ic.sky_horizon);
                    ImGui::ColorEdit3(AV("ground").c_str(), ic.sky_ground);
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
            // --- Physics: RigidBody ---
            if (w->has<physics::RigidBodyComponent>(sel) &&
                ImGui::CollapsingHeader((AV("rigid_body") + "###RigidBody").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                // Owned AV strings (not temporaries): the pointers outlive Combo.
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
            // --- Physics: Collider ---
            if (w->has<physics::ColliderComponent>(sel) &&
                ImGui::CollapsingHeader((AV("collider") + "###Collider").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
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
            if (w->has<audio::AudioComponent>(sel) &&
                ImGui::CollapsingHeader((AV("audio") + "###Audio").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
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
                                    (void)draw_reflected_field(view, index);
                                }
                            }
                        }
                    }
                }
            }
            ImGui::Spacing();
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
    }
    ImGui::End();

    // --- Bottom: Assets + Console ------------------------------------------------
    if (ImGui::Begin((AV("assets") + "###Assets").c_str())) {
        char filter[128]{};
        std::strncpy(filter, app.browser().filter_text.c_str(), sizeof(filter) - 1);
        if (ImGui::InputText(AV("filter").c_str(), filter, sizeof(filter))) {
            app.browser().filter_text = filter;
        }
        // Type filter mirrors the model's own -1 = "all types" contract; the
        // combo order is the same enum order filter_assets switches on.
        // Owned AV strings (not temporaries): the pointers outlive Combo.
        ImGui::SameLine();
        const std::string type_items[6] = {AV("asset_type_all"), AV("asset_type_mesh"),
                                           AV("asset_type_texture"), AV("asset_type_material"),
                                           AV("asset_type_shader"), AV("asset_type_scene")};
        const char* type_names[] = {type_items[0].c_str(), type_items[1].c_str(),
                                    type_items[2].c_str(), type_items[3].c_str(),
                                    type_items[4].c_str(), type_items[5].c_str()};
        const int type_values[] = {-1, static_cast<int>(assets::AssetType::Mesh),
                                  static_cast<int>(assets::AssetType::Texture),
                                  static_cast<int>(assets::AssetType::Material),
                                  static_cast<int>(assets::AssetType::Shader),
                                  static_cast<int>(assets::AssetType::Scene)};
        int combo_idx = 0; // All
        for (size_t i = 1; i < std::size(type_values); ++i) {
            if (app.browser().filter_type == type_values[i]) {
                combo_idx = static_cast<int>(i);
                break;
            }
        }
        if (ImGui::Combo(AV("type").c_str(), &combo_idx, type_names, static_cast<int>(std::size(type_names)))) {
            app.browser().filter_type = type_values[combo_idx];
        }
        // External import (no OS dialog in v0.1: absolute source path input).
        // Types by extension: .nfmesh/.png/.jpg/.bmp/.tga/.nfmat/.nfscene.
        static char import_src[256]{};
        static char import_dir[128] = "content://Meshes";
        static bool import_overwrite = false;
        ImGui::InputText(AV("import_source").c_str(), import_src, sizeof(import_src));
        ImGui::SameLine();
        ImGui::InputText(AV("import_to").c_str(), import_dir, sizeof(import_dir));
        ImGui::SameLine();
        ImGui::Checkbox(AV("overwrite").c_str(), &import_overwrite);
        ImGui::SameLine();
        if (ImGui::Button(AV("import").c_str())) {
            size_t job = 0;
            std::string err;
            if (!app.import_file(import_src, import_dir, import_overwrite, job, err)) {
                push_error(app.console(), "Import rejected", err);
            } else {
                app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor,
                                              "Import queued (#" + std::to_string(job) + ")",
                                              std::chrono::system_clock::now(), __FILE__, __LINE__});
            }
        }
        // Queue states (processed incrementally by the shell, one job/frame).
        // Technical diagnostics (job ids, absolute paths, states): English on
        // purpose, like the console log lines — they mirror engine counters,
        // not UI wording. (The import DIALOG's states translate via
        // import_state_*; these inline rows are the debug tail.)
        for (const ImportJob& job : app.import_queue().jobs()) {
            const char* job_state = "?";
            switch (job.state) {
                case ImportJob::State::Queued: job_state = "queued"; break;
                case ImportJob::State::Working: job_state = "working"; break;
                case ImportJob::State::Done: job_state = "done"; break;
                case ImportJob::State::Failed: job_state = "FAILED"; break;
            }
            ImGui::Text("[import #%zu] %s -> %s : %s", job.id, job.src_absolute.c_str(),
                        job.dst_logical.c_str(), job_state);
            if (job.state == ImportJob::State::Failed && !job.error.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", job.error.c_str());
            }
        }
        if (ImGui::Button(AV("clear_imports").c_str())) {
            app.import_queue().clear_finished();
        }
        const std::vector<AssetEntry> entries = app.browser_entries();
        rt_text_str(AVF("assets_count_fmt", entries.size()));
        if (ImGui::BeginChild("##assetlist", ImVec2(0, 0), true)) {
            int idx = 0;
            for (const AssetEntry& e : entries) {
                ImGui::PushID(idx++);
                const char* badge = "?";
                switch (e.type) {
                    case assets::AssetType::Mesh: badge = "[MESH]"; break;
                    case assets::AssetType::Scene: badge = "[SCENE]"; break;
                    case assets::AssetType::Shader: badge = "[SHADER]"; break;
                    case assets::AssetType::Texture: badge = "[TEX]"; break;
                    case assets::AssetType::Material: badge = "[MAT]"; break;
                    default: badge = "[?]"; break;
                }
                const bool selected = (app.browser().selected_path == e.logical_path);
                // Thumbnails for image assets: the shell's preview cache uploads
                // on first request and hands back an ImGui id. 0 means "no
                // preview" (headless/tests, or a not-yet-imported source) and
                // draws a placeholder — never a missing-texture draw.
                if (e.type == assets::AssetType::Texture && app.preview_texture) {
                    const uintptr_t pid = app.preview_texture(e.logical_path);
                    if (pid != 0) {
                        ImGui::Image(static_cast<ImTextureID>(pid), ImVec2(48, 48));
                        ImGui::SameLine();
                    } else {
                        ImGui::TextDisabled("%s", AV("asset_no_preview").c_str());
                        ImGui::SameLine();
                    }
                }
                if (ImGui::Selectable((std::string(badge) + " " + e.logical_path).c_str(), selected)) {
                    app.browser().selected_path = e.logical_path;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    std::string info;
                    const AssetOpenAction act = classify_double_click(e, info);
                    if (act == AssetOpenAction::OpenScene) {
                        intents.open_scene_dialog_confirm = true;
                        intents.open_scene_path = info;
                    } else if (act == AssetOpenAction::ShowMeshInfo) {
                        app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor, info,
                                                      std::chrono::system_clock::now(), __FILE__,
                                                      __LINE__});
                    } else if (act == AssetOpenAction::ShowTextureInfo) {
                        app.console().push(LogMessage{
                            LogLevel::Info, LogCategory::Editor,
                            "Texture " + info + " — assign it from an entity's Material section.",
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
                    } else if (act == AssetOpenAction::ShowMaterialInfo) {
                        std::string detail = info;
                        if (app.runtime() != nullptr) {
                            rendering::PBRMaterialParams mp{};
                            app.runtime()->material_params(info, mp);
                            char buf[160];
                            std::snprintf(buf, sizeof(buf), " base=(%.2f,%.2f,%.2f) metallic=%.2f "
                                                            "roughness=%.2f",
                                          static_cast<double>(mp.base_color[0]),
                                          static_cast<double>(mp.base_color[1]),
                                          static_cast<double>(mp.base_color[2]),
                                          static_cast<double>(mp.metallic),
                                          static_cast<double>(mp.roughness));
                            detail += buf;
                        }
                        app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor, detail,
                                                      std::chrono::system_clock::now(), __FILE__,
                                                      __LINE__});
                    }
                }
                if (e.type == assets::AssetType::Mesh && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("NF_MESH", e.logical_path.c_str(),
                                              e.logical_path.size() + 1);
                    ImGui::Text("%s", e.logical_path.c_str());
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
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
        ImGui::Combo(AV("level").c_str(), &level_filter, levels, 5);
        ImGui::SameLine();
        // Category filter: the bitmask the ConsoleBuffer query takes. The
        // names mirror LogCategory; index 0 is "All" so nothing is hidden by
        // default. The index is NOT the enum value — LogCategory::All is
        // 0xFFFF, and indexing the arrays with it would read far out of bounds.
        //
        // A3b decision: these stay ENGLISH on purpose. They are the same tags
        // that prefix every log line, so translating the filter would break the
        // correspondence between the chip you click and the text you are
        // filtering. The Level filter above IS localised, because Trace/Info/
        // Warn are severities a user reads rather than enum identifiers.
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
        ImGui::Combo(AV("category").c_str(), &category_filter, categories, 14);
        ImGui::SameLine();
        if (ImGui::Button(AV("clear").c_str())) {
            app.console().clear();
        }
        ImGui::SameLine();
        rt_disabled_str(AVF("console_dropped", app.console().dropped()));
        // Search: case-insensitive substring over the message text. Frame-
        // persistent like the two combos above; the panel owns the query state
        // and ConsoleBuffer stays a pure reader.
        static char search[128] = "";
        ImGui::PushItemWidth(220.0f);
        // ##id keeps the label out of the shaped-text path; the hint is owned
        // (not a temporary) so it outlives the widget call.
        const std::string search_hint = AV("console_search_hint");
        ImGui::InputTextWithHint("##consolesearch", search_hint.c_str(), search,
                                 IM_ARRAYSIZE(search));
        ImGui::PopItemWidth();
        const int cat_index =
            (category_filter >= 0 && category_filter < IM_ARRAYSIZE(category_values))
                ? category_filter
                : 0;
        const LogCategory selected = category_values[cat_index];
        const nf::editor::ConsoleBuffer::Filter filter{
            static_cast<LogLevel>(level_filter),
            (selected == LogCategory::All) ? LogCategory::All : selected,
            std::string(search)};
        const std::vector<LogMessage> lines = app.console().filtered(filter);
        if (ImGui::BeginChild("##consolelines", ImVec2(0, 0), true)) {
            for (const LogMessage& m : lines) {
                // Message text is arbitrary (asset paths, engine errors, an
                // Arabic entity name in a rename failure), so shape it for
                // display and avoid the %s format path entirely.
                const std::string line = "[" + std::string(level_name(m.level)) + "][" +
                                         std::string(category_name(m.category)) + "] " +
                                         ui::shape_arabic(m.text);
                ImGui::TextColored(level_color(m.level), "%s", line.c_str());
            }
            if (!lines.empty()) {
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
                const fs::path path = fs::temp_directory_path(ec) / "nf_profile_trace.json";
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

    return intents;
}

} // namespace nf::editor
