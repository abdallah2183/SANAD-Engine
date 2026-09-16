// Panels.cpp — ImGui panel recording: toolbar, outliner, viewport, inspector,
// assets, console. All scene mutations go through EditorApp (commands +
// validation); widgets only collect input and forward intents.

#define _CRT_SECURE_NO_WARNINGS
#include <NF/Editor/UiShell.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Editor/ReflectedInspector.hpp>
#include <NF/Editor/UiRenderer.hpp>
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

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: default layout only (first frame)
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
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

void push_info(ConsoleBuffer& console, const std::string& what) {
    console.push(LogMessage{LogLevel::Info, LogCategory::Editor, what,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

void push_error(ConsoleBuffer& console, const std::string& what, const std::string& err) {
    console.push(LogMessage{LogLevel::Error, LogCategory::Editor, what + ": " + err,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

// --- Localisation helpers (Phase 15) ----------------------------------------
// TR(key): translated string (logical UTF-8). AV(key): translated + shaped
// for display. Window/header labels append a stable "###id" so switching
// languages never resets docking or widget state (IDs stay English).
inline std::string TR(const char* key) {
    return ui::tr(key);
}
inline std::string AV(const char* key) {
    return ui::shape_arabic(ui::tr(key));
}

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

const char* entity_label_ptr(EditorApp& app, ecs::Entity e) {
    thread_local std::string scratch;
    if (const ecs::World* w = app.world()) {
        scratch = entity_label(*w, e);
    } else {
        scratch = "Entity ?";
    }
    return scratch.c_str();
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

    // --- Top toolbar ---------------------------------------------------------
    if (ImGui::BeginMainMenuBar()) {
            if (ImGui::Button(AV("new").c_str())) {
                std::string err;
                if (!app.new_scene(err)) {
                    push_error(app.console(), "New scene failed", err);
                }
            }
            ImGui::SameLine();
            static char open_path[256]{};
            if (ImGui::Button((AV("open") + "...").c_str())) {
                ImGui::OpenPopup("OpenScene");
            }
            if (ImGui::BeginPopupModal((AV("open") + "###OpenScene").c_str(), nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("content:// path", open_path, sizeof(open_path));
                if (ImGui::Button(AV("open").c_str())) {
                    intents.open_scene_dialog_confirm = true;
                    intents.open_scene_path = open_path;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(AV("cancel").c_str())) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(AV("save").c_str())) {
                std::string err;
                if (!app.save(err)) {
                    push_error(app.console(), "Save failed", err);
                }
            }
            ImGui::SameLine();
            static char saveas_path[256]{};
            if (ImGui::Button(AV("save_as").c_str())) {
                ImGui::OpenPopup("SaveSceneAs");
            }
            if (ImGui::BeginPopupModal((AV("save_as") + "###SaveSceneAs").c_str(), nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("content:// path", saveas_path, sizeof(saveas_path));
                if (ImGui::Button(AV("save").c_str())) {
                    intents.save_as_confirm = true;
                    intents.save_as_path = saveas_path;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(AV("cancel").c_str())) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (!app.playing() && ImGui::Button(AV("play").c_str())) {
                std::string err;
                if (!app.play(err)) {
                    push_error(app.console(), "Play failed", err);
                }
            }
            ImGui::SameLine();
            if (app.playing() && ImGui::Button(AV("stop").c_str())) {
                std::string err;
                if (!app.stop(err)) {
                    push_error(app.console(), "Stop failed", err);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button((AV("game") + "##save_menu").c_str())) {
                ImGui::OpenPopup("GameSaves");
            }
            if (ImGui::BeginPopup("GameSaves")) {
                static char slot[64] = "slot1";
                ImGui::InputText(AV("slot").c_str(), slot, sizeof(slot));

                if (ImGui::Button(AV("save_game").c_str())) {
                    std::string err;
                    if (!app.save_game(slot, err)) {
                        push_error(app.console(), "Save game failed", err);
                    } else {
                        push_info(app.console(), std::string("Saved game to '") + slot + "'");
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(AV("load_game").c_str())) {
                    std::string err;
                    if (!app.load_game(slot, err)) {
                        push_error(app.console(), "Load game failed", err);
                    } else {
                        push_info(app.console(), std::string("Loaded game from '") + slot + "'");
                    }
                }
                ImGui::SameLine();
                if (!app.has_save(slot)) {
                    ImGui::TextDisabled("%s", AV("no_such_slot").c_str());
                }

                // Autosave. The interval is a real duration rather than a frame
                // count, so the behaviour does not change with frame rate.
                static float interval = 120.0f;
                bool enabled = app.autosave_enabled();
                if (ImGui::Checkbox(AV("autosave").c_str(), &enabled)) {
                    app.set_autosave(enabled ? interval : 0.0f, "autosave_");
                }
                if (enabled) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::DragFloat(AV("interval_s").c_str(), &interval, 5.0f, 5.0f,
                                         3600.0f)) {
                        app.set_autosave(interval, "autosave_");
                    }
                    ImGui::TextDisabled("autosaves written: %u", app.autosaves_performed());
                }

                ImGui::Separator();
                const std::vector<runtime::SaveSystem::SlotInfo> slots = app.list_saves();
                if (slots.empty()) {
                    ImGui::TextDisabled("%s", AV("no_save_slots").c_str());
                }
                for (const runtime::SaveSystem::SlotInfo& info : slots) {
                    ImGui::Text("%s  (%s, schema %u, %s)", info.name.c_str(),
                                info.scene_name.c_str(), info.schema_version,
                                info.saved_at.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("|");
            ImGui::SameLine();
            // Project actions. Building only makes sense with a project open:
            // started straight into the engine tree there is nothing to package.
            if (app.has_project()) {
                ImGui::Text("Project: %s", app.project_name().c_str());
                ImGui::SameLine();
                if (ImGui::Button(AV("build").c_str())) {
                    intents.build_project = true;
                }
            } else {
                ImGui::TextDisabled("Project: (engine tree)");
            }
            ImGui::SameLine();
            static char newproj_dir[256]{};
            static char newproj_name[128]{};
            if (ImGui::Button((AV("new") + "...").c_str())) {
                ImGui::OpenPopup("NewProject");
            }
            if (ImGui::BeginPopupModal((AV("new") + "###NewProject").c_str(), nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("directory", newproj_dir, sizeof(newproj_dir));
                ImGui::InputText("name", newproj_name, sizeof(newproj_name));
                if (ImGui::Button(AV("create").c_str())) {
                    intents.new_project_confirm = true;
                    intents.new_project_dir = newproj_dir;
                    intents.new_project_name = newproj_name;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(AV("cancel").c_str())) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(stats.validation_on ? "[Validation ON]" : "[Validation OFF]");
            if (stats.validation_errors != 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "errors=%u",
                                   static_cast<unsigned>(stats.validation_errors));
            }
            ImGui::SameLine();
            ImGui::Text("%.1f FPS / %.2f ms", st.fps, st.frame_ms);
            ImGui::SameLine();
            ImGui::TextUnformatted(st.scene_label.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted("|");
            ImGui::SameLine();
            if (ImGui::Button(AV("about").c_str())) {
                ImGui::OpenPopup("AboutSANAD");
            }
            if (ImGui::BeginPopupModal((AV("about") + "###AboutSANAD").c_str(), nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.15f, 1.0f), "SANAD Engine (%s)",
                                   ui::shape_arabic("محرك سند").c_str());
                ImGui::Text("Founder & Project Lead: Abdallah (%s)",
                            ui::shape_arabic("عبدالله").c_str());
                ImGui::Text("The Modern Extensible Arabic C++23 & Vulkan Game Engine");
                ImGui::Separator();
                ImGui::Text("Phases 12-16: LOD + Shadows/Sky + Audio/glTF + Lua + Arabic UI");
                ImGui::Text("Validation: 0 errors | 636 automated tests passing");
                ImGui::Text("Open Source Community: We welcome all contributors to build SANAD together!");
                ImGui::Separator();
                if (ImGui::Button(AV("close").c_str(), ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            // Language toggle (Phase 15): English <-> Arabic, shaped + Amiri.
            if (ui::current_language() == ui::Language::Arabic) {
                if (ImGui::Button("English")) {
                    ui::set_language(ui::Language::English);
                }
            } else {
                if (ImGui::Button(ui::shape_arabic("العربية").c_str())) {
                    ui::set_language(ui::Language::Arabic);
                }
            }
        ImGui::EndMainMenuBar();
    }

    // --- Left: Scene Outliner --------------------------------------------------
    if (ImGui::Begin((AV("outliner") + "###Outliner").c_str())) {
        if (ImGui::Button("+ Empty")) {
            std::string err;
            const int n = static_cast<int>(app.status().entity_count);
            if (!app.create_entity("Entity " + std::to_string(n), ecs::kInvalidEntity, err)) {
                push_error(app.console(), "Create entity failed", err);
            }
        }
        ImGui::SameLine();
        const bool has_sel = app.selection().has_selection();
        if (!has_sel) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Delete")) {
            app.request_delete(app.selection().primary());
        }
        if (!has_sel) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!has_sel) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save as prefab")) {
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
        // Inline (non-modal) delete confirmation.
        if (app.outliner().pending_delete.valid()) {
            const ecs::Entity pd = app.outliner().pending_delete;
            ImGui::Text("Delete '%s'?", entity_label_ptr(app, pd));
            ImGui::SameLine();
            if (ImGui::Button("Confirm")) {
                std::string err;
                if (!app.confirm_delete(err)) {
                    push_error(app.console(), "Delete failed", err);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                app.cancel_delete();
            }
        }
        const std::vector<OutlinerRow> rows = app.outliner_rows();
        if (rows.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("Empty scene — press + Empty to create your first entity, "
                               "or drag a mesh from the Asset Browser into the viewport.");
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
            const std::string row_text =
                row.is_prefab ? ("[Prefab] " + row.label) : row.label;
            const bool open = ImGui::TreeNodeEx(row_text.c_str(), flags);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyCtrl) {
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
                ImGui::Text("%s", row.label.c_str());
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
        ImGui::Text("Gizmo: [W] Translate  [E] Rotate  [R] Scale   (%s, %s)",
                    app.gizmo_mode() == GizmoMode::Translate
                        ? "Translate"
                        : (app.gizmo_mode() == GizmoMode::Rotate ? "Rotate" : "Scale"),
                    app.gizmo_space() == GizmoSpace::World ? "World" : "Local");
        if (stats.viewport_lit != 0) {
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
        ImGui::Image(static_cast<ImTextureID>(UiRenderer::kViewportTextureId), img_size,
                     ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
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
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NF_MESH")) {
                intents.viewport_drop = true;
                intents.dropped_mesh_path.assign(static_cast<const char*>(pl->Data), pl->DataSize);
            }
            ImGui::EndDragDropTarget();
        }
        if (!app.selection().has_selection()) {
            ImGui::TextDisabled("No selection — gizmo disabled. Click the scene or an outliner row.");
        } else if (!app.viewport_dragging()) {
            ImGui::TextDisabled("Drag the selection to move it (W/E/R switch mode, Esc cancels).");
        } else {
            ImGui::TextDisabled("Dragging… release to commit (one undo step), Esc cancels.");
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
            }
            if (!ic.error.empty()) {
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Error: %s", ic.error.c_str());
            }
            if (ImGui::CollapsingHeader((AV("name") + "###Name").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::InputText("##name", ic.name, sizeof(ic.name));
                ImGui::SameLine();
                if (ImGui::Button("Apply##name")) {
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
                ImGui::DragFloat3("Position", ic.pos, 0.05f);
                ImGui::DragFloat3("Rotation", ic.rot, 0.5f);
                ImGui::DragFloat3("Scale", ic.scl, 0.02f, 0.01f, 1000.0f);
                if (const auto* t = w->get<scene::Transform>(sel)) {
                    if (t->parent.valid()) {
                        ImGui::Text("Parent: %s", entity_label_ptr(app, t->parent));
                    } else {
                        ImGui::Text("Parent: (root)");
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Apply##transform")) {
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
                    ImGui::InputText("AssetId", ic.mesh_id, sizeof(ic.mesh_id));
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
                    ImGui::Combo("Material", &ic.mat_choice, mat_combo_item, &mat_list,
                                 static_cast<int>(mat_list.size()));
                    ImGui::SameLine();
                    if (ImGui::Button("Assign##material")) {
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
                    if (ImGui::Button("Apply##mesh")) {
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
                mat_tweaked |= ImGui::ColorEdit3("Base color", ic.mat_base);
                mat_tweaked |= ImGui::SliderFloat("Metallic", &ic.mat_metal, 0.0f, 1.0f);
                mat_tweaked |= ImGui::SliderFloat("Roughness", &ic.mat_rough, 0.0f, 1.0f);
                mat_tweaked |= ImGui::SliderFloat("AO", &ic.mat_ao, 0.0f, 1.0f);
                mat_tweaked |= ImGui::ColorEdit3("Emission", ic.mat_em);
                mat_tweaked |=
                    ImGui::SliderFloat("Emission strength", &ic.mat_estr, 0.0f, 8.0f);
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
                ImGui::Combo("Albedo", &ic.albedo_choice, tex_combo_item, &tex_list,
                             static_cast<int>(tex_list.size()));
                ImGui::SameLine();
                if (ImGui::Button("Apply##albedo")) {
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
                ImGui::Combo("Mip filter", &ic.mip_choice, "None\0Nearest\0Linear\0");
                ImGui::SameLine();
                if (ImGui::Button("Apply##mip")) {
                    std::string err;
                    if (!app.set_material_mip_mode(
                            ic.mat_path, static_cast<rhi::MipMapMode>(ic.mip_choice), err)) {
                        ic.error = err;
                        push_error(app.console(), "Mip filter assign failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
                if (ImGui::Button("Apply##material_params")) {
                    std::string err;
                    if (!app.set_material_params(ic.mat_path, material_edit_from_cache(ic), err)) {
                        ic.error = err;
                        push_error(app.console(), "Material edit failed", err);
                    } else {
                        ic.error.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Save material")) {
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
                    if (ImGui::Button("Apply to prefab")) {
                        std::string err;
                        if (!app.apply_prefab(sel, err)) {
                            ic.error = err;
                            push_error(app.console(), "Prefab apply failed", err);
                        } else {
                            ic.error.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Revert to prefab")) {
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
                ImGui::Checkbox("Active", &ic.cam_active);
                ImGui::DragFloat("FOV", &ic.cam[0], 0.5f, 1.0f, 179.0f);
                ImGui::DragFloat("Near", &ic.cam[1], 0.01f, 0.001f, 100.0f);
                ImGui::DragFloat("Far", &ic.cam[2], 1.0f, 0.01f, 10000.0f);
                if (ImGui::Button("Apply##camera")) {
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
                    if (ImGui::Button((AV("add_sky") + "##addsky").c_str())) {
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
                const char* types[] = {"Static", "Dynamic", "Kinematic"};
                ImGui::Combo("Type", &ic.rb_type, types, 3);
                if (ic.rb_type != 0) { // mass only matters for Dynamic/Kinematic
                    ImGui::DragFloat("Mass", &ic.rb_mass, 0.1f, 0.001f, 10000.0f);
                }
                ImGui::SliderFloat("Friction", &ic.rb_friction, 0.0f, 2.0f);
                ImGui::SliderFloat("Restitution", &ic.rb_restitution, 0.0f, 1.0f);
                ImGui::SliderFloat("Linear Damping", &ic.rb_lin_damp, 0.0f, 1.0f);
                ImGui::SliderFloat("Angular Damping", &ic.rb_ang_damp, 0.0f, 1.0f);
                ImGui::Checkbox("Allow Sleep", &ic.rb_allow_sleep);
                if (ImGui::Button("Apply##rigidbody")) {
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
                const char* shapes[] = {"Sphere", "Box", "Plane"};
                ImGui::Combo("Shape", &ic.col_shape, shapes, 3);
                if (ic.col_shape == 0) {
                    ImGui::DragFloat("Radius", &ic.col_radius, 0.05f, 0.001f, 100.0f);
                } else if (ic.col_shape == 1) {
                    ImGui::DragFloat3("Half Extents", ic.col_half, 0.05f, 0.001f, 100.0f);
                } else {
                    ImGui::DragFloat3("Normal", ic.col_normal, 0.05f, -1.0f, 1.0f);
                }
                if (ImGui::Button("Apply##collider")) {
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
                    ImGui::TextDisabled("bones: %d", static_cast<int>(anim->skeleton.bones.size()));
                    if (anim->has_procedural) {
                        ImGui::TextDisabled("procedural: %s (%s, %.2fs)",
                                            anim->procedural_clip_name.c_str(),
                                            anim->procedural.kind ==
                                                    animation::ProceduralClipSpec::Kind::Spin
                                                ? "spin"
                                                : "bob",
                                            static_cast<double>(anim->procedural.duration));
                    }
                    ImGui::TextDisabled("time: %.3fs  state: %s", static_cast<double>(anim->player.time()),
                                        anim->use_state_machine
                                            ? anim->state_machine.current_state().c_str()
                                            : "-");
                }
                if (!ic.anim_clip_names.empty()) {
                    std::vector<const char*> names;
                    names.reserve(ic.anim_clip_names.size());
                    for (const auto& n : ic.anim_clip_names) {
                        names.push_back(n.c_str());
                    }
                    ImGui::Combo("Clip", &ic.anim_clip, names.data(),
                                 static_cast<int>(names.size()));
                } else {
                    ImGui::TextDisabled("no clips (see the Animation: line in the scene file)");
                }
                ImGui::DragFloat("Speed", &ic.anim_speed, 0.05f, -10.0f, 10.0f);
                const char* loops[] = {"None", "Loop", "Ping-Pong"};
                ImGui::Combo("Loop", &ic.anim_loop, loops, 3);
                ImGui::Checkbox("Paused", &ic.anim_paused);
                ImGui::Checkbox("State machine", &ic.anim_state_machine);
                if (ImGui::Button("Apply##animation")) {
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
                    ImGui::TextDisabled("buffer: %s%s",
                                        aud->buffer_name.empty() ? "(generated)"
                                                                 : aud->buffer_name.c_str(),
                                        aud->resolved_buffer() == nullptr ? "  [no data]" : "");
                    ImGui::TextDisabled("cursor: %d frames, playing: %s",
                                        static_cast<int>(aud->sample_cursor),
                                        aud->playing ? "yes" : "no");
                }
                ImGui::SliderFloat("Volume", &ic.aud_volume, 0.0f, 2.0f);
                ImGui::DragFloat("Pitch", &ic.aud_pitch, 0.01f, 0.01f, 4.0f);
                ImGui::Checkbox("Looping", &ic.aud_looping);
                ImGui::Checkbox("Autoplay", &ic.aud_autoplay);
                ImGui::Checkbox("3D (spatial)", &ic.aud_spatial);
                if (ic.aud_spatial) {
                    ImGui::DragFloat("Min distance", &ic.aud_min_dist, 0.1f, 0.01f, 1000.0f);
                    ImGui::DragFloat("Max distance", &ic.aud_max_dist, 0.5f, 0.01f, 10000.0f);
                }
                if (ImGui::Button("Apply##audio")) {
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
                    ImGui::TextDisabled("no gameplay modules are registered in this build");
                } else {
                    static int chosen = 0;
                    std::vector<const char*> names;
                    names.reserve(candidates.size());
                    for (const std::string& candidate : candidates) names.push_back(candidate.c_str());
                    if (chosen >= static_cast<int>(names.size())) chosen = 0;

                    ImGui::Combo("Module", &chosen, names.data(), static_cast<int>(names.size()));
                    ImGui::SameLine();
                    if (ImGui::Button("Attach##gameplay")) {
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
                    ImGui::TextDisabled("attached: %s (%s)", gmc->module_name.c_str(),
                                        gmc->enabled ? "enabled" : "disabled");
                    ImGui::SameLine();
                    if (ImGui::Button("Detach##gameplay")) {
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
                        ImGui::TextDisabled("module '%s' is not registered in this build",
                                            gmc->module_name.c_str());
                    } else {
                        const gameplay::GameplayStateBinding binding = module->state();
                        if (!binding.valid()) {
                            ImGui::TextDisabled("module declares no reflected state");
                        } else {
                            ReflectedObjectView view =
                                ReflectedObjectView::build(binding.instance, binding.meta);
                            if (view.empty()) {
                                ImGui::TextDisabled("module declares no editable properties");
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
            if (ImGui::Button("Undo") && app.stack().can_undo()) {
                std::string err;
                if (!app.undo(err)) {
                    push_error(app.console(), "Undo failed", err);
                } else {
                    ic.valid = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Redo") && app.stack().can_redo()) {
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
        if (ImGui::InputText("Filter", filter, sizeof(filter))) {
            app.browser().filter_text = filter;
        }
        // External import (no OS dialog in v0.1: absolute source path input).
        // Types by extension: .nfmesh/.png/.jpg/.bmp/.tga/.nfmat/.nfscene.
        static char import_src[256]{};
        static char import_dir[128] = "content://Meshes";
        static bool import_overwrite = false;
        ImGui::InputText("Import source", import_src, sizeof(import_src));
        ImGui::SameLine();
        ImGui::InputText("Import to", import_dir, sizeof(import_dir));
        ImGui::SameLine();
        ImGui::Checkbox("Overwrite", &import_overwrite);
        ImGui::SameLine();
        if (ImGui::Button("Import")) {
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
        if (ImGui::Button("Clear finished imports")) {
            app.import_queue().clear_finished();
        }
        const std::vector<AssetEntry> entries = app.browser_entries();
        ImGui::Text("%zu assets", entries.size());
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
        const char* levels[] = {"Trace", "Debug", "Info", "Warn", "Error"};
        ImGui::Combo("Level", &level_filter, levels, 5);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            app.console().clear();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("dropped=%zu", app.console().dropped());
        const std::vector<LogMessage> lines =
            app.console().filtered(static_cast<LogLevel>(level_filter));
        if (ImGui::BeginChild("##consolelines", ImVec2(0, 0), true)) {
            for (const LogMessage& m : lines) {
                ImGui::TextColored(level_color(m.level), "[%s] %s", level_name(m.level), m.text.c_str());
            }
            if (!lines.empty()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    return intents;
}

} // namespace nf::editor
