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
#include <NF/Editor/UiTheme.hpp>
#include <NF/Editor/ProjectLauncher.hpp> // save_language() for the live toggles

#include <NF/Assets/MeshExport.hpp>
#include <NF/Core/Profiler.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/Runtime/Runtime.hpp>
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
    if (dl == nullptr || s <= 0.0f) {
        return;
    }
    const float t = std::max(1.35f, s * 0.082f); // responsive line thickness
    const float h = s * 0.5f;                    // half extent
    const float d = s * 0.16f;                   // arrowhead extent

    // Shading helpers derived from input color: adapts dynamically to idle / hover / active
    const ImVec4 c4 = ImGui::ColorConvertU32ToFloat4(col);
    auto make_shade = [&](float rgb_scale, float a_scale) -> ImU32 {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(
            std::min(1.0f, c4.x * rgb_scale),
            std::min(1.0f, c4.y * rgb_scale),
            std::min(1.0f, c4.z * rgb_scale),
            std::min(1.0f, c4.w * a_scale)));
    };
    const ImU32 col_subtle = make_shade(1.0f, 0.20f);
    const ImU32 col_mid = make_shade(0.78f, 0.85f);
    const ImU32 col_dark = make_shade(0.48f, 0.90f);
    const ImU32 col_bright = make_shade(1.22f, 1.0f);

    switch (icon) {
        case Icon::New: {
            // Document with folded top-right corner and interior lines
            const float f = s * 0.26f;
            const ImVec2 p0(c.x - h * 0.65f, c.y - h * 0.90f);
            const ImVec2 p1(c.x + h * 0.65f - f, c.y - h * 0.90f);
            const ImVec2 p2(c.x + h * 0.65f, c.y - h * 0.90f + f);
            const ImVec2 p3(c.x + h * 0.65f, c.y + h * 0.90f);
            const ImVec2 p4(c.x - h * 0.65f, c.y + h * 0.90f);

            // Shaded sheet background
            dl->AddQuadFilled(p0, p1, ImVec2(c.x - h * 0.65f, c.y - h * 0.90f + f), p4, col_subtle);
            dl->AddQuadFilled(ImVec2(c.x - h * 0.65f, c.y - h * 0.90f + f), p2, p3, p4, col_subtle);

            // Outer contour
            dl->AddLine(p0, p1, col, t);
            dl->AddLine(p2, p3, col, t);
            dl->AddLine(p3, p4, col, t);
            dl->AddLine(p4, p0, col, t);

            // Fold flap (triangle)
            const ImVec2 f_inner(c.x + h * 0.65f - f, c.y - h * 0.90f + f);
            dl->AddTriangleFilled(p1, p2, f_inner, col_mid);
            dl->AddLine(p1, f_inner, col, t * 0.85f);
            dl->AddLine(f_inner, p2, col, t * 0.85f);

            // Interior document text lines
            dl->AddLine(ImVec2(c.x - h * 0.35f, c.y - h * 0.10f),
                        ImVec2(c.x + h * 0.35f, c.y - h * 0.10f), col, t * 0.85f);
            dl->AddLine(ImVec2(c.x - h * 0.35f, c.y + h * 0.25f),
                        ImVec2(c.x + h * 0.35f, c.y + h * 0.25f), col, t * 0.85f);
            dl->AddLine(ImVec2(c.x - h * 0.35f, c.y + h * 0.60f),
                        ImVec2(c.x + h * 0.05f, c.y + h * 0.60f), col, t * 0.85f);
            break;
        }
        case Icon::Open: {
            // Folder: Back flap with tab + tilted 3D front pocket
            dl->AddRectFilled(ImVec2(c.x - h * 0.85f, c.y - h * 0.85f),
                              ImVec2(c.x - h * 0.15f, c.y - h * 0.45f), col_mid, 1.5f);
            dl->AddRectFilled(ImVec2(c.x - h * 0.85f, c.y - h * 0.55f),
                              ImVec2(c.x + h * 0.85f, c.y + h * 0.70f), col_subtle, 1.5f);
            dl->AddRect(ImVec2(c.x - h * 0.85f, c.y - h * 0.55f),
                        ImVec2(c.x + h * 0.85f, c.y + h * 0.70f), col, 1.5f, 0, t * 0.85f);

            // Front pocket tilted forward (isometric 3D perspective)
            const ImVec2 f1(c.x - h * 0.90f, c.y - h * 0.15f);
            const ImVec2 f2(c.x + h * 0.70f, c.y - h * 0.15f);
            const ImVec2 f3(c.x + h * 0.88f, c.y + h * 0.75f);
            const ImVec2 f4(c.x - h * 0.72f, c.y + h * 0.75f);
            dl->AddQuadFilled(f1, f2, f3, f4, col_dark);
            dl->AddQuad(f1, f2, f3, f4, col, t);
            break;
        }
        case Icon::Save: {
            // Floppy disk: 3.5" diskette with metallic shutter and label
            dl->AddRectFilled(ImVec2(c.x - h * 0.80f, c.y - h * 0.85f),
                              ImVec2(c.x + h * 0.80f, c.y + h * 0.85f), col_subtle, 2.0f);
            dl->AddRect(ImVec2(c.x - h * 0.80f, c.y - h * 0.85f),
                        ImVec2(c.x + h * 0.80f, c.y + h * 0.85f), col, 2.0f, 0, t);

            // Shutter slider at top
            dl->AddRectFilled(ImVec2(c.x - h * 0.45f, c.y - h * 0.85f),
                              ImVec2(c.x + h * 0.45f, c.y - h * 0.20f), col_mid, 1.0f);
            dl->AddRect(ImVec2(c.x - h * 0.45f, c.y - h * 0.85f),
                        ImVec2(c.x + h * 0.45f, c.y - h * 0.20f), col, 1.0f, 0, t * 0.85f);
            // Shutter cutout window
            dl->AddRectFilled(ImVec2(c.x - h * 0.18f, c.y - h * 0.70f),
                              ImVec2(c.x + h * 0.12f, c.y - h * 0.35f), col_dark, 0.5f);

            // Label at bottom
            dl->AddRectFilled(ImVec2(c.x - h * 0.55f, c.y + h * 0.15f),
                              ImVec2(c.x + h * 0.55f, c.y + h * 0.70f), col_bright, 1.0f);
            dl->AddLine(ImVec2(c.x - h * 0.38f, c.y + h * 0.35f),
                        ImVec2(c.x + h * 0.38f, c.y + h * 0.35f), col_dark, t * 0.75f);
            dl->AddLine(ImVec2(c.x - h * 0.38f, c.y + h * 0.52f),
                        ImVec2(c.x + h * 0.18f, c.y + h * 0.52f), col_dark, t * 0.75f);
            break;
        }
        case Icon::Play: {
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.60f, c.y - h * 0.82f),
                                  ImVec2(c.x - h * 0.60f, c.y + h * 0.82f),
                                  ImVec2(c.x + h * 0.85f, c.y), col);
            break;
        }
        case Icon::Stop: {
            dl->AddRectFilled(ImVec2(c.x - h * 0.75f, c.y - h * 0.75f),
                              ImVec2(c.x + h * 0.75f, c.y + h * 0.75f), col, 2.0f);
            break;
        }
        case Icon::Move: {
            // 4-way translation cross with center anchor
            dl->AddLine(ImVec2(c.x - h * 0.88f, c.y), ImVec2(c.x + h * 0.88f, c.y), col, t * 1.1f);
            dl->AddLine(ImVec2(c.x, c.y - h * 0.88f), ImVec2(c.x, c.y + h * 0.88f), col, t * 1.1f);
            // Sharp arrowheads
            dl->AddTriangleFilled(ImVec2(c.x + h, c.y), ImVec2(c.x + h - d * 1.1f, c.y - d * 0.75f),
                                  ImVec2(c.x + h - d * 1.1f, c.y + d * 0.75f), col);
            dl->AddTriangleFilled(ImVec2(c.x - h, c.y), ImVec2(c.x - h + d * 1.1f, c.y - d * 0.75f),
                                  ImVec2(c.x - h + d * 1.1f, c.y + d * 0.75f), col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - h), ImVec2(c.x - d * 0.75f, c.y - h + d * 1.1f),
                                  ImVec2(c.x + d * 0.75f, c.y - h + d * 1.1f), col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + h), ImVec2(c.x - d * 0.75f, c.y + h - d * 1.1f),
                                  ImVec2(c.x + d * 0.75f, c.y + h - d * 1.1f), col);
            // Center ring
            dl->AddCircleFilled(c, s * 0.10f, col);
            break;
        }
        case Icon::Rotate: {
            // Sweeping 270-degree rotation arc with sharp arrowhead and pivot center
            dl->PathArcTo(c, h * 0.78f, 0.28f * kPi, 1.82f * kPi, 28);
            dl->PathStroke(col, 0, t * 1.15f);
            const ImVec2 tip(c.x + h * 0.78f * std::cos(0.28f * kPi),
                             c.y + h * 0.78f * std::sin(0.28f * kPi));
            const float a = s * 0.22f;
            dl->AddTriangleFilled(ImVec2(tip.x + a * 1.05f, tip.y - a * 0.15f),
                                  ImVec2(tip.x - a * 0.55f, tip.y - a * 0.95f),
                                  ImVec2(tip.x + a * 0.15f, tip.y + a * 0.95f), col);
            dl->AddCircleFilled(c, s * 0.08f, col_mid);
            break;
        }
        case Icon::Scale: {
            // Dual box scale: small box at bottom-left, large box at top-right, diagonal arrow
            const ImVec2 sm_min(c.x - h * 0.85f, c.y + h * 0.20f);
            const ImVec2 sm_max(c.x - h * 0.25f, c.y + h * 0.80f);
            dl->AddRectFilled(sm_min, sm_max, col_subtle, 1.0f);
            dl->AddRect(sm_min, sm_max, col, 1.0f, 0, t * 0.85f);

            const ImVec2 lg_min(c.x - h * 0.15f, c.y - h * 0.80f);
            const ImVec2 lg_max(c.x + h * 0.85f, c.y + h * 0.20f);
            dl->AddRectFilled(lg_min, lg_max, col_mid, 1.0f);
            dl->AddRect(lg_min, lg_max, col, 1.0f, 0, t * 0.85f);

            // Diagonal connecting arrow
            const ImVec2 a_start(c.x - h * 0.10f, c.y + h * 0.10f);
            const ImVec2 a_end(c.x + h * 0.50f, c.y - h * 0.50f);
            dl->AddLine(a_start, a_end, col, t * 1.1f);
            dl->AddTriangleFilled(ImVec2(a_end.x + s * 0.05f, a_end.y - s * 0.05f),
                                  ImVec2(a_end.x - s * 0.18f, a_end.y),
                                  ImVec2(a_end.x, a_end.y + s * 0.18f), col);
            break;
        }
        case Icon::Snap: {
            // Horseshoe magnet with distinct North/South pole tips
            const float u_r = h * 0.55f;
            const float u_thick = s * 0.16f;
            // Arc at top
            dl->PathArcTo(ImVec2(c.x, c.y - h * 0.15f), u_r, 1.0f * kPi, 2.0f * kPi, 18);
            dl->PathLineTo(ImVec2(c.x + u_r, c.y + h * 0.65f));
            dl->PathLineTo(ImVec2(c.x + u_r - u_thick, c.y + h * 0.65f));
            dl->PathArcTo(ImVec2(c.x, c.y - h * 0.15f), u_r - u_thick, 2.0f * kPi, 1.0f * kPi, 18);
            dl->PathLineTo(ImVec2(c.x - u_r, c.y + h * 0.65f));
            dl->PathFillConvex(col_mid);

            // Outer stroke
            dl->PathArcTo(ImVec2(c.x, c.y - h * 0.15f), u_r, 1.0f * kPi, 2.0f * kPi, 18);
            dl->PathLineTo(ImVec2(c.x + u_r, c.y + h * 0.65f));
            dl->PathLineTo(ImVec2(c.x + u_r - u_thick, c.y + h * 0.65f));
            dl->PathArcTo(ImVec2(c.x, c.y - h * 0.15f), u_r - u_thick, 2.0f * kPi, 1.0f * kPi, 18);
            dl->PathLineTo(ImVec2(c.x - u_r, c.y + h * 0.65f));
            dl->PathStroke(col, ImDrawFlags_Closed, t * 0.85f);

            // Pole tips at bottom
            dl->AddRectFilled(ImVec2(c.x - u_r, c.y + h * 0.38f),
                              ImVec2(c.x - u_r + u_thick, c.y + h * 0.65f), col_bright, 0.5f);
            dl->AddRectFilled(ImVec2(c.x + u_r - u_thick, c.y + h * 0.38f),
                              ImVec2(c.x + u_r, c.y + h * 0.65f), col_bright, 0.5f);
            break;
        }
        case Icon::Ground: {
            // True 3D perspective ground plane with converging grid lines & horizon
            dl->AddLine(ImVec2(c.x - h * 0.95f, c.y - h * 0.40f),
                        ImVec2(c.x + h * 0.95f, c.y - h * 0.40f), col_dark, t * 0.75f);

            const ImVec2 tl(c.x - h * 0.48f, c.y - h * 0.30f);
            const ImVec2 tr(c.x + h * 0.48f, c.y - h * 0.30f);
            const ImVec2 br(c.x + h * 0.95f, c.y + h * 0.75f);
            const ImVec2 bl(c.x - h * 0.95f, c.y + h * 0.75f);
            dl->AddQuadFilled(tl, tr, br, bl, col_subtle);
            dl->AddQuad(tl, tr, br, bl, col, t);

            // Perspective converging lines
            dl->AddLine(ImVec2(c.x, c.y - h * 0.30f), ImVec2(c.x, c.y + h * 0.75f), col, t * 0.85f);
            dl->AddLine(ImVec2(c.x - h * 0.24f, c.y - h * 0.30f),
                        ImVec2(c.x - h * 0.50f, c.y + h * 0.75f), col_mid, t * 0.75f);
            dl->AddLine(ImVec2(c.x + h * 0.24f, c.y - h * 0.30f),
                        ImVec2(c.x + h * 0.50f, c.y + h * 0.75f), col_mid, t * 0.75f);

            // Horizontal perspective lines
            dl->AddLine(ImVec2(c.x - h * 0.70f, c.y + h * 0.20f),
                        ImVec2(c.x + h * 0.70f, c.y + h * 0.20f), col_mid, t * 0.75f);
            break;
        }
        case Icon::Cube: {
            // Isometric 3D shaded cube (top light, left mid, right shadow)
            const ImVec2 top_pt(c.x, c.y - h * 0.95f);
            const ImVec2 right_pt(c.x + h * 0.85f, c.y - h * 0.45f);
            const ImVec2 mid_pt(c.x, c.y + h * 0.05f);
            const ImVec2 left_pt(c.x - h * 0.85f, c.y - h * 0.45f);
            const ImVec2 bot_pt(c.x, c.y + h * 0.95f);
            const ImVec2 bot_left(c.x - h * 0.85f, c.y + h * 0.45f);
            const ImVec2 bot_right(c.x + h * 0.85f, c.y + h * 0.45f);

            // Shaded faces
            dl->AddQuadFilled(top_pt, right_pt, mid_pt, left_pt, col_bright);
            dl->AddQuadFilled(left_pt, mid_pt, bot_pt, bot_left, col_mid);
            dl->AddQuadFilled(mid_pt, right_pt, bot_right, bot_pt, col_dark);

            // Crisp outlines
            dl->AddQuad(top_pt, right_pt, mid_pt, left_pt, col, t * 0.85f);
            dl->AddQuad(left_pt, mid_pt, bot_pt, bot_left, col, t * 0.85f);
            dl->AddQuad(mid_pt, right_pt, bot_right, bot_pt, col, t * 0.85f);
            break;
        }
        case Icon::Sun: {
            // Luminous sun: filled core + glow ring + 8 tapered radial rays
            dl->AddCircleFilled(c, h * 0.40f, col, 20);
            dl->AddCircle(c, h * 0.55f, col_subtle, 20, t * 0.8f);
            for (int i = 0; i < 8; ++i) {
                const float ang = (static_cast<float>(i) / 8.0f) * 2.0f * kPi;
                dl->AddLine(ImVec2(c.x + std::cos(ang) * h * 0.62f, c.y + std::sin(ang) * h * 0.62f),
                            ImVec2(c.x + std::cos(ang) * h * 0.95f, c.y + std::sin(ang) * h * 0.95f),
                            col, t * 1.1f);
            }
            break;
        }
        case Icon::Camera: {
            // Cinema camera: rounded body + film reels + lens cone
            dl->AddRectFilled(ImVec2(c.x - h * 0.85f, c.y - h * 0.45f),
                              ImVec2(c.x + h * 0.35f, c.y + h * 0.55f), col, 2.0f);
            // Lens cone
            dl->AddTriangleFilled(ImVec2(c.x + h * 0.35f, c.y),
                                  ImVec2(c.x + h * 0.95f, c.y - h * 0.48f),
                                  ImVec2(c.x + h * 0.95f, c.y + h * 0.48f), col);
            // Dual film reels on top
            dl->AddCircleFilled(ImVec2(c.x - h * 0.45f, c.y - h * 0.65f), h * 0.24f, col_bright, 14);
            dl->AddCircle(ImVec2(c.x - h * 0.45f, c.y - h * 0.65f), h * 0.24f, col, 14, t * 0.8f);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.05f, c.y - h * 0.65f), h * 0.24f, col_bright, 14);
            dl->AddCircle(ImVec2(c.x + h * 0.05f, c.y - h * 0.65f), h * 0.24f, col, 14, t * 0.8f);
            break;
        }
        case Icon::Sky: {
            // Fluffy cloud with flat base and 3 soft puffs
            const float r = s * 0.26f;
            dl->AddRectFilled(ImVec2(c.x - r * 1.6f, c.y + r * 0.1f),
                              ImVec2(c.x + r * 1.9f, c.y + r * 0.95f), col, 3.0f);
            dl->AddCircleFilled(ImVec2(c.x - r * 0.70f, c.y + r * 0.15f), r * 0.85f, col, 16);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.35f, c.y - r * 0.20f), r * 1.15f, col, 18);
            dl->AddCircleFilled(ImVec2(c.x + r * 1.35f, c.y + r * 0.20f), r * 0.75f, col, 14);
            break;
        }
        case Icon::Import: {
            // Open receiving dock + downward arrow
            dl->AddLine(ImVec2(c.x - h * 0.85f, c.y + h * 0.30f),
                        ImVec2(c.x - h * 0.85f, c.y + h * 0.80f), col, t);
            dl->AddLine(ImVec2(c.x - h * 0.85f, c.y + h * 0.80f),
                        ImVec2(c.x + h * 0.85f, c.y + h * 0.80f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.85f, c.y + h * 0.80f),
                        ImVec2(c.x + h * 0.85f, c.y + h * 0.30f), col, t);
            // Arrow pointing down into dock
            dl->AddLine(ImVec2(c.x, c.y - h * 0.90f), ImVec2(c.x, c.y + h * 0.40f), col, t * 1.15f);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + h * 0.55f),
                                  ImVec2(c.x - s * 0.20f, c.y + h * 0.05f),
                                  ImVec2(c.x + s * 0.20f, c.y + h * 0.05f), col);
            break;
        }
        case Icon::Export: {
            // Open dock + upward launch arrow
            dl->AddLine(ImVec2(c.x - h * 0.85f, c.y + h * 0.30f),
                        ImVec2(c.x - h * 0.85f, c.y + h * 0.80f), col, t);
            dl->AddLine(ImVec2(c.x - h * 0.85f, c.y + h * 0.80f),
                        ImVec2(c.x + h * 0.85f, c.y + h * 0.80f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.85f, c.y + h * 0.80f),
                        ImVec2(c.x + h * 0.85f, c.y + h * 0.30f), col, t);
            // Arrow pointing up
            dl->AddLine(ImVec2(c.x, c.y + h * 0.40f), ImVec2(c.x, c.y - h * 0.50f), col, t * 1.15f);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - h * 0.85f),
                                  ImVec2(c.x - s * 0.20f, c.y - h * 0.25f),
                                  ImVec2(c.x + s * 0.20f, c.y - h * 0.25f), col);
            break;
        }
        case Icon::Settings: {
            // Mechanical cog gear: outer ring with 6 sturdy gear teeth & cutout center
            const float r_outer = h * 0.90f;
            const float r_inner = h * 0.60f;
            const float r_hole = h * 0.30f;
            for (int i = 0; i < 6; ++i) {
                const float ang = (static_cast<float>(i) / 6.0f) * 2.0f * kPi;
                dl->AddLine(ImVec2(c.x + std::cos(ang) * (r_inner - 1.0f),
                                   c.y + std::sin(ang) * (r_inner - 1.0f)),
                            ImVec2(c.x + std::cos(ang) * r_outer,
                                   c.y + std::sin(ang) * r_outer),
                            col, s * 0.26f);
            }
            dl->AddCircleFilled(c, r_inner, col, 20);
            dl->AddCircleFilled(c, r_hole, IM_COL32(26, 32, 44, 255), 16);
            dl->AddCircle(c, r_hole, col_mid, 16, t * 0.75f);
            break;
        }
        case Icon::Undo:
        case Icon::Redo: {
            const float dir = (icon == Icon::Undo) ? 1.0f : -1.0f;
            dl->PathArcTo(ImVec2(c.x, c.y + h * 0.25f), h * 0.75f, 1.10f * kPi, 1.90f * kPi, 24);
            dl->PathStroke(col, 0, t * 1.2f);
            const ImVec2 tip(c.x - dir * h * 0.75f, c.y + h * 0.22f);
            dl->AddTriangleFilled(ImVec2(tip.x, tip.y + s * 0.22f),
                                  ImVec2(tip.x + dir * s * 0.22f, tip.y - s * 0.05f),
                                  ImVec2(tip.x - dir * s * 0.22f, tip.y + s * 0.32f), col);
            break;
        }
        case Icon::Delete: {
            // Modern waste bin: tapered body with interior ribs & handle lid
            const ImVec2 p1(c.x - h * 0.60f, c.y - h * 0.35f);
            const ImVec2 p2(c.x + h * 0.60f, c.y - h * 0.35f);
            const ImVec2 p3(c.x + h * 0.48f, c.y + h * 0.85f);
            const ImVec2 p4(c.x - h * 0.48f, c.y + h * 0.85f);
            dl->AddQuadFilled(p1, p2, p3, p4, col_subtle);
            dl->AddQuad(p1, p2, p3, p4, col, t);

            // Vertical ribs
            dl->AddLine(ImVec2(c.x - h * 0.20f, c.y - h * 0.20f),
                        ImVec2(c.x - h * 0.16f, c.y + h * 0.70f), col, t * 0.75f);
            dl->AddLine(ImVec2(c.x + h * 0.20f, c.y - h * 0.20f),
                        ImVec2(c.x + h * 0.16f, c.y + h * 0.70f), col, t * 0.75f);

            // Lid & handle
            dl->AddLine(ImVec2(c.x - h * 0.80f, c.y - h * 0.38f),
                        ImVec2(c.x + h * 0.80f, c.y - h * 0.38f), col, t * 1.15f);
            dl->AddRect(ImVec2(c.x - h * 0.25f, c.y - h * 0.65f),
                        ImVec2(c.x + h * 0.25f, c.y - h * 0.38f), col, 1.0f, 0, t * 0.85f);
            break;
        }
    }
}

