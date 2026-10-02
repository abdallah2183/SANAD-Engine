// TransformGizmoView.cpp — ImGui drawing + hit-testing for the transform gizmo.
//
// One call per frame after the viewport image. Reads the selection, the live
// render camera and the gizmo mode/space; draws arrows + planes (translate),
// rings (rotate) or arms + boxes (scale) at the selection origin; hover
// highlights the handle under the pointer and reports it back. The caller
// routes a click on the reported handle to UiIntents::viewport_gizmo_press.
//
// The geometry (layout, hit-testing, drag deltas) is TransformGizmo.hpp —
// this file only turns layouts into draw-list calls.

#include <NF/Editor/TransformGizmoView.hpp>

#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/TransformGizmo.hpp>
#include <NF/Editor/UiTheme.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>

namespace nf::editor {

namespace {

constexpr float kGrabPx = 9.0f; // pointer grab tolerance around every handle
constexpr float kConeLenPx = 16.0f;
constexpr float kConeHalfPx = 7.0f;
constexpr float kTipBoxHalfPx = 8.0f;
constexpr float kCenterBoxHalfPx = 7.0f;
constexpr float kPlaneFrac = 0.35f; // plane quad side as a fraction of arm length

ImVec4 axis_color(int axis) {
    if (axis == 0) {
        return theme::axis_x();
    }
    if (axis == 1) {
        return theme::axis_y();
    }
    return theme::axis_z();
}

ImU32 with_alpha(ImVec4 c, float a) {
    c.w = a;
    return ImGui::GetColorU32(c);
}

// Screen-space unit direction + length of origin -> tip. False when collapsed
// (axis aims at the eye), where a cone would fold inside out.
bool screen_dir(float ox, float oy, float tx, float ty, float& dx, float& dy) {
    const float vx = tx - ox;
    const float vy = ty - oy;
    const float len = std::sqrt(vx * vx + vy * vy);
    if (len <= 1e-4f) {
        return false;
    }
    dx = vx / len;
    dy = vy / len;
    return true;
}

void draw_cone(ImDrawList* dl, float ox, float oy, float tx, float ty, ImU32 col, bool hot) {
    float dx = 1.0f, dy = 0.0f;
    if (!screen_dir(ox, oy, tx, ty, dx, dy)) {
        dl->AddCircleFilled(ImVec2(tx, ty), kConeHalfPx, col, 16);
        return;
    }
    const float bx = tx - dx * kConeLenPx;
    const float by = ty - dy * kConeLenPx;
    const float nx = -dy;
    const float ny = dx;
    const float hw = kConeHalfPx + (hot ? 1.5f : 0.0f);
    const ImVec2 tip(tx, ty);
    const ImVec2 b0(bx + nx * hw, by + ny * hw);
    const ImVec2 b1(bx - nx * hw, by - ny * hw);
    dl->AddTriangleFilled(ImVec2(tip.x - dx * 1.5f, tip.y - dy * 1.5f),
                          ImVec2(b0.x - dx * 1.5f, b0.y - dy * 1.5f),
                          ImVec2(b1.x - dx * 1.5f, b1.y - dy * 1.5f),
                          IM_COL32(10, 12, 16, 140));
    dl->AddTriangleFilled(tip, b0, b1, col);
    if (hot) {
        dl->AddTriangle(tip, b0, b1, IM_COL32(255, 255, 255, 230), 1.5f);
    }
}

void draw_shaft(ImDrawList* dl, float ox, float oy, float tx, float ty, ImU32 col, float width,
                bool hot) {
    // Stop the shaft at the cone base so it never pokes through the head.
    float dx = 1.0f, dy = 0.0f;
    float ex = tx, ey = ty;
    if (screen_dir(ox, oy, tx, ty, dx, dy)) {
        ex = tx - dx * (kConeLenPx * 0.9f);
        ey = ty - dy * (kConeLenPx * 0.9f);
    }
    dl->AddLine(ImVec2(ox, oy), ImVec2(ex, ey), IM_COL32(10, 12, 16, 140), width + 2.5f);
    dl->AddLine(ImVec2(ox, oy), ImVec2(ex, ey), col, width + (hot ? 1.0f : 0.0f));
}

void draw_box(ImDrawList* dl, float cx, float cy, float half, ImU32 fill, bool hot) {
    dl->AddRectFilled(ImVec2(cx - half - 1.0f, cy - half - 1.0f),
                      ImVec2(cx + half + 1.0f, cy + half + 1.0f), IM_COL32(10, 12, 16, 160),
                      2.0f);
    dl->AddRectFilled(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half), fill, 2.0f);
    if (hot) {
        dl->AddRect(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half),
                    IM_COL32(255, 255, 255, 230), 2.0f, 1.5f);
    }
}

