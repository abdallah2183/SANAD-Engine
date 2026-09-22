// Toolbar.cpp — the editor's top bar: menus, icon toolbar, settings, dialogs.
//
// Icons are DRAWN with ImDrawList primitives, not typed as Unicode symbols. The
// previous bar used symbol characters and every one of them rasterised as an
// empty box, because the UI font has no glyph for those code points: a row of
// diamonds is what the user saw. Lines, arcs and triangles always render, scale
// with the button and take the accent colour.
//
// Layout: a real menu bar (File/Edit/View/Create/Game/Settings/Help) plus a
// toolbar row reserved through BeginViewportSideBar, so the dockspace starts
// BELOW the toolbar instead of under it. Buttons carry explicit padding between
// them — "the buttons at the top left are stuck together" was a real report.
//
// This file is ASCII-only on purpose: Arabic strings live in the localization
// table (NF/UI/Localization.cpp) and reach the UI through TR()/AV(), so a
// console or editor that mangles UTF-8 cannot corrupt the source.

#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Editor/ProjectLauncher.hpp> // save_language() for the live toggles

#include <NF/Assets/MeshExport.hpp>
#include <NF/Core/Profiler.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>
#include <NF/Editor/UiText.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace nf::editor {

// The preset palette table (sky_preset_values / sky_preset_edit) is inline in
// ToolbarUi.hpp: it is pure data, and this file belongs to the editor
// *application* target while NFEditorCore — which the test suite links — does
// not compile it.

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Every string that reaches a widget goes through AV (NF/Editor/UiText.hpp):
// it translates AND shapes into Arabic presentation forms. ImGui has no bidi
// reordering, so an unshaped logical-order string renders MIRRORED — that is
// the entire bug class behind "فأظشرلأا عوطق" instead of "إظهار الشبكة".
//
// There is deliberately no TR() helper in this file any more. It existed only to
// be misused: 24 call sites passed it straight into Checkbox/DragFloat/Combo/
// MenuItem/Text, i.e. exactly the display positions it is wrong for. If a key is
// ever genuinely needed for LOGIC (an ID, a comparison, a lookup) call
// ui::tr(key) directly, so the unshaped call is visibly deliberate instead of
// looking like the default.

void push_info_console(EditorApp& app, const std::string& what) {
    app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor, what,
                                  std::chrono::system_clock::now(), __FILE__, __LINE__});
}

void push_error_console(EditorApp& app, const std::string& what, const std::string& err) {
    app.console().push(LogMessage{LogLevel::Error, LogCategory::Editor, what + ": " + err,
                                  std::chrono::system_clock::now(), __FILE__, __LINE__});
}

// --- icons -------------------------------------------------------------------

enum class Icon {
    New, Open, Save, Play, Stop, Move, Rotate, Scale, Snap, Ground, Cube, Sun, Camera, Sky,
    Import, Export, Settings, Undo, Redo, Delete,
};

void draw_icon(ImDrawList* dl, ImVec2 c, float s, Icon icon, ImU32 col) {
    const float t = std::max(1.4f, s * 0.085f); // a stroke that survives DPI
    const float h = s * 0.5f;                   // half extent
    const float d = s * 0.16f;                  // arrow head (move icon)
    switch (icon) {
        case Icon::New: {
            const float f = s * 0.28f; // folded corner, like a new document
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, 1.5f, 0, t);
            dl->AddLine(ImVec2(c.x + h - f, c.y - h), ImVec2(c.x + h, c.y - h + f), col, t);
            dl->AddLine(ImVec2(c.x + h - f, c.y - h), ImVec2(c.x + h - f, c.y - h + f), col, t);
            dl->AddLine(ImVec2(c.x + h - f, c.y - h + f), ImVec2(c.x + h, c.y - h + f), col, t);
            break;
        }
        case Icon::Open: {
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.55f), ImVec2(c.x + h, c.y + h * 0.6f), col, 1.5f, 0,
                        t);
            dl->AddLine(ImVec2(c.x - h, c.y - h * 0.55f), ImVec2(c.x - h * 0.25f, c.y - h * 0.95f),
                        col, t);
            dl->AddLine(ImVec2(c.x - h * 0.25f, c.y - h * 0.95f),
                        ImVec2(c.x + h * 0.1f, c.y - h * 0.55f), col, t);
            break;
        }
        case Icon::Save: {
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, 1.5f, 0, t);
            dl->AddRectFilled(ImVec2(c.x - h * 0.45f, c.y - h),
                              ImVec2(c.x + h * 0.45f, c.y - h * 0.15f), col, 1.0f);
            dl->AddRect(ImVec2(c.x - h * 0.6f, c.y + h * 0.25f), ImVec2(c.x + h * 0.6f, c.y + h), col,
                        1.0f, 0, t);
            break;
        }
        case Icon::Play:
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.6f, c.y - h * 0.85f),
                                  ImVec2(c.x - h * 0.6f, c.y + h * 0.85f), ImVec2(c.x + h * 0.8f, c.y),
                                  col);
            break;
        case Icon::Stop:
            dl->AddRectFilled(ImVec2(c.x - h * 0.8f, c.y - h * 0.8f),
                              ImVec2(c.x + h * 0.8f, c.y + h * 0.8f), col, 1.5f);
            break;
        case Icon::Move: {
            dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), col, t);
            dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y + h), col, t);
            dl->AddTriangleFilled(ImVec2(c.x + h, c.y), ImVec2(c.x + h - d, c.y - d),
                                  ImVec2(c.x + h - d, c.y + d), col);
            dl->AddTriangleFilled(ImVec2(c.x - h, c.y), ImVec2(c.x - h + d, c.y - d),
                                  ImVec2(c.x - h + d, c.y + d), col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - h), ImVec2(c.x - d, c.y - h + d),
                                  ImVec2(c.x + d, c.y - h + d), col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + h), ImVec2(c.x - d, c.y + h - d),
                                  ImVec2(c.x + d, c.y + h - d), col);
            break;
        }
        case Icon::Rotate: {
            // Three-quarter arc plus a head: "turn about an axis".
            dl->PathArcTo(c, h * 0.85f, 0.35f * kPi, 1.85f * kPi, 28);
            dl->PathStroke(col, 0, t);
            const ImVec2 tip(c.x + h * 0.85f * std::cos(0.35f * kPi),
                             c.y + h * 0.85f * std::sin(0.35f * kPi));
            const float a = s * 0.2f;
            dl->AddTriangleFilled(ImVec2(tip.x + a, tip.y - a * 0.2f),
                                  ImVec2(tip.x - a * 0.6f, tip.y - a),
                                  ImVec2(tip.x + a * 0.2f, tip.y + a), col);
            break;
        }
        case Icon::Scale: {
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h * 0.1f, c.y + h * 0.1f), col, 1.0f, 0,
                        t);
            dl->AddRectFilled(ImVec2(c.x + h * 0.2f, c.y + h * 0.2f), ImVec2(c.x + h, c.y + h), col,
                              1.0f);
            dl->AddLine(ImVec2(c.x - h * 0.1f, c.y - h * 0.1f),
                        ImVec2(c.x + h * 0.35f, c.y + h * 0.35f), col, t);
            break;
        }
        case Icon::Snap: {
            for (int i = 0; i <= 2; ++i) {
                const float o = -h + (2.0f * h) * (static_cast<float>(i) / 2.0f);
                dl->AddLine(ImVec2(c.x - h, c.y + o), ImVec2(c.x + h, c.y + o), col, t * 0.7f);
                dl->AddLine(ImVec2(c.x + o, c.y - h), ImVec2(c.x + o, c.y + h), col, t * 0.7f);
            }
            dl->AddCircleFilled(ImVec2(c.x + h * 0.5f, c.y - h * 0.5f), s * 0.12f, col);
            break;
        }
        case Icon::Ground: {
            dl->AddLine(ImVec2(c.x - h, c.y + h * 0.2f), ImVec2(c.x + h, c.y + h * 0.2f), col, t);
            dl->AddLine(ImVec2(c.x - h, c.y + h * 0.7f), ImVec2(c.x + h, c.y + h * 0.7f), col, t * 0.6f);
            dl->AddLine(ImVec2(c.x - h * 0.5f, c.y + h * 0.2f), ImVec2(c.x - h * 0.1f, c.y - h * 0.6f),
                        col, t * 0.7f);
            dl->AddLine(ImVec2(c.x + h * 0.5f, c.y + h * 0.2f), ImVec2(c.x + h * 0.1f, c.y - h * 0.6f),
                        col, t * 0.7f);
            break;
        }
        case Icon::Cube: {
            // Isometric cube: the silhouette every 3D tool uses for "mesh".
            const ImVec2 top(c.x, c.y - h);
            const ImVec2 right(c.x + h, c.y - h * 0.45f);
            const ImVec2 mid(c.x, c.y + h * 0.1f);
            const ImVec2 left(c.x - h, c.y - h * 0.45f);
            const ImVec2 bottom(c.x, c.y + h);
            dl->AddQuad(top, right, mid, left, col, t);
            dl->AddQuad(left, mid, bottom, ImVec2(c.x - h, c.y + h * 0.45f), col, t);
            dl->AddQuad(mid, right, ImVec2(c.x + h, c.y + h * 0.45f), bottom, col, t);
            break;
        }
        case Icon::Sun: {
            dl->AddCircle(ImVec2(c.x, c.y), h * 0.45f, col, 20, t);
            for (int i = 0; i < 8; ++i) {
                const float ang = (static_cast<float>(i) / 8.0f) * 2.0f * kPi;
                dl->AddLine(ImVec2(c.x + std::cos(ang) * h * 0.62f, c.y + std::sin(ang) * h * 0.62f),
                            ImVec2(c.x + std::cos(ang) * h, c.y + std::sin(ang) * h), col, t);
            }
            break;
        }
        case Icon::Camera: {
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.55f), ImVec2(c.x + h, c.y + h * 0.7f), col, 2.0f, 0,
                        t);
            dl->AddTriangleFilled(ImVec2(c.x + h, c.y + h * 0.1f),
                                  ImVec2(c.x + h * 1.6f, c.y - h * 0.5f),
                                  ImVec2(c.x + h * 1.6f, c.y + h * 0.7f), col);
            break;
        }