/// Draws the SANAD brand emblem (dark rounded badge + stylized geometric S
/// wings and golden/accent core ribbon, matching the app icon and logo).
void draw_brand_badge(ImDrawList* dl, ImVec2 p, float size) {
    const ImVec2 b(p.x + size, p.y + size);
    const float r = size * 0.22f;
    dl->AddRectFilled(p, b, IM_COL32(24, 28, 36, 255), r);
    dl->AddRect(p, b, ImGui::GetColorU32(theme::alpha(theme::accent(), 0.65f)), r, 0, 1.2f);

    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float u = size * 0.34f;
    const float stroke = std::max(1.8f, size * 0.11f);
    const ImU32 wing_col = IM_COL32(200, 210, 226, 255);
    const ImU32 gold_col = IM_COL32(245, 172, 48, 255);

    // Upper wing (slate silver)
    dl->AddLine(ImVec2(c.x - u * 0.85f, c.y - u * 0.10f),
                ImVec2(c.x - u * 0.35f, c.y - u * 0.65f), wing_col, stroke);
    dl->AddLine(ImVec2(c.x - u * 0.35f, c.y - u * 0.65f),
                ImVec2(c.x + u * 0.85f, c.y - u * 0.85f), wing_col, stroke);
    // Center S-ribbon (warm gold)
    dl->AddLine(ImVec2(c.x + u * 0.75f, c.y - u * 0.40f),
                ImVec2(c.x - u * 0.75f, c.y + u * 0.40f), gold_col, stroke * 1.1f);
    // Lower wing (slate silver)
    dl->AddLine(ImVec2(c.x - u * 0.85f, c.y + u * 0.85f),
                ImVec2(c.x + u * 0.35f, c.y + u * 0.65f), wing_col, stroke);
    dl->AddLine(ImVec2(c.x + u * 0.35f, c.y + u * 0.65f),
                ImVec2(c.x + u * 0.85f, c.y + u * 0.10f), wing_col, stroke);
}