const char* handle_tip(GizmoMode mode, GizmoHandle h) {
    int axis = -1;
    (void)gizmo_handle_axis(h, axis);
    static const char* kMove[3] = {"X move", "Y move", "Z move"};
    static const char* kRot[3] = {"Rotate X", "Rotate Y", "Rotate Z"};
    static const char* kScale[3] = {"Scale X", "Scale Y", "Scale Z"};
    if (axis >= 0) {
        if (mode == GizmoMode::Rotate) {
            return kRot[axis];
        }
        if (mode == GizmoMode::Scale) {
            return kScale[axis];
        }
        return kMove[axis];
    }
    if (h == GizmoHandle::Center) {
        return (mode == GizmoMode::Scale) ? "Scale all" : "Move freely";
    }
    return "Move in plane";
}

} // namespace

GizmoHandle draw_transform_gizmo(EditorApp& app, const rendering::Camera& cam,
                                 const ImVec2& img_min, const ImVec2& img_max) {
    if (app.playing()) {
        return GizmoHandle::None;
    }
    ecs::World* w = app.world();
    if (w == nullptr || !app.selection().has_selection()) {
        return GizmoHandle::None;
    }
    // Origin entity: primary with a Transform, else the first selected with one.
    ecs::Entity origin_e = app.selection().primary();
    const scene::Transform* ot = nullptr;
    if (origin_e.valid() && w->is_alive(origin_e)) {
        ot = w->get<scene::Transform>(origin_e);
    }
    if (ot == nullptr) {
        for (ecs::Entity e : app.selection().all()) {
            if (!e.valid() || !w->is_alive(e)) {
                continue;
            }
            ot = w->get<scene::Transform>(e);
            if (ot != nullptr) {
                origin_e = e;
                break;
            }
        }
    }
    if (ot == nullptr) {
        return GizmoHandle::None;
    }
    const float x0 = img_min.x, y0 = img_min.y, x1 = img_max.x, y1 = img_max.y;
    const float img_w = x1 - x0, img_h = y1 - y0;
    if (img_w <= 1.0f || img_h <= 1.0f) {
        return GizmoHandle::None;
    }
    const float ox = ot->world_x, oy = ot->world_y, oz = ot->world_z;
    float to_cam_x = cam.position.x - ox;
    float to_cam_y = cam.position.y - oy;
    float to_cam_z = cam.position.z - oz;
    const float dist = std::sqrt(to_cam_x * to_cam_x + to_cam_y * to_cam_y + to_cam_z * to_cam_z);
    if (dist <= 1e-4f) {
        return GizmoHandle::None; // inside the selection: any gizmo would fold inside out
    }
    float view_x = cam.target.x - cam.position.x;
    float view_y = cam.target.y - cam.position.y;
    float view_z = cam.target.z - cam.position.z;
    const float view_l =
        std::sqrt(view_x * view_x + view_y * view_y + view_z * view_z);
    if (view_l <= 1e-9f) {
        return GizmoHandle::None;
    }
    view_x /= view_l;
    view_y /= view_l;
    view_z /= view_l;
    const float view_dir[3] = {view_x, view_y, view_z};

    // Frame axes: world, or the entity's own for Local space.
    float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (app.gizmo_space() == GizmoSpace::Local) {
        // The entity's WORLD rotation, not a quaternion rebuilt from its LOCAL
        // euler angles. For a root they are the same, but for a child of a
        // rotated parent they are not: using the local angles made the Local
        // gizmo axes point somewhere the object was not, so grabbing "local X"
        // moved the object along a world axis it was not aligned to. world_rot is
        // the same value extraction and bounds use, so the gizmo, the drawn mesh
        // and the pick volume all agree on which way the object faces.
        const Quat q = ot->world_rot;
        for (int a = 0; a < 3; ++a) {
            const Vec3 v = q.rotate(Vec3{axes[a][0], axes[a][1], axes[a][2]});
            axes[a][0] = v.x;
            axes[a][1] = v.y;
            axes[a][2] = v.z;
        }
    }

    const GizmoMode mode = app.gizmo_mode();
    const float len = gizmo_world_length(kTransformGizmoAxisPx, img_h, dist, cam.fov_y_rad);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool img_hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool dragging = app.viewport_dragging() && app.gizmo_drag_handle() != GizmoHandle::None;

    GizmoArmLayout layout{};
    const bool have_arms =
        (mode != GizmoMode::Rotate) &&
        gizmo_arm_layout(cam.view_projection, x0, y0, x1, y1, ox, oy, oz, axes, len, view_dir,
                         layout);

    // Hover (or the actively dragged handle while a gesture runs).
    GizmoHandle hover = GizmoHandle::None;
    if (dragging) {
        hover = app.gizmo_drag_handle();
    } else if (img_hovered && have_arms) {
        if (mode == GizmoMode::Translate) {
            hover = gizmo_hit_arms(layout, mouse.x, mouse.y, kGrabPx, kConeHalfPx + 2.0f,
                                   kCenterBoxHalfPx + 1.0f, kPlaneFrac, true, mode);
        } else {
            hover = gizmo_hit_arms(layout, mouse.x, mouse.y, kGrabPx, kTipBoxHalfPx + 1.0f,
                                   kCenterBoxHalfPx + 1.0f, 0.0f, false, mode);
        }
    } else if (img_hovered && mode == GizmoMode::Rotate) {
        hover = gizmo_hit_rings(cam.view_projection, x0, y0, x1, y1, ox, oy, oz, axes, len,
                                mouse.x, mouse.y, 7.0f);
    }
    // Hover feedback only — the click itself is routed by the caller (it
    // owns the press state machine): a click on the hovered handle becomes
    // viewport_gizmo_press, and the pick press is skipped for it.
    if (hover != GizmoHandle::None && !dragging && img_hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", handle_tip(mode, hover));
    }

    // --- Draw far -> near ------------------------------------------------------
    if (mode == GizmoMode::Rotate) {
        float samples[128 * 2];
        // Ring depth from the normal axis facing: draw the most edge-on
        // (least facing) first so the facing ring stays on top.
        int rorder[3] = {0, 1, 2};
        float facing[3] = {0.0f, 0.0f, 0.0f};
        for (int a = 0; a < 3; ++a) {
            facing[a] = std::fabs(axes[a][0] * view_x + axes[a][1] * view_y + axes[a][2] * view_z);
        }
        for (int i = 1; i < 3; ++i) {
            const int key = rorder[i];
            int j = i - 1;
            while (j >= 0 && facing[rorder[j]] > facing[key]) {
                rorder[j + 1] = rorder[j];
                --j;
            }
            rorder[j + 1] = key;
        }
        for (int i = 0; i < 3; ++i) {
            const int a = rorder[i];
            const int n = gizmo_ring_samples(cam.view_projection, x0, y0, x1, y1, ox, oy, oz,
                                             axes, a, len, samples, 128);
            if (n < 2) {
                continue;
            }
            const bool hot =
                (hover == GizmoHandle::AxisX && a == 0) || (hover == GizmoHandle::AxisY && a == 1) ||
                (hover == GizmoHandle::AxisZ && a == 2);
            const ImU32 col = with_alpha(axis_color(a), hot ? 1.0f : 0.85f);
            // Dark underlay first, colour on top: legible over any background.
            for (int pass = 0; pass < 2; ++pass) {
                for (int s = 0; s < n; ++s) {
                    const float* p = &samples[s * 2];
                    const float* q = &samples[((s + 1) % n) * 2];
                    if (pass == 0) {
                        dl->AddLine(ImVec2(p[0], p[1]), ImVec2(q[0], q[1]),
                                    IM_COL32(10, 12, 16, 120), hot ? 6.5f : 4.5f);
                    } else {
                        dl->AddLine(ImVec2(p[0], p[1]), ImVec2(q[0], q[1]), col,
                                    hot ? 4.0f : 2.5f);
                    }
                }
            }
            if (hot) {
                // White seam on the hot ring.
                for (int s = 0; s < n; ++s) {
                    const float* p = &samples[s * 2];
                    const float* q = &samples[((s + 1) % n) * 2];
                    dl->AddLine(ImVec2(p[0], p[1]), ImVec2(q[0], q[1]),
                                IM_COL32(255, 255, 255, 200), 1.0f);
                }
            }
        }
        return hover;
    }

    if (!have_arms) {
        return hover;
    }
    static const GizmoHandle kAxisH[3] = {GizmoHandle::AxisX, GizmoHandle::AxisY,
                                          GizmoHandle::AxisZ};
    if (mode == GizmoMode::Translate) {
        // Planes first (behind everything), then arms far -> near, center last.
        static const int kPlanes[3][4] = {{0, 1, 2, 0}, {0, 2, 1, 1}, {1, 2, 0, 2}};
        static const GizmoHandle kPlaneH[3] = {GizmoHandle::PlaneXY, GizmoHandle::PlaneXZ,
                                               GizmoHandle::PlaneYZ};
        for (int p = 0; p < 3; ++p) {
            const int a = kPlanes[p][0], b = kPlanes[p][1], nn = kPlanes[p][2];
            const bool hot = (hover == kPlaneH[p]);
            const ImU32 col = with_alpha(axis_color(nn), hot ? 0.55f : 0.30f);
            ImVec2 c[4];
            const float ws[4][3] = {
                {ox, oy, oz},
                {ox + axes[a][0] * len * kPlaneFrac, oy + axes[a][1] * len * kPlaneFrac,
                 oz + axes[a][2] * len * kPlaneFrac},
                {ox + (axes[a][0] + axes[b][0]) * len * kPlaneFrac,
                 oy + (axes[a][1] + axes[b][1]) * len * kPlaneFrac,
                 oz + (axes[a][2] + axes[b][2]) * len * kPlaneFrac},
                {ox + axes[b][0] * len * kPlaneFrac, oy + axes[b][1] * len * kPlaneFrac,
                 oz + axes[b][2] * len * kPlaneFrac},
            };
            // Project with the same view matrix the hit test used.
            bool ok = true;
            for (int k = 0; k < 4 && ok; ++k) {
                const Vec4 clip =
                    cam.view_projection * Vec4{ws[k][0], ws[k][1], ws[k][2], 1.0f};
                if (clip.w <= 1e-5f) {
                    ok = false;
                    break;
                }
                // Same linear NDC -> px mapping as gizmo_arm_layout: no y flip
                // (the engine's projection already carries the Vulkan flip).
                const float nx = clip.x / clip.w, ny = clip.y / clip.w;
                c[k].x = x0 + (nx * 0.5f + 0.5f) * img_w;
                c[k].y = y0 + (ny * 0.5f + 0.5f) * img_h;
            }
            if (!ok) {
                continue;
            }
            dl->AddQuadFilled(c[0], c[1], c[2], c[3], col);
            dl->AddQuad(c[0], c[1], c[2], c[3], with_alpha(axis_color(nn), hot ? 0.9f : 0.5f),
                        1.0f);
        }
        for (int i = 0; i < 3; ++i) {
            const int a = layout.order[i];
            const bool hot = (hover == kAxisH[a]);
            const ImU32 col = ImGui::GetColorU32(axis_color(a));
            draw_shaft(dl, layout.ox, layout.oy, layout.tip_x[a], layout.tip_y[a], col, 3.0f,
                       hot);
            draw_cone(dl, layout.ox, layout.oy, layout.tip_x[a], layout.tip_y[a], col, hot);
        }
        draw_box(dl, layout.ox, layout.oy, kCenterBoxHalfPx, IM_COL32(235, 238, 245, 235),
                 hover == GizmoHandle::Center);
        return hover;
    }
    // Scale: thin arms + box tips + uniform center box.
    for (int i = 0; i < 3; ++i) {
        const int a = layout.order[i];
        const bool hot = (hover == kAxisH[a]);
        const ImU32 col = ImGui::GetColorU32(axis_color(a));
        dl->AddLine(ImVec2(layout.ox, layout.oy), ImVec2(layout.tip_x[a], layout.tip_y[a]),
                    IM_COL32(10, 12, 16, 140), 4.0f);
        dl->AddLine(ImVec2(layout.ox, layout.oy), ImVec2(layout.tip_x[a], layout.tip_y[a]), col,
                    hot ? 3.0f : 2.0f);
        draw_box(dl, layout.tip_x[a], layout.tip_y[a], kTipBoxHalfPx, col, hot);
    }
    draw_box(dl, layout.ox, layout.oy, kCenterBoxHalfPx + 1.0f, IM_COL32(235, 238, 245, 235),
             hover == GizmoHandle::Center);
    return hover;
}

} // namespace nf::editor