case Icon::Sky: {
            const float r = s * 0.26f;
            dl->AddCircle(ImVec2(c.x - r * 0.7f, c.y), r, col, 16, t);
            dl->AddCircle(ImVec2(c.x + r * 0.55f, c.y - r * 0.25f), r * 1.15f, col, 16, t);
            dl->AddCircle(ImVec2(c.x + r * 1.5f, c.y), r * 0.8f, col, 16, t);
            dl->AddLine(ImVec2(c.x - r * 1.7f, c.y + r), ImVec2(c.x + r * 2.3f, c.y + r), col, t);
            break;
        }
        case Icon::Import: {
            dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y + h * 0.2f), col, t);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + h * 0.5f), ImVec2(c.x - s * 0.2f, c.y - h * 0.05f),
                                  ImVec2(c.x + s * 0.2f, c.y - h * 0.05f), col);
            dl->AddLine(ImVec2(c.x - h, c.y + h * 0.85f), ImVec2(c.x + h, c.y + h * 0.85f), col, t);
            break;
        }
        case Icon::Export: {
            dl->AddLine(ImVec2(c.x, c.y + h), ImVec2(c.x, c.y - h * 0.2f), col, t);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - h * 0.5f), ImVec2(c.x - s * 0.2f, c.y + h * 0.05f),
                                  ImVec2(c.x + s * 0.2f, c.y + h * 0.05f), col);
            dl->AddLine(ImVec2(c.x - h, c.y + h * 0.85f), ImVec2(c.x + h, c.y + h * 0.85f), col, t);
            break;
        }
        case Icon::Settings: {
            dl->AddCircle(ImVec2(c.x, c.y), h * 0.45f, col, 18, t);
            for (int i = 0; i < 6; ++i) {
                const float ang = (static_cast<float>(i) / 6.0f) * 2.0f * kPi;
                dl->AddLine(ImVec2(c.x + std::cos(ang) * h * 0.62f, c.y + std::sin(ang) * h * 0.62f),
                            ImVec2(c.x + std::cos(ang) * h, c.y + std::sin(ang) * h), col, t);
            }
            break;
        }
        case Icon::Undo:
        case Icon::Redo: {
            // Mirror images of one another: an arc over an arrow head.
            const float dir = (icon == Icon::Undo) ? 1.0f : -1.0f;
            dl->PathArcTo(ImVec2(c.x, c.y + h * 0.35f), h * 0.8f, 1.1f * kPi, 1.9f * kPi, 20);
            dl->PathStroke(col, 0, t);
            const ImVec2 tip(c.x - dir * h * 0.8f, c.y + h * 0.3f);
            dl->AddTriangleFilled(ImVec2(tip.x, tip.y + s * 0.17f),
                                  ImVec2(tip.x + dir * s * 0.2f, tip.y - s * 0.02f),
                                  ImVec2(tip.x - dir * s * 0.3f, tip.y + s * 0.32f), col);
            break;
        }
        case Icon::Delete: {
            dl->AddRect(ImVec2(c.x - h * 0.7f, c.y - h * 0.4f), ImVec2(c.x + h * 0.7f, c.y + h), col,
                        1.0f, 0, t);
            dl->AddLine(ImVec2(c.x - h, c.y - h * 0.4f), ImVec2(c.x + h, c.y - h * 0.4f), col, t);
            dl->AddLine(ImVec2(c.x - h * 0.3f, c.y - h * 0.7f), ImVec2(c.x + h * 0.3f, c.y - h * 0.7f),
                        col, t);
            break;
        }
    }
}