/// An icon button. `id` keeps widget identity stable while the label changes
/// with the language; the tooltip carries the translated, shaped name.
bool icon_button(const char* id, Icon icon, const std::string& tooltip, bool active,
                 float size = 28.0f) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##btn", ImVec2(size, size));
    const bool clicked = ImGui::IsItemActivated() && ImGui::IsItemHovered();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg = 0;
    ImU32 border = 0;
    if (active) {
        bg = ImGui::GetColorU32(theme::alpha(theme::accent(), 0.28f));
        border = ImGui::GetColorU32(theme::accent());
    } else if (hovered) {
        bg = IM_COL32(46, 54, 70, 240);
        border = IM_COL32(75, 88, 114, 200);
    } else {
        bg = IM_COL32(31, 36, 47, 180);
        border = IM_COL32(44, 52, 68, 140);
    }

    const float rounding = 4.0f;
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), bg, rounding);
    dl->AddRect(p, ImVec2(p.x + size, p.y + size), border, rounding, 0, active ? 1.5f : 1.0f);

    const ImU32 fg = active ? ImGui::GetColorU32(theme::accent())
                            : (hovered ? IM_COL32(250, 252, 255, 255)
                                       : ImGui::GetColorU32(theme::text_primary()));
    draw_icon(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.52f, icon, fg);
    if (!tooltip.empty() && hovered) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
    ImGui::PopID();
    return clicked;
}