/// An icon button. `id` keeps widget identity stable while the label changes
/// with the language; the tooltip carries the translated, shaped name. The
/// padding between buttons is explicit, because "the buttons at the top left are
/// stuck together" was a real report about the previous text-only toolbar.
bool icon_button(const char* id, Icon icon, const std::string& tooltip, bool active,
                 float size = 26.0f) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##btn", ImVec2(size, size));
    const bool clicked = ImGui::IsItemActivated() && ImGui::IsItemHovered();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = active ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                            : (hovered ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                       : ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), bg, 3.0f);
    if (active) {
        dl->AddRect(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(ImGuiCol_CheckMark), 3.0f, 0,
                    1.5f);
    }
    const ImU32 fg = ImGui::GetColorU32(active ? ImGuiCol_CheckMark : ImGuiCol_Text);
    draw_icon(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.52f, icon, fg);
    if (!tooltip.empty() && hovered) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
    ImGui::PopID();
    return clicked;
}

/// Same button, text label: for actions whose meaning a glyph cannot carry
/// (Local/World is a state, not an icon).
bool text_toolbar_button(const char* id, const std::string& label, bool active) {
    ImGui::PushID(id);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::Button(label.c_str(), ImVec2(0.0f, 26.0f));
    if (active) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return clicked;
}
// --- native file dialogs -----------------------------------------------------
//
// The editor is a Windows program already, so it can ask the OS for a real
// picker. A path text box is not a substitute for "show me my files", which is
// exactly what the Import/Export request was about.

#ifdef _WIN32
/// Win32 Save-As. False means the user cancelled (out_path untouched) — never a
/// hard error, because cancelling is a normal thing to do.
bool save_file_dialog(const std::string& title, const std::string& filter,
                      const std::string& default_ext, std::string& out_path) {
    char buffer[MAX_PATH * 2] = {};
    std::snprintf(buffer, sizeof(buffer), "%s", out_path.c_str());
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetActiveWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = sizeof(buffer);
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrDefExt = default_ext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (::GetSaveFileNameA(&ofn) != TRUE) {
        return false;
    }
    out_path = buffer;
    return true;
}

bool open_file_dialog(const std::string& title, const std::string& filter, std::string& out_path) {
    char buffer[MAX_PATH * 2] = {};
    std::snprintf(buffer, sizeof(buffer), "%s", out_path.c_str());
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetActiveWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = sizeof(buffer);
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameA(&ofn) != TRUE) {
        return false;
    }
    out_path = buffer;
    return true;
}
#else
bool save_file_dialog(const std::string&, const std::string&, const std::string&, std::string&) {
    return false;
}
bool open_file_dialog(const std::string&, const std::string&, std::string&) {
    return false;
}
#endif

/// Filter string for the export dialog, built from the formats this build can
/// actually write. A hardcoded list here is how a dialog ends up offering a
/// format the exporter does not implement.
///
/// The Win32 filter is a sequence of double-NUL-terminated "name\0pattern\0"
/// pairs followed by an extra NUL, so the embedded zeros are deliberate.
std::string export_filter() {
    std::string f;
    for (const assets::MeshFormat fmt : assets::mesh_formats()) {
        const std::string ext = assets::mesh_format_extension(fmt);
        f += std::string(assets::mesh_format_name(fmt)) + " (*." + ext + ")\0*." + ext;
        f += '\0';
    }
    f += "All files (*.*)\0*.*";
    f += '\0';
    f += '\0';
    return f;
}

/// Extensions the import queue accepts (ImportQueue sniffs the same set).
std::string import_filter() {
    std::string f;
    f += "Models (*.gltf;*.glb;*.nfmesh)\0*.gltf;*.glb;*.nfmesh";
    f += '\0';
    f += "Textures (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga";
    f += '\0';
    f += "Audio (*.wav;*.ogg;*.mp3)\0*.wav;*.ogg;*.mp3";
    f += '\0';
    f += "All files (*.*)\0*.*";
    f += '\0';
    f += '\0';
    return f;
}

std::string scene_filter() {
    std::string f;
    f += "NOVAForge Scene (*.nfscene)\0*.nfscene";
    f += '\0';
    f += "All files (*.*)\0*.*";
    f += '\0';
    f += '\0';
    return f;
}
/// Sky presets behind the Create > Sky and Settings buttons. They are the same
/// palettes rendering::TimeOfDay blends through (day / golden hour / night), so
/// a preset picked here and a timelapse run agree about what those names mean.
void apply_sky_preset(EditorApp& app, int preset) {
    // One sky per scene, and the edit lands either way. This used to return
    // straight after create_sky_entity(), which installs a DEFAULT sky — so on
    // any scene that had no sky yet, choosing "night" produced a bright day sky
    // and the dropdown looked like it did nothing. EditorApp::apply_sky_edit now
    // owns "find or create, then apply", so the two paths cannot diverge again.
    std::string err;
    if (!app.apply_sky_edit(sky_preset_edit(preset), err)) {
        push_error_console(app, "Sky preset failed", err);
    }
}