/// Same button, text label: for actions whose meaning a glyph cannot carry
/// (Local/World is a state, not an icon).
bool text_toolbar_button(const char* id, const std::string& label, bool active,
                         float height = 28.0f) {
    ImGui::PushID(id);
    const ImVec2 text_sz = ImGui::CalcTextSize(label.c_str());
    const float w = text_sz.x + 20.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##btn", ImVec2(w, height));
    const bool clicked = ImGui::IsItemActivated() && ImGui::IsItemHovered();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg = 0;
    ImU32 border = 0;
    if (active) {
        bg = ImGui::GetColorU32(theme::alpha(theme::accent(), 0.28f));
        border = ImGui::GetColorU32(theme::accent());
    } else if (hovered) {
        bg = IM_COL32(46, 54, 70, 240);
        border = IM_COL32(75, 88, 114, 200);
    } else {
        bg = IM_COL32(31, 36, 47, 180);
        border = IM_COL32(44, 52, 68, 140);
    }

    const float rounding = 4.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), bg, rounding);
    dl->AddRect(p, ImVec2(p.x + w, p.y + height), border, rounding, 0, active ? 1.5f : 1.0f);

    const ImU32 fg = active ? ImGui::GetColorU32(theme::accent())
                            : (hovered ? IM_COL32(250, 252, 255, 255)
                                       : ImGui::GetColorU32(theme::text_primary()));
    const ImVec2 text_pos(p.x + (w - text_sz.x) * 0.5f, p.y + (height - text_sz.y) * 0.5f);
    dl->AddText(text_pos, fg, label.c_str());

    if (!label.empty() && hovered) {
        ImGui::SetTooltip("%s", label.c_str());
    }

    ImGui::PopID();
    return clicked;
}