void create_primitive_item(EditorApp& app, PrimitiveKind kind, const char* key) {
    if (ImGui::MenuItem(AV(key).c_str())) {
        std::string err;
        if (!app.create_primitive(kind, ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create failed", std::string(key) + ": " + err);
        }
    }
}

void create_menu_contents(EditorApp& app) {
    create_primitive_item(app, PrimitiveKind::Ground, "add_ground");
    create_primitive_item(app, PrimitiveKind::Cube, "add_cube");
    create_primitive_item(app, PrimitiveKind::Sphere, "add_sphere");
    create_primitive_item(app, PrimitiveKind::Quad, "add_quad");
    ImGui::Separator();
    if (ImGui::MenuItem(AV("add_light").c_str())) {
        std::string err;
        if (!app.create_directional_light(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create light failed", err);
        }
    }
    if (ImGui::MenuItem(AV("add_camera").c_str())) {
        std::string err;
        if (!app.create_camera(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create camera failed", err);
        }
    }
    if (ImGui::MenuItem(AV("add_sky").c_str())) {
        std::string err;
        if (!app.create_sky_entity(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create sky failed", err);
        }
    }
    if (ImGui::MenuItem((AV("add_sky") + " - " + AV("sky_sunset")).c_str())) {
        apply_sky_preset(app, 1);
    }
    if (ImGui::MenuItem((AV("add_sky") + " - " + AV("sky_night")).c_str())) {
        apply_sky_preset(app, 2);
    }
}
/// A panel-section header used by the settings window.
bool settings_section(const std::string& label, bool default_open = false) {
    return ImGui::CollapsingHeader(label.c_str(),
                                   default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

/// The autosave interval is one preference, not two: the Game menu and the
/// Settings window edit the SAME session value, so both go through this. It
/// starts at two minutes — long enough to stay out of the way, short enough
/// that a crash loses almost nothing.
float& autosave_interval() {
    static float interval = 120.0f;
    return interval;
}

/// Play/Stop and the save-slot menu live here rather than in the panels: they
/// are session actions, not scene editing.
void game_menu_contents(EditorApp& app) {
    if (!app.playing()) {
        if (ImGui::MenuItem(AV("play").c_str())) {
            std::string err;
            if (!app.play(err)) {
                push_error_console(app, "Play failed", err);
            }
        }
    } else if (ImGui::MenuItem(AV("stop").c_str())) {
        std::string err;
        if (!app.stop(err)) {
            push_error_console(app, "Stop failed", err);
        }
    }
    ImGui::Separator();
    static char slot[64] = "slot1";
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText(AV("slot").c_str(), slot, sizeof(slot));
    if (ImGui::MenuItem(AV("save_game").c_str())) {
        std::string err;
        if (!app.save_game(slot, err)) {
            push_error_console(app, "Save game failed", err);
        } else {
            push_info_console(app, std::string("Saved game to '") + slot + "'");
        }
    }
    if (ImGui::MenuItem(AV("load_game").c_str())) {
        std::string err;
        if (!app.load_game(slot, err)) {
            push_error_console(app, "Load game failed", err);
        } else {
            push_info_console(app, std::string("Loaded game from '") + slot + "'");
        }
    }
    if (!app.has_save(slot)) {
        ImGui::TextDisabled("%s", AV("no_such_slot").c_str());
    }
    ImGui::Separator();
    bool autosave = app.autosave_enabled();
    if (ImGui::Checkbox(AV("autosave").c_str(), &autosave)) {
        app.set_autosave(autosave ? autosave_interval() : 0.0f, "autosave_");
    }
    if (autosave) {
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat(AV("autosave_interval").c_str(), &autosave_interval(), 5.0f, 5.0f,
                             3600.0f, "%.0f")) {
            app.set_autosave(autosave_interval(), "autosave_");
        }
    }
}
} // namespace

EditorUiSettings& ui_settings() {
    static EditorUiSettings settings;
    return settings;
}

void apply_ui_accent(const EditorUiSettings& settings) {
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 accent(settings.accent_r, settings.accent_g, settings.accent_b, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.75f);
    style.Colors[ImGuiCol_Button] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.60f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.85f);
    style.Colors[ImGuiCol_CheckMark] = accent;
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    style.Colors[ImGuiCol_SliderGrabActive] = accent;
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
}

namespace {

/// One menu entry per gizmo mode, with the shortcut the viewport also honours.
void gizmo_mode_menu(EditorApp& app, GizmoMode mode, const char* key, const char* shortcut) {
    const bool on = (app.gizmo_mode() == mode);
    if (ImGui::MenuItem(AV(key).c_str(), shortcut, on)) {
        app.set_gizmo_mode(mode);
    }
}

/// Same action as gizmo_mode_menu, as a held-accent icon button.
void gizmo_mode_button(EditorApp& app, const char* id, Icon icon, const char* key,
                       const char* shortcut_suffix, GizmoMode mode) {
    const bool on = (app.gizmo_mode() == mode);
    if (icon_button(id, icon, AV(key) + shortcut_suffix, on)) {
        app.set_gizmo_mode(mode);
    }
}

/// Create-menu primitive as a toolbar button (asset cook + entity, one undo).
void create_primitive_button(EditorApp& app, const char* id, Icon icon, PrimitiveKind kind,
                             const char* key) {
    if (icon_button(id, icon, AV(key), false)) {
        std::string err;
        if (!app.create_primitive(kind, ecs::kInvalidEntity, err)) {
            push_error_console(app, std::string("Create ") + key + " failed", err);
        }
    }
}

/// Where the export dialog points before the user browses: the scene's own name
/// in the user's Pictures folder, which exists on every Windows install. The
/// extension is added by the caller, because it depends on the format.
std::string suggested_export_name(EditorApp& app) {
    std::string stem = app.status().scene_label;
    // The label carries the dirty marker and the "(path)" suffix; neither
    // belongs in a file name.
    const size_t marker = stem.find_first_of("*(");
    if (marker != std::string::npos) {
        stem.erase(marker);
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '-')) {
        stem.pop_back();
    }
    if (stem.empty()) {
        stem = "scene";
    }
    // The user's Pictures folder, which exists on every Windows install; the
    // working directory is the fallback so a headless or unusual environment
    // still gets a usable suggestion rather than an empty string.
    std::string dir = ".";
#ifdef _WIN32
    // _dupenv_s rather than getenv: the editor builds /W4 /WX and the CRT marks
    // getenv deprecated there.
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, "USERPROFILE") == 0 && buffer != nullptr) {
        dir = std::string(buffer) + "\\Pictures";
        free(buffer);
    }
#endif
    return dir + "\\" + stem;
}

/// Right-aligned push so the status block sits at the menu bar's edge.
float status_block_start() {
    return ImGui::GetWindowWidth() * 0.45f;
}

void sync_snap_from_settings(EditorApp& app, const EditorUiSettings& settings) {
    app.gizmo_snap() = GizmoSnap{settings.snap_enabled ? settings.snap_move : 0.0f,
                                 settings.snap_enabled ? settings.snap_rotate : 0.0f,
                                 settings.snap_enabled ? settings.snap_scale : 0.0f};
}

} // namespace
void toolbar_ui(EditorApp& app, const UiFrameStats& stats, UiIntents& intents) {
    EditorUiSettings& st = ui_settings();

    // --- Menu bar -------------------------------------------------------------
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(("  " + AV("file")).c_str())) {
            if (ImGui::MenuItem(AV("new_scene").c_str(), "Ctrl+N")) {
                std::string err;
                if (!app.new_scene(err)) {
                    push_error_console(app, "New scene failed", err);
                }
            }
            if (ImGui::MenuItem(AV("open_scene").c_str(), "Ctrl+O")) {
                // A real picker: the scene list is not the only place a scene can
                // live, and this is how a user reaches one outside it.
                std::string picked;
                if (open_file_dialog(AV("open_scene"), scene_filter(), picked)) {
                    intents.open_scene_dialog_confirm = true;
                    intents.open_scene_path = picked;
                }
            }
            if (ImGui::MenuItem(AV("save_scene").c_str(), "Ctrl+S")) {
                std::string err;
                if (!app.save(err)) {
                    push_error_console(app, "Save failed", err);
                }
            }
            if (ImGui::MenuItem(AV("save_scene_as").c_str(), "Ctrl+Shift+S")) {
                std::string picked = app.scene_path();
                if (save_file_dialog(AV("save_scene_as"), scene_filter(), "nfscene", picked)) {
                    intents.save_as_confirm = true;
                    intents.save_as_path = picked;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem((AV("import_model") + "   Ctrl+I").c_str())) {
                st.show_import = true;
            }
            if (ImGui::MenuItem((AV("export") + "   Ctrl+E").c_str())) {
                st.show_export = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(AV("settings").c_str())) {
                st.show_settings = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(("  " + AV("edit")).c_str())) {
            const bool can_undo = app.stack().undo_size() > 0;
            const bool can_redo = app.stack().redo_size() > 0;
            if (ImGui::MenuItem(AV("undo").c_str(), "Ctrl+Z", false, can_undo)) {
                std::string err;
                if (!app.undo(err)) {
                    push_error_console(app, "Undo failed", err);
                }
            }
            if (ImGui::MenuItem(AV("redo").c_str(), "Ctrl+Y", false, can_redo)) {
                std::string err;
                if (!app.redo(err)) {
                    push_error_console(app, "Redo failed", err);
                }
            }
            ImGui::Separator();
            const bool has_sel = app.selection().has_selection();
            if (ImGui::MenuItem(AV("delete").c_str(), "Del", false, has_sel)) {
                // A4: immediate and undoable, matching the outliner button and
                // the Del key. Undo (Ctrl+Z) is the safety net, not a confirm.
                std::string err;
                if (!app.delete_entity(app.selection().primary(), err)) {
                    push_error_console(app, "Delete failed", err);
                }
            }
            if (ImGui::MenuItem(AV("save_prefab").c_str(), nullptr, false, has_sel)) {
                std::string err;
                const ecs::Entity sel = app.selection().primary();
                const std::string path = "content://Prefabs/" + std::to_string(sel.id) + ".nfscene";
                if (!app.create_prefab(sel, path, err)) {
                    push_error_console(app, "Save prefab failed", err);
                } else {
                    push_info_console(app, "Prefab saved: " + path);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(("  " + AV("view")).c_str())) {
            ImGui::MenuItem(AV("grid_step").c_str(), nullptr, &st.show_grid);
            ImGui::MenuItem(AV("fps").c_str(), nullptr, &st.show_fps);
            ImGui::MenuItem(AV("validation").c_str(), nullptr, &st.show_validation);
            ImGui::Separator();
            gizmo_mode_menu(app, GizmoMode::Translate, "move", "W");
            gizmo_mode_menu(app, GizmoMode::Rotate, "rotate", "E");
            gizmo_mode_menu(app, GizmoMode::Scale, "scale_tool", "R");
            ImGui::Separator();
            const bool local_space = (app.gizmo_space() == GizmoSpace::Local);
            if (ImGui::MenuItem(local_space ? AV("local_space").c_str() : AV("world_space").c_str(),
                                nullptr, local_space)) {
                app.set_gizmo_space(local_space ? GizmoSpace::World : GizmoSpace::Local);
            }
            if (ImGui::MenuItem(AV("snap").c_str(), nullptr, &st.snap_enabled)) {
                sync_snap_from_settings(app, st);
            }
            ImGui::Separator();
            bool profiler = ui_settings().show_profiler;
            if (ImGui::MenuItem(AV("profiler").c_str(), nullptr, &profiler)) {
                ui_settings().show_profiler = profiler;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(("  " + AV("create")).c_str())) {
            create_menu_contents(app);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(("  " + AV("game")).c_str())) {
            game_menu_contents(app);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(("  " + AV("settings")).c_str())) {
            if (ImGui::MenuItem(AV("preferences").c_str())) {
                st.show_settings = true;
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(AV("sky_preset").c_str())) {
                if (ImGui::MenuItem(AV("sky_day").c_str(), nullptr, st.sky_preset == 0)) {
                    st.sky_preset = 0;
                    apply_sky_preset(app, 0);
                }
                if (ImGui::MenuItem(AV("sky_sunset").c_str(), nullptr, st.sky_preset == 1)) {
                    st.sky_preset = 1;
                    apply_sky_preset(app, 1);
                }
                if (ImGui::MenuItem(AV("sky_night").c_str(), nullptr, st.sky_preset == 2)) {
                    st.sky_preset = 2;
                    apply_sky_preset(app, 2);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // The label is whichever language is NOT active: that is what the
            // click switches to. The ID stays stable across the flip, so the
            // widget does not lose hover mid-toggle.
            const bool arabic = (ui::current_language() == ui::Language::Arabic);
            const std::string lang_label = arabic ? std::string("English") : AV("arabic_name");
            if (ImGui::MenuItem((lang_label + "###langmenu").c_str())) {
                ui::set_language(arabic ? ui::Language::English : ui::Language::Arabic);
                save_language(!arabic); // persist the flip (shell settings file)
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(("  " + AV("help")).c_str())) {
            if (ImGui::MenuItem(AV("about").c_str())) {
                st.show_about = true;
            }
            ImGui::EndMenu();
        }

        // Right-hand side: the live status. The numbers that used to sit in the
        // middle of the bar are still here, just not on top of the buttons.
        const EditorStatus status = app.status();
        const float right_start = status_block_start();
        if (ImGui::GetCursorPosX() < right_start) {
            ImGui::SetCursorPosX(right_start);
        }
        if (rt_active()) {
            // RTL: one shaped line, right-aligned (SameLine chains cannot
            // mirror per item, so the whole block becomes a single string).
            // Parts compose in LOGICAL order, then shape once — the shaper
            // keeps the Latin/digit runs (paths, numbers) in order.
            std::string line;
            const auto sep = [&]() -> const char* { return line.empty() ? "" : " | "; };
            if (st.show_fps) {
                char fb[64] = {};
                std::snprintf(fb, sizeof(fb), "%.1f %s | %.2f ms", status.fps,
                              ui::tr("fps").c_str(), status.frame_ms);
                line += fb;
            }
            if (stats.validation_on && st.show_validation) {
                char vb[64] = {};
                std::snprintf(vb, sizeof(vb), "%s%s %u", sep(), ui::tr("validation").c_str(),
                              stats.validation_errors);
                line += vb;
            }
            {
                char eb[64] = {};
                std::snprintf(eb, sizeof(eb), "%s%zu %s", sep(), status.entity_count,
                              ui::tr("entities").c_str());
                line += eb;
            }
            if (app.has_project()) {
                line += sep();
                line += ui::tr("project") + " " + app.project_name();
            }
            rt_disabled_str(ui::shape_arabic(line));
        } else {
            if (st.show_fps) {
                ImGui::TextDisabled("%.1f %s | %.2f ms", status.fps, ui::tr("fps").c_str(),
                                    status.frame_ms);
            }
            if (stats.validation_on && st.show_validation) {
                ImGui::SameLine();
                ImGui::TextDisabled("| %s %u", AV("validation").c_str(), stats.validation_errors);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("| %zu %s", status.entity_count, AV("entities").c_str());
            if (app.has_project()) {
                ImGui::SameLine();
                ImGui::TextDisabled("| %s %s", AV("project").c_str(), app.project_name().c_str());
            }
        }
        ImGui::EndMainMenuBar();
    }
// --- Toolbar row ----------------------------------------------------------
    //
    // BeginViewportSideBar reserves the strip above the dockspace, so the panels
    // start BELOW the toolbar. Painting the buttons over the first docked panel
    // (which is what a plain window at y = 0 does) is the other classic way to
    // make a toolbar feel broken.
    const float toolbar_h = ImGui::GetFrameHeight() + 10.0f;
    if (ImGui::BeginViewportSideBar("##NFToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, toolbar_h,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextDisabled("NOVAForge");
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();

            if (icon_button("##tb_new", Icon::New, AV("new_scene"), false)) {
                std::string err;
                if (!app.new_scene(err)) {
                    push_error_console(app, "New scene failed", err);
                }
            }
            ImGui::SameLine();
            if (icon_button("##tb_open", Icon::Open, AV("open_scene"), false)) {
                std::string picked;
                if (open_file_dialog(AV("open_scene"), scene_filter(), picked)) {
                    intents.open_scene_dialog_confirm = true;
                    intents.open_scene_path = picked;
                }
            }
            ImGui::SameLine();
            if (icon_button("##tb_save", Icon::Save, AV("save_scene"), false)) {
                std::string err;
                if (!app.save(err)) {
                    push_error_console(app, "Save failed", err);
                }
            }
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();

            // Gizmo mode: held-accent buttons, and W/E/R keep working.
            gizmo_mode_button(app, "##tb_move", Icon::Move, "move", "   (W)",
                              GizmoMode::Translate);
            ImGui::SameLine();
            gizmo_mode_button(app, "##tb_rotate", Icon::Rotate, "rotate", "   (E)",
                              GizmoMode::Rotate);
            ImGui::SameLine();
            gizmo_mode_button(app, "##tb_scale", Icon::Scale, "scale_tool", "   (R)",
                              GizmoMode::Scale);
            ImGui::SameLine();

            // Local/World reads as a labelled state, so the glyph does not have
            // to carry the meaning on its own.
            const bool local_space = (app.gizmo_space() == GizmoSpace::Local);
            if (text_toolbar_button("##tb_space", local_space ? AV("local_space") : AV("world_space"),
                                    local_space)) {
                app.set_gizmo_space(local_space ? GizmoSpace::World : GizmoSpace::Local);
            }
            ImGui::SameLine();
            if (icon_button("##tb_snap", Icon::Snap, AV("snap"), st.snap_enabled)) {
                st.snap_enabled = !st.snap_enabled;
                sync_snap_from_settings(app, st);
            }
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            // Decluttered row (G9): only actions used while a scene is being
            // shaped keep a slot. The once-per-scene creates (light, camera,
            // sky) live in Create, and Import/Export live in File with their
            // Ctrl+I / Ctrl+E shortcuts — a button that is clicked twice per
            // project does not earn permanent space in the bar.
            create_primitive_button(app, "##tb_ground", Icon::Ground, PrimitiveKind::Ground, "add_ground");
            ImGui::SameLine();
            create_primitive_button(app, "##tb_cube", Icon::Cube, PrimitiveKind::Cube, "add_cube");
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();

            if (icon_button("##tb_undo", Icon::Undo, AV("undo"), false)) {
                std::string err;
                if (!app.undo(err)) {
                    push_error_console(app, "Undo failed", err);
                }
            }
            ImGui::SameLine();
            if (icon_button("##tb_redo", Icon::Redo, AV("redo"), false)) {
                std::string err;
                if (!app.redo(err)) {
                    push_error_console(app, "Redo failed", err);
                }
            }
            ImGui::SameLine();
            if (icon_button("##tb_settings", Icon::Settings, AV("settings"), st.show_settings)) {
                st.show_settings = !st.show_settings;
            }

            // Play/Stop sit on the right edge: the one pair whose state has to be
            // legible at a glance.
            const float play_x = ImGui::GetWindowWidth() - (ImGui::GetFrameHeight() + 30.0f);
            if (ImGui::GetCursorPosX() < play_x) {
                ImGui::SetCursorPosX(play_x);
            }
            if (!app.playing()) {
                if (icon_button("##tb_play", Icon::Play, AV("play"), false)) {
                    std::string err;
                    if (!app.play(err)) {
                        push_error_console(app, "Play failed", err);
                    }
                }
            } else if (icon_button("##tb_stop", Icon::Stop, AV("stop"), true)) {
                std::string err;
                if (!app.stop(err)) {
                    push_error_console(app, "Stop failed", err);
                }
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
// --- Settings window ------------------------------------------------------
    if (st.show_settings) {
        ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("settings") + "###NFSettings").c_str(), &st.show_settings)) {
            if (settings_section(AV("viewport"), true)) {
                ImGui::Checkbox(AV("grid_step").c_str(), &st.show_grid);
                ImGui::SetNextItemWidth(150.0f);
                // "label###id": the visible label the slider never had, with the
                // widget ID left as it was so any layout/automation that knows
                // this control keeps finding it. The grid SIZE was adjustable
                // but anonymous — the only unlabelled control in this window.
                ImGui::DragFloat((AV("grid_step_value") + "###grid_step").c_str(), &st.grid_step, 0.1f,
                                 0.1f, 10.0f, "%.2f");
                ImGui::Checkbox(AV("fps").c_str(), &st.show_fps);
                ImGui::Checkbox(AV("validation").c_str(), &st.show_validation);
                // Everything the View menu toggles is a preference, so the
                // settings window carries the same set — a user who only ever
                // opens Settings must not miss a switch that exists.
                ImGui::Checkbox(AV("profiler").c_str(), &st.show_profiler);
            }

            if (settings_section(AV("snap"))) {
                if (ImGui::Checkbox(AV("snap").c_str(), &st.snap_enabled)) {
                    sync_snap_from_settings(app, st);
                }
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::DragFloat(AV("move").c_str(), &st.snap_move, 0.05f, 0.0f, 10.0f, "%.2f") &&
                    st.snap_enabled) {
                    sync_snap_from_settings(app, st);
                }
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::DragFloat(AV("rotate").c_str(), &st.snap_rotate, 1.0f, 0.0f, 90.0f, "%.0f") &&
                    st.snap_enabled) {
                    sync_snap_from_settings(app, st);
                }
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::DragFloat(AV("scale_tool").c_str(), &st.snap_scale, 0.05f, 0.0f, 2.0f,
                                     "%.2f") &&
                    st.snap_enabled) {
                    sync_snap_from_settings(app, st);
                }
            }

            if (settings_section(AV("sky_preset"))) {
                // The labels must OUTLIVE the Combo call. AV() returns a
                // std::string by value, so taking .c_str() of the temporary left
                // all three pointers dangling at the end of that same statement
                // and the dropdown painted garbage (the "????" boxes). Keep the
                // owning strings alive in this scope, then point at them.
                const std::string preset_labels[3] = {AV("sky_day"), AV("sky_sunset"),
                                                      AV("sky_night")};
                const char* presets[3] = {preset_labels[0].c_str(), preset_labels[1].c_str(),
                                          preset_labels[2].c_str()};
                int preset = st.sky_preset;
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo(AV("sky_preset").c_str(), &preset, presets, 3)) {
                    st.sky_preset = preset;
                    apply_sky_preset(app, preset);
                }
            }

            if (settings_section(AV("rendering"))) {
                // Render preferences are COMPONENT state, not renderer state:
                // Runtime::renderer() is const, so the editor cannot write global
                // renderer settings. What it can write is the same values the
                // renderer reads — the shadow parameters of the scene's
                // directional light (scene_light_entity picks the first, which is
                // the one that is drawn). The Inspector edits the SELECTED light;
                // a user who never selects it had no way to reach shadows at all,
                // which is the gap the lead's list names. One Apply = one undo
                // step, exactly like the Inspector's light section.
                const ecs::World* world = app.world();
                const ecs::Entity light =
                    (world != nullptr) ? scene_light_entity(*world) : ecs::kInvalidEntity;
                if (!light.valid()) {
                    ImGui::TextDisabled("%s", AV("no_light_settings").c_str());
                } else {
                    bool has_light = false;
                    LightEdit le = read_light(*world, light, has_light);
                    ImGui::SetNextItemWidth(200.0f);
                    ImGui::SliderFloat(AV("shadow_strength").c_str(), &le.shadow_strength, 0.0f, 1.0f);
                    ImGui::SetNextItemWidth(200.0f);
                    ImGui::DragFloat(AV("shadow_bias").c_str(), &le.shadow_bias, 0.00005f, 0.0f,
                                     0.01f, "%.5f");
                    ImGui::SetNextItemWidth(200.0f);
                    // Cascades are whole tiles of the atlas, so the only
                    // meaningful values are 1..kMaxShadowCascades — a slider,
                    // not a drag, same range as the Inspector.
                    ImGui::SliderInt(AV("shadow_cascades").c_str(), &le.shadow_cascades, 1,
                                     static_cast<int>(rendering::kMaxShadowCascades));
                    if (ImGui::Button((AV("apply") + "##rendering").c_str())) {
                        std::string err;
                        if (!app.set_light(light, le, err)) {
                            push_error_console(app, "Shadow settings failed", err);
                        }
                    }
                }
            }

            if (settings_section(AV("language"))) {
                // Same flip as the Settings menu item: the label names the
                // language NOT active, because that is what the click does. The
                // language's own name is deliberate Latin where it is "English"
                // (a language name is shown in its own script); the Arabic side
                // goes through AV like every other display string.
                const bool arabic = (ui::current_language() == ui::Language::Arabic);
                int lang = arabic ? 1 : 0;
                const std::string en_label = "English";
                const std::string ar_label = AV("arabic_name");
                ImGui::RadioButton(en_label.c_str(), &lang, 0);
                ImGui::SameLine();
                ImGui::RadioButton(ar_label.c_str(), &lang, 1);
                if (lang != (arabic ? 1 : 0)) {
                    ui::set_language(lang == 1 ? ui::Language::Arabic : ui::Language::English);
                    save_language(lang == 1); // persist the flip (shell settings file)
                }
            }

            if (settings_section(AV("autosave"))) {
                // The same session value the Game menu edits (autosave_interval()),
                // so flipping it in either place is visible in both.
                bool autosave = app.autosave_enabled();
                if (ImGui::Checkbox(AV("autosave").c_str(), &autosave)) {
                    app.set_autosave(autosave ? autosave_interval() : 0.0f, "autosave_");
                }
                if (autosave) {
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::DragFloat(AV("autosave_interval").c_str(), &autosave_interval(),
                                         5.0f, 5.0f, 3600.0f, "%.0f")) {
                        app.set_autosave(autosave_interval(), "autosave_");
                    }
                }
            }

            if (settings_section(AV("theme_accent"))) {
                float col[3] = {st.accent_r, st.accent_g, st.accent_b};
                if (ImGui::ColorEdit3(AV("theme_accent").c_str(), col)) {
                    st.accent_r = col[0];
                    st.accent_g = col[1];
                    st.accent_b = col[2];
                    apply_ui_accent(st);
                }
            }

            ImGui::Separator();
            if (ImGui::Button(AV("close").c_str(), ImVec2(120.0f, 0.0f))) {
                st.show_settings = false;
            }
        }
        ImGui::End();
    }
// --- Import dialog --------------------------------------------------------
    if (st.show_import) {
        ImGui::SetNextWindowSize(ImVec2(580.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("import_model") + "###NFImport").c_str(), &st.show_import)) {
            static char src[MAX_PATH * 2] = {};
            static char dst[128] = "content://Meshes";
            ImGui::TextDisabled("%s", AV("import_source").c_str());
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::InputText("###src", src, sizeof(src));
            ImGui::SameLine();
            if (ImGui::Button(AV("browse").c_str())) {
                std::string picked = src;
                if (open_file_dialog(AV("import_model"), import_filter(), picked)) {
                    std::snprintf(src, sizeof(src), "%s", picked.c_str());
                }
            }
            ImGui::SetNextItemWidth(300.0f);
            ImGui::InputText("###dst", dst, sizeof(dst));
            ImGui::SameLine();
            ImGui::TextDisabled("content://");
            ImGui::Separator();
            ImGui::BeginDisabled(src[0] == '\0');
            if (ImGui::Button(AV("import_model").c_str(), ImVec2(160.0f, 0.0f))) {
                size_t job = 0;
                std::string err;
                if (!app.import_file(src, dst, true, job, err)) {
                    push_error_console(app, "Import failed", err);
                } else {
                    push_info_console(app,
                                      std::string("Queued import #") + std::to_string(job) + ": " + src);
                    st.show_import = false;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(AV("cancel").c_str(), ImVec2(120.0f, 0.0f))) {
                st.show_import = false;
            }

            // Queue state: the dialog reports progress instead of being a
            // fire-and-forget button, which is what the import work needed.
            const auto& jobs = app.import_queue().jobs();
            if (!jobs.empty()) {
                ImGui::Separator();
                ImGui::Text("%zu %s", jobs.size(), AV("assets").c_str());
                ImGui::BeginChild("##imports", ImVec2(0.0f, 130.0f), true);
                for (const ImportJob& job : jobs) {
                    // Job states are user-facing (import dialog), so they
                    // translate like every other widget label.
                    const char* state_key = "import_state_queued";
                    switch (job.state) {
                        case ImportJob::State::Working: state_key = "import_state_working"; break;
                        case ImportJob::State::Failed: state_key = "import_state_failed"; break;
                        case ImportJob::State::Done: state_key = "import_state_done"; break;
                        case ImportJob::State::Queued: break;
                    }
                    const std::string state = AV(state_key);
                    ImGui::Text("%s  %.0f%%  %s", job.dst_logical.c_str(),
                                static_cast<double>(job.progress) * 100.0, state.c_str());
                    if (!job.error.empty()) {
                        ImGui::TextDisabled("   %s", job.error.c_str());
                    }
                }
                ImGui::EndChild();
                if (ImGui::Button(AV("clear_imports").c_str())) {
                    app.import_queue().clear_finished();
                }
            }
        }
        ImGui::End();
    }
// --- Export dialog --------------------------------------------------------
    if (st.show_export) {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("export") + "###NFExport").c_str(), &st.show_export)) {
            static char path[MAX_PATH * 2] = {};
            const std::vector<assets::MeshFormat>& formats = assets::mesh_formats();
            st.export_format = std::clamp(st.export_format, 0, static_cast<int>(formats.size()) - 1);
            const assets::MeshFormat fmt = formats[static_cast<size_t>(st.export_format)];

            ImGui::TextWrapped("%s", AV("export_hint").c_str());
            ImGui::Separator();

            // Same dangling-temporary trap as the sky combo: AV() returns a
            // temporary std::string, so its .c_str() must not be stored past the
            // statement. Own the labels first.
            const std::string scope_labels[2] = {AV("export_selection"), AV("export_whole_scene")};
            const char* scope[] = {scope_labels[0].c_str(), scope_labels[1].c_str()};
            int scope_index = st.export_whole_scene ? 1 : 0;
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo(AV("export_scope").c_str(), &scope_index, scope, 2)) {
                st.export_whole_scene = (scope_index == 1);
            }

            std::vector<const char*> names;
            names.reserve(formats.size());
            for (const assets::MeshFormat f : formats) {
                names.push_back(assets::mesh_format_name(f));
            }
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::Combo(AV("export_format").c_str(), &st.export_format, names.data(),
                             static_cast<int>(names.size()))) {
                // Changing the format rewrites the extension, so a .glb is never
                // written into a file the user named .obj.
                std::string base(path);
                const size_t dot = base.find_last_of('.');
                if (dot != std::string::npos && dot > 0) {
                    base.erase(dot);
                }
                if (base.empty()) {
                    base = suggested_export_name(app);
                }
                std::snprintf(path, sizeof(path), "%s.%s", base.c_str(),
                              assets::mesh_format_extension(fmt));
            }
            if (path[0] == '\0') {
                const std::string base = suggested_export_name(app);
                std::snprintf(path, sizeof(path), "%s.%s", base.c_str(),
                              assets::mesh_format_extension(fmt));
            }

            ImGui::TextDisabled("%s", AV("output_path").c_str());
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::InputText("###export_path", path, sizeof(path));
            ImGui::SameLine();
            if (ImGui::Button(AV("browse").c_str())) {
                std::string picked = path;
                if (save_file_dialog(AV("export"), export_filter(),
                                     assets::mesh_format_extension(fmt), picked)) {
                    std::snprintf(path, sizeof(path), "%s", picked.c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::Button(AV("export").c_str(), ImVec2(160.0f, 0.0f))) {
                std::string err;
                const size_t n = app.export_meshes_to_file(path, fmt, st.export_whole_scene, err);
                if (n == 0) {
                    push_error_console(app, "Export failed", err);
                } else {
                    push_info_console(app, "Exported " + std::to_string(n) + " mesh(es) to " + path);
                    st.show_export = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(AV("cancel").c_str(), ImVec2(120.0f, 0.0f))) {
                st.show_export = false;
            }
        }
        ImGui::End();
    }

    // --- About ----------------------------------------------------------------
    if (st.show_about) {
        ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((AV("about") + "###NFAbout").c_str(), &st.show_about)) {
            // Compose LOGICAL then shape once: concatenating two already-shaped
            // strings would freeze the LTR order instead of the visual RTL one.
            rt_colored_str(ImVec4(1.0f, 0.62f, 0.15f, 1.0f),
                           ui::shape_arabic(ui::tr("engine_name") + " - " + ui::tr("engine_name")));
            rt_text_str(ui::shape_arabic(ui::tr("founder") + ": " + ui::tr("founder")));
            rt_wrapped_key("about_tagline");
            ImGui::Separator();
            rt_text_str(ui::shape_arabic(
                ui::tr("project") + ": " +
                (app.has_project() ? app.project_name() : ui::tr("about_engine_tree"))));
            char counts[128] = {};
            std::snprintf(counts, sizeof(counts), "%zu %s   %zu/%zu", app.status().entity_count,
                          ui::tr("entities").c_str(), app.status().selected_count,
                          app.status().entity_count);
            rt_text_str(ui::shape_arabic(counts));
            ImGui::Separator();
            if (ImGui::Button(AV("close").c_str(), ImVec2(120.0f, 0.0f))) {
                st.show_about = false;
            }
        }
        ImGui::End();
    }
}

} // namespace nf::editor