void toolbar_divider(float btn_sz) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 18.0f;
    const float y0 = p.y + (btn_sz - h) * 0.5f;
    dl->AddLine(ImVec2(p.x + 5.0f, y0), ImVec2(p.x + 5.0f, y0 + h), IM_COL32(52, 60, 78, 200), 1.0f);
    ImGui::Dummy(ImVec2(11.0f, btn_sz));
    ImGui::SameLine();
}

bool play_button(EditorApp& app, float btn_sz = 28.0f) {
    const bool playing = app.playing();
    const float btn_w = 34.0f;
    ImGui::PushID("##tb_playstop");
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##btn", ImVec2(btn_w, btn_sz));
    const bool clicked = ImGui::IsItemActivated() && ImGui::IsItemHovered();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg = 0;
    ImU32 border = 0;
    ImU32 fg = 0;

    if (playing) {
        bg = hovered ? IM_COL32(195, 48, 56, 255) : IM_COL32(150, 36, 42, 220);
        border = IM_COL32(235, 80, 88, 255);
        fg = IM_COL32(255, 255, 255, 255);
    } else {
        bg = hovered ? IM_COL32(38, 128, 68, 255) : IM_COL32(28, 86, 48, 210);
        border = hovered ? IM_COL32(65, 195, 105, 255) : IM_COL32(48, 142, 78, 180);
        fg = hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(210, 250, 225, 255);
    }

    const float rounding = 4.0f;
    dl->AddRectFilled(p, ImVec2(p.x + btn_w, p.y + btn_sz), bg, rounding);
    dl->AddRect(p, ImVec2(p.x + btn_w, p.y + btn_sz), border, rounding, 0, 1.2f);

    draw_icon(dl, ImVec2(p.x + btn_w * 0.5f, p.y + btn_sz * 0.5f), btn_sz * 0.54f,
              playing ? Icon::Stop : Icon::Play, fg);

    if (hovered) {
        ImGui::SetTooltip("%s", playing ? AV("stop").c_str() : AV("play").c_str());
    }
    ImGui::PopID();

    if (clicked) {
        std::string err;
        if (playing) {
            if (!app.stop(err)) {
                push_error_console(app, "Stop failed", err);
            }
        } else {
            if (!app.launch_game(err)) {
                push_error_console(app, "Play failed", err);
            }
        }
    }
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
    f += "SANAD Scene (*.nfscene)\0*.nfscene";
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
    ImGui::Separator();
    if (ImGui::MenuItem(AV("add_weather").c_str())) {
        std::string err;
        if (!app.create_weather(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create weather failed", err);
        }
    }
    if (ImGui::MenuItem(AV("add_cable").c_str())) {
        std::string err;
        if (!app.create_cable(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create cable failed", err);
        }
    }
    if (ImGui::MenuItem(AV("add_rule_tile").c_str())) {
        std::string err;
        if (!app.create_rule_tile(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create rule tile failed", err);
        }
    }
    if (ImGui::MenuItem(AV("add_flow_field").c_str())) {
        std::string err;
        if (!app.create_flow_field(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create flow field failed", err);
        }
    }
    if (ImGui::MenuItem(AV("add_talent_tree").c_str())) {
        std::string err;
        if (!app.create_talent_tree(ecs::kInvalidEntity, err)) {
            push_error_console(app, "Create talent tree failed", err);
        }
    }
}
/// A panel-section header used by the settings window.
///
/// Takes the KEY, not the translated text, and appends `###sec_<key>`.
///
/// Both halves of that are fixes, not decoration:
///
///  * A section header and the first control inside it very often carry the SAME
///    label - "Snap" section, "Snap" checkbox; "Theme accent" section, "Theme
///    accent" colour picker. ImGui derives a widget's ID from its label, so that
///    pair is a duplicate-ID conflict, and it surfaces as a modal "MESSAGE FROM
///    DEAR IMGUI Programmer error" the user must dismiss before the editor is
///    usable again. Anchoring the ID to the key makes every header unique by
///    construction, no matter what the labels inside it turn out to be, so the
///    class of bug cannot come back through a new section.
///  * Deriving the ID from the TRANSLATED text would give one control two
///    different IDs across the two languages, so the open/closed state stored in
///    editor_layout.ini would silently not apply after a language switch.
///
/// Guarded by tests/EditorTests/test_imgui_id_uniqueness.cpp, because the failure
/// mode is invisible to every test that does not open this window with a GPU.
bool settings_section(const char* key, bool default_open = false) {
    const std::string label = AV(key) + "###sec_" + key;
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
            // Play = the real game in its own window (NFGamePlayer spawned
            // beside the editor). The in-viewport play session remains an
            // engine API for tests and automation, not a menu entry: a
            // viewport that "plays" while the editor keeps rendering it reads
            // as nothing happening — the exact complaint this replaces.
            if (!app.launch_game(err)) {
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

// The live accent colour, read by every panel that draws an accent-coloured
// element. It lives here, next to the settings object it reads, because
// UiTheme.hpp is included by files that must not depend on the settings at all
// — and this is the one function in that header that genuinely needs them.
//
// (This definition was destroyed by a `git checkout` of this file while chasing
// an unrelated UTF-8 corruption, and the link error it caused was masked by a
// stale-object link failure reporting exit 1120. Recorded here so the next
// person who reverts a file knows the settings-dependent definitions live in it,
// and so nobody "fixes" the link error by deleting the declaration.)
ImVec4 theme::accent() {
    const EditorUiSettings& s = ui_settings();
    return ImVec4(s.accent_r, s.accent_g, s.accent_b, 1.0f);
}

void apply_ui_accent(const EditorUiSettings& settings) {
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 accent(settings.accent_r, settings.accent_g, settings.accent_b, 1.0f);
    // Neutral controls, accent only on state. Same rule as apply_forge_theme:
    // the accent must be a highlight, never the fill of every button/header.
    style.Colors[ImGuiCol_Header] = theme::rgb(0x26, 0x30, 0x42);
    style.Colors[ImGuiCol_HeaderHovered] = theme::rgb(0x2E, 0x3A, 0x50);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    style.Colors[ImGuiCol_Button] = theme::rgb(0x2A, 0x2F, 0x37);
    style.Colors[ImGuiCol_ButtonHovered] = theme::rgb(0x35, 0x3C, 0x47);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.30f);
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
                       const char* shortcut_suffix, GizmoMode mode, float size = 28.0f) {
    const bool on = (app.gizmo_mode() == mode);
    if (icon_button(id, icon, AV(key) + shortcut_suffix, on, size)) {
        app.set_gizmo_mode(mode);
    }
}

/// Create-menu primitive as a toolbar button (asset cook + entity, one undo).
void create_primitive_button(EditorApp& app, const char* id, Icon icon, PrimitiveKind kind,
                             const char* key, float size = 28.0f) {
    if (icon_button(id, icon, AV(key), false, size)) {
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
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, IM_COL32(18, 21, 27, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(16.0f, 6.0f));
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(AV("file").c_str())) {
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
            if (ImGui::MenuItem(AV("recover_draft").c_str(), nullptr, false,
                                app.has_recovery_copy())) {
                std::string err;
                if (!app.recover_last_draft(err)) {
                    push_error_console(app, "Recover draft failed", err);
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

        if (ImGui::BeginMenu(AV("edit").c_str())) {
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
            if (ImGui::MenuItem(AV("duplicate").c_str(), "Ctrl+D", false, has_sel)) {
                std::string err;
                if (!app.duplicate_selection(err)) {
                    push_error_console(app, "Duplicate failed", err);
                }
            }
            if (ImGui::MenuItem(AV("save_prefab").c_str(), nullptr, false, has_sel)) {
                std::string err;
                const ecs::Entity sel = app.selection().primary();
                // The path comes from the entity's NAME, not from its id: the
                // ECS recycles ids, so a prefab named after one was
                // overwritten by the next entity that got the same id.
                const std::string path = app.prefab_path_for(sel);
                if (!app.create_prefab(sel, path, err)) {
                    push_error_console(app, "Save prefab failed", err);
                } else {
                    push_info_console(app, "Prefab saved: " + path);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(AV("view").c_str())) {
            ImGui::MenuItem(AV("grid_step").c_str(), nullptr, &st.show_grid);
            ImGui::MenuItem(AV("show_navmesh").c_str(), nullptr, &st.show_navmesh);
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
            ImGui::MenuItem(AV("viewport_stats").c_str(), nullptr, &st.show_viewport_stats);
            ImGui::Separator();
            // The rebinding window lives here because it is a VIEW on the
            // project's input table, not a scene object like the panels above.
            ImGui::MenuItem(AV("input_bindings").c_str(), nullptr, &st.show_input);
            ImGui::MenuItem(AV("tab_talent_tree").c_str(), nullptr, &st.show_talent_tree);
            ImGui::MenuItem(AV("tab_flow_field").c_str(), nullptr, &st.show_flow_field);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(AV("create").c_str())) {
            create_menu_contents(app);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(AV("game").c_str())) {
            game_menu_contents(app);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(AV("settings").c_str())) {
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

        if (ImGui::BeginMenu(AV("help").c_str())) {
            if (ImGui::MenuItem(AV("start_here_menu").c_str())) {
                st.show_start_here = true;
            }
            if (ImGui::MenuItem(AV("shortcuts").c_str(), "F1")) {
                st.show_shortcuts = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(AV("about").c_str())) {
                st.show_about = true;
            }
            ImGui::EndMenu();
        }

        // Right-hand side: the live status. The numbers that used to sit in the
        // middle of the bar are still here, just not on top of the buttons.
        const EditorStatus status = app.status();
        std::string status_str;
        const auto sep = [&]() -> const char* { return status_str.empty() ? "" : " | "; };
        if (st.show_fps) {
            char fb[64] = {};
            std::snprintf(fb, sizeof(fb), "%.1f %s (%.1f ms)", status.fps,
                          ui::tr("fps").c_str(), status.frame_ms);
            status_str += fb;
        }
        if (stats.validation_on && st.show_validation) {
            char vb[64] = {};
            std::snprintf(vb, sizeof(vb), "%s%s: %u", sep(), ui::tr("validation").c_str(),
                          stats.validation_errors);
            status_str += vb;
        }
        {
            char eb[64] = {};
            std::snprintf(eb, sizeof(eb), "%s%zu %s", sep(), status.entity_count,
                          ui::tr("entities").c_str());
            status_str += eb;
        }
        if (app.has_project()) {
            status_str += sep();
            status_str += ui::tr("project") + ": " + app.project_name();
        }

        const std::string shaped_status = rt_active() ? ui::shape_arabic(status_str) : status_str;
        const float status_w = ImGui::CalcTextSize(shaped_status.c_str()).x;
        const float target_x = ImGui::GetWindowWidth() - status_w - 18.0f;
        if (target_x > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(target_x);
        }
        ImGui::TextDisabled("%s", shaped_status.c_str());

        // 1px border line under the main menu bar
        const ImVec2 bar_pos = ImGui::GetWindowPos();
        const ImVec2 bar_sz = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(bar_pos.x, bar_pos.y + bar_sz.y - 1.0f),
                    ImVec2(bar_pos.x + bar_sz.x, bar_pos.y + bar_sz.y - 1.0f),
                    IM_COL32(36, 42, 54, 255), 1.0f);

        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(1);

    // --- Toolbar row ----------------------------------------------------------
    const float toolbar_h = 44.0f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(23, 26, 33, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(36, 42, 54, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (ImGui::BeginViewportSideBar("##NFToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, toolbar_h,
                                    ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove)) {
        const float btn_sz = 28.0f;
        const float y_pos = (toolbar_h - btn_sz) * 0.5f;
        ImGui::SetCursorPosY(y_pos);

        const ImVec2 tb_pos = ImGui::GetWindowPos();
        const ImVec2 tb_sz = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 1px bottom border line for the toolbar
        dl->AddLine(ImVec2(tb_pos.x, tb_pos.y + toolbar_h - 1.0f),
                    ImVec2(tb_pos.x + tb_sz.x, tb_pos.y + toolbar_h - 1.0f),
                    IM_COL32(34, 40, 52, 255), 1.0f);

        // Brand mark + localised engine name pill
        {
            const float badge_sz = 22.0f;
            const std::string name = AV("engine_name");
            const ImVec2 text_sz = ImGui::CalcTextSize(name.c_str());
            const float pill_w = badge_sz + 8.0f + text_sz.x + 14.0f;
            const ImVec2 cur = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##tb_brand", ImVec2(pill_w, btn_sz));
            const bool brand_hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) {
                st.show_about = true;
            }
            if (brand_hovered) {
                dl->AddRectFilled(cur, ImVec2(cur.x + pill_w, cur.y + btn_sz),
                                  IM_COL32(40, 48, 64, 180), 4.0f);
                dl->AddRect(cur, ImVec2(cur.x + pill_w, cur.y + btn_sz),
                            IM_COL32(65, 78, 102, 160), 4.0f);
                ImGui::SetTooltip("%s", AV("about").c_str());
            }
            const float badge_y = cur.y + (btn_sz - badge_sz) * 0.5f;
            draw_brand_badge(dl, ImVec2(cur.x + 6.0f, badge_y), badge_sz);
            const float text_y = cur.y + (btn_sz - text_sz.y) * 0.5f;
            dl->AddText(ImVec2(cur.x + badge_sz + 12.0f, text_y),
                        ImGui::GetColorU32(theme::text_primary()), name.c_str());
        }

        ImGui::SameLine(0.0f, 10.0f);
        toolbar_divider(btn_sz);

        // 1. Scene / Project actions
        if (icon_button("##tb_new", Icon::New, AV("new_scene") + " (Ctrl+N)", false, btn_sz)) {
            std::string err;
            if (!app.new_scene(err)) {
                push_error_console(app, "New scene failed", err);
            }
        }
        ImGui::SameLine();
        if (icon_button("##tb_open", Icon::Open, AV("open_scene") + " (Ctrl+O)", false, btn_sz)) {
            std::string picked;
            if (open_file_dialog(AV("open_scene"), scene_filter(), picked)) {
                intents.open_scene_dialog_confirm = true;
                intents.open_scene_path = picked;
            }
        }
        ImGui::SameLine();
        if (icon_button("##tb_save", Icon::Save, AV("save_scene") + " (Ctrl+S)", false, btn_sz)) {
            std::string err;
            if (!app.save(err)) {
                push_error_console(app, "Save failed", err);
            }
        }

        ImGui::SameLine(0.0f, 8.0f);
        toolbar_divider(btn_sz);

        // 2. Gizmo modes
        gizmo_mode_button(app, "##tb_move", Icon::Move, "move", " (W)", GizmoMode::Translate, btn_sz);
        ImGui::SameLine();
        gizmo_mode_button(app, "##tb_rotate", Icon::Rotate, "rotate", " (E)", GizmoMode::Rotate, btn_sz);
        ImGui::SameLine();
        gizmo_mode_button(app, "##tb_scale", Icon::Scale, "scale_tool", " (R)", GizmoMode::Scale, btn_sz);
        ImGui::SameLine();

        const bool local_space = (app.gizmo_space() == GizmoSpace::Local);
        if (text_toolbar_button("##tb_space", local_space ? AV("local_space") : AV("world_space"),
                                local_space, btn_sz)) {
            app.set_gizmo_space(local_space ? GizmoSpace::World : GizmoSpace::Local);
        }
        ImGui::SameLine();
        if (icon_button("##tb_snap", Icon::Snap, AV("snap"), st.snap_enabled, btn_sz)) {
            st.snap_enabled = !st.snap_enabled;
            sync_snap_from_settings(app, st);
        }

        ImGui::SameLine(0.0f, 8.0f);
        toolbar_divider(btn_sz);

        // 3. Quick primitives
        create_primitive_button(app, "##tb_ground", Icon::Ground, PrimitiveKind::Ground, "add_ground", btn_sz);
        ImGui::SameLine();
        create_primitive_button(app, "##tb_cube", Icon::Cube, PrimitiveKind::Cube, "add_cube", btn_sz);

        ImGui::SameLine(0.0f, 8.0f);
        toolbar_divider(btn_sz);

        // 4. Undo / Redo & Settings
        if (icon_button("##tb_undo", Icon::Undo, AV("undo") + " (Ctrl+Z)", false, btn_sz)) {
            std::string err;
            if (!app.undo(err)) {
                push_error_console(app, "Undo failed", err);
            }
        }
        ImGui::SameLine();
        if (icon_button("##tb_redo", Icon::Redo, AV("redo") + " (Ctrl+Y)", false, btn_sz)) {
            std::string err;
            if (!app.redo(err)) {
                push_error_console(app, "Redo failed", err);
            }
        }
        ImGui::SameLine();
        if (icon_button("##tb_settings", Icon::Settings, AV("settings"), st.show_settings, btn_sz)) {
            st.show_settings = !st.show_settings;
        }

        // 5. Play / Stop (right-aligned)
        const float play_btn_w = 34.0f;
        const float play_x = ImGui::GetWindowWidth() - play_btn_w - 18.0f;
        if (ImGui::GetCursorPosX() < play_x) {
            ImGui::SetCursorPosX(play_x);
        }
        ImGui::SetCursorPosY(y_pos);
        play_button(app, btn_sz);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
// --- Settings window ------------------------------------------------------
    if (st.show_settings) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin((AV("settings") + "###NFSettings").c_str(), &st.show_settings)) {
            if (settings_section("viewport", true)) {
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

            if (settings_section("snap")) {
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

            if (settings_section("sky_preset")) {
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

            if (settings_section("rendering")) {
                // Exposure is the ONE renderer setting the editor may write, and
                // it goes through the runtime's narrow setter (Runtime::set_exposure)
                // rather than a mutable renderer() — see that function. It applies
                // on the next frame, so there is no Apply button and no undo step:
                // no scene data changed.
                if (runtime::Runtime* rt = app.runtime()) {
                    float exposure = rt->exposure();
                    ImGui::SetNextItemWidth(200.0f);
                    if (ImGui::SliderFloat((AV("exposure") + "##settings_exposure").c_str(),
                                           &exposure, 0.1f, 8.0f, "%.2f")) {
                        rt->set_exposure(exposure);
                    }
                }

                // Everything below is COMPONENT state, not renderer state: the
                // editor writes the same values the renderer reads — the shadow
                // parameters of the scene's directional light (scene_light_entity
                // picks the first, which is the one that is drawn). The Inspector
                // edits the SELECTED light; a user who never selects it had no way
                // to reach shadows at all, which is the gap the lead's list names.
                // One Apply = one undo step, exactly like the Inspector's light
                // section.
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

            if (settings_section("language")) {
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

            if (settings_section("autosave")) {
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

            if (settings_section("theme_accent")) {
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
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(580.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
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
            ImGui::TextDisabled("%s", AV("import_to").c_str());
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::InputText("###dst", dst, sizeof(dst));
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
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
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

    // --- Keyboard shortcuts modal (F1 / Help > Shortcuts) ---------------------
    if (st.show_shortcuts) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(560.0f, 530.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin((AV("shortcuts") + "###NFShortcuts").c_str(), &st.show_shortcuts)) {
            auto shortcut_row = [](const std::string& action, const char* key) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                rt_text_str(action);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.72f, 1.0f, 1.0f));
                ImGui::TextUnformatted(key);
                ImGui::PopStyleColor();
            };

            auto category_header = [](const std::string& title) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Spacing();
                rt_colored_str(theme::accent(), title);
                ImGui::Separator();
                ImGui::TableSetColumnIndex(1);
                ImGui::Spacing();
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::Separator();
            };

            if (ImGui::BeginTable("##shortcuts_table", 2,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(AV("shortcut_action").c_str(), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(AV("shortcut_key").c_str(), ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableHeadersRow();

                category_header(AV("shortcut_general"));
                shortcut_row(AV("shortcuts"), "F1");
                shortcut_row(AV("shortcut_f5"), "F5");
                shortcut_row(AV("save_scene"), "Ctrl + S");
                shortcut_row(AV("open_scene"), "Ctrl + O");
                shortcut_row(AV("new_scene"), "Ctrl + N");
                shortcut_row(AV("undo"), "Ctrl + Z");
                shortcut_row(AV("redo"), "Ctrl + Y / Ctrl+Shift+Z");
                shortcut_row(AV("close"), "Esc");

                category_header(AV("shortcut_viewport"));
                shortcut_row(AV("shortcut_select"), "Q");
                shortcut_row(AV("move"), "W");
                shortcut_row(AV("rotate"), "E");
                shortcut_row(AV("scale_tool"), "R");
                shortcut_row(AV("shortcut_focus"), "F");
                shortcut_row(AV("shortcut_fly_cam"), "RMB + WASD");

                category_header(AV("entities"));
                shortcut_row(AV("delete"), "Delete");
                shortcut_row(AV("duplicate"), "Ctrl + D");

                category_header(AV("assets"));
                shortcut_row(AV("import_model"), "Ctrl + I");
                shortcut_row(AV("export"), "Ctrl + E");

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button(AV("close").c_str(), ImVec2(120.0f, 0.0f))) {
                st.show_shortcuts = false;
            }
        }
        ImGui::End();
    }

    // --- About ----------------------------------------------------------------
    if (st.show_about) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin((AV("about") + "###NFAbout").c_str(), &st.show_about)) {
#ifndef NF_ENGINE_VERSION
#define NF_ENGINE_VERSION "0.6.0"
#endif
            const float badge_sz = 44.0f;
            const ImVec2 cur = ImGui::GetCursorScreenPos();
            draw_brand_badge(ImGui::GetWindowDrawList(), cur, badge_sz);
            ImGui::Dummy(ImVec2(badge_sz, badge_sz));
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::BeginGroup();
            rt_colored_str(ImVec4(1.0f, 0.68f, 0.22f, 1.0f),
                           ui::shape_arabic(ui::tr("engine_name") + "  v" + NF_ENGINE_VERSION));
            rt_text_str(AV("founder"));
            ImGui::EndGroup();
            ImGui::Spacing();
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
