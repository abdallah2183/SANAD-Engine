// TransformGizmo.cpp — pure screen-space math for the in-viewport gizmo.
//
// See TransformGizmo.hpp for the contract. Everything here is float
// arithmetic over an explicit px rect: no ImGui, no window, no scene.

#include <NF/Editor/TransformGizmo.hpp>

#include <cmath>

namespace nf::editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool project_px(const Mat4& view_proj, float x0, float y0, float x1, float y1, float wx, float wy,
                float wz, float& out_x, float& out_y) {
    const Vec4 clip = view_proj * Vec4{wx, wy, wz, 1.0f};
    if (clip.w <= 1e-5f) {
        return false; // behind the camera: a projection would fold inside out
    }
    // NDC -> px is LINEAR, with NO y flip, and that is the whole fix.
    //
    // The render NDC this receives is already y-DOWN: Mat4::perspective
    // negates m[1][1] for Vulkan, so world +Y lands at NDC y < 0, which is
    // memory row 0 — and memory row 0 IS the top of the presented image
    // (ImGui samples the texture with v=0 at the widget top). So
    // ndc_y = -1 is the image top and the mapping to pixels is plain
    // `ny * 0.5 + 0.5`.
    //
    // The `0.5 - ny * 0.5` this used to apply was the bridge for the OTHER
    // direction: it converts RENDER NDC into POINTER NDC (Panels' to_ndc maps
    // the image top to +1, and viewport_ndc_to_pixel maps +1 back to row 0).
    // Applying it here drew the gizmo vertically mirrored against the image it
    // is drawn over — the handles floated off their object, the hover missed
    // the handle the user was pointing at, and an axis drag moved the opposite
    // way. Pinned by gizmo_projection_agrees_with_the_pointer_row.
    const float ndc_x = clip.x / clip.w;
    const float ndc_y = clip.y / clip.w;
    out_x = x0 + (ndc_x * 0.5f + 0.5f) * (x1 - x0);
    out_y = y0 + (ndc_y * 0.5f + 0.5f) * (y1 - y0);
    return true;
}

// Orthonormal basis (u, v) spanning the plane perpendicular to unit axis n.
void plane_basis(float nx, float ny, float nz, float u[3], float v[3]) {
    // Pick a helper vector least aligned with n, cross twice.
    float hx = 0.0f, hy = 1.0f, hz = 0.0f;
    if (std::fabs(ny) > 0.9f) {
        hx = 1.0f;
        hy = 0.0f;
        hz = 0.0f;
    }
    // u = normalize(n x h)
    float ux = ny * hz - nz * hy;
    float uy = nz * hx - nx * hz;
    float uz = nx * hy - ny * hx;
    const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (ul <= 1e-9f) {
        ux = 1.0f;
        uy = 0.0f;
        uz = 0.0f;
    } else {
        ux /= ul;
        uy /= ul;
        uz /= ul;
    }
    // v = n x u
    u[0] = ux;
    u[1] = uy;
    u[2] = uz;
    v[0] = ny * uz - nz * uy;
    v[1] = nz * ux - nx * uz;
    v[2] = nx * uy - ny * ux;
}

} // namespace

bool gizmo_handle_axis(GizmoHandle h, int& out_axis) {
    switch (h) {
        case GizmoHandle::AxisX: out_axis = 0; return true;
        case GizmoHandle::AxisY: out_axis = 1; return true;
        case GizmoHandle::AxisZ: out_axis = 2; return true;
        default: return false;
    }
}

bool gizmo_handle_plane(GizmoHandle h, int& out_axis_a, int& out_axis_b, int& out_normal) {
    switch (h) {
        case GizmoHandle::PlaneXY: out_axis_a = 0; out_axis_b = 1; out_normal = 2; return true;
        case GizmoHandle::PlaneXZ: out_axis_a = 0; out_axis_b = 2; out_normal = 1; return true;
        case GizmoHandle::PlaneYZ: out_axis_a = 1; out_axis_b = 2; out_normal = 0; return true;
        default: return false;
    }
}

bool gizmo_axis_param(const Ray& ray, float px, float py, float pz, float dx, float dy, float dz,
                      float& out_t) {
    // Closest point between ray (o + t*d) and line (p + s*a):
    // solve 2x2 in the (t, s) plane. Denominator ~0 means parallel.
    const float ox = ray.ox - px;
    const float oy = ray.oy - py;
    const float oz = ray.oz - pz;
    const float a = ray.dx * ray.dx + ray.dy * ray.dy + ray.dz * ray.dz; // == 1, kept explicit
    const float b = ray.dx * dx + ray.dy * dy + ray.dz * dz;
    const float c = 1.0f; // |axis| == 1 by contract
    const float d = ray.dx * ox + ray.dy * oy + ray.dz * oz;
    const float e = dx * ox + dy * oy + dz * oz;
    const float denom = a * c - b * b;
    if (std::fabs(denom) < 1e-8f) {
        return false;
    }
    // `s`, the parameter ALONG THE AXIS — that is the number an axis drag
    // needs, and the doc above always claimed it. The expression this used to
    // evaluate is the standard 2x2 solution, but it yields `t`, the parameter
    // along the RAY. They differ by the angle between the two, so a drag that
    // moved the pointer toward the axis end of the screen produced a small
    // `t` delta and one that moved away produced a large one — the object
    // moved at wildly different rates per direction, and near a ray parallel
    // to the axis `t` ran away to a huge value (the "drag flies off" report).
    // Deriving `s` directly (closest approach onto the axis) is symmetric in
    // both directions and bounded by how far the pointer can actually travel.
    out_t = (a * e - b * d) / denom;
    return true;
}

bool gizmo_plane_point(const Ray& ray, float px, float py, float pz, float nx, float ny,
                       float nz, float out_p[3]) {
    const float denom = ray.dx * nx + ray.dy * ny + ray.dz * nz;
    if (std::fabs(denom) < 1e-9f) {
        return false;
    }
    const float t = ((px - ray.ox) * nx + (py - ray.oy) * ny + (pz - ray.oz) * nz) / denom;
    if (t < 0.0f) {
        return false; // behind the eye
    }
    out_p[0] = ray.ox + ray.dx * t;
    out_p[1] = ray.oy + ray.dy * t;
    out_p[2] = ray.oz + ray.dz * t;
    return true;
}

float gizmo_ring_angle(float ax, float ay, float az, float bx, float by, float bz, float nx,
                       float ny, float nz) {
    // Signed angle from a to b about n: atan2(n . (a x b), a . b).
    const float cx = ay * bz - az * by;
    const float cy = az * bx - ax * bz;
    const float cz = ax * by - ay * bx;
    const float sin_a = nx * cx + ny * cy + nz * cz;
    const float cos_a = ax * bx + ay * by + az * bz;
    if (sin_a == 0.0f && cos_a <= 0.0f) {
        return kPi; // exactly opposite: atan2 would give +-pi, pick +pi
    }
    return std::atan2(sin_a, cos_a);
}

float gizmo_uniform_factor(float ox, float oy, float press_x, float press_y, float cur_x,
                           float cur_y, float radius_px) {
    if (radius_px <= 1e-6f) {
        return 1.0f;
    }
    const float d0x = press_x - ox;
    const float d0y = press_y - oy;
    const float d1x = cur_x - ox;
    const float d1y = cur_y - oy;
    const float r0 = std::sqrt(d0x * d0x + d0y * d0y);
    const float r1 = std::sqrt(d1x * d1x + d1y * d1y);
    return 1.0f + (r1 - r0) / radius_px;
}

float gizmo_axis_factor(float press_x, float press_y, float cur_x, float cur_y, float dir_x,
                        float dir_y, float axis_px_len) {
    if (axis_px_len <= 1e-6f) {
        return 1.0f;
    }
    const float dx = cur_x - press_x;
    const float dy = cur_y - press_y;
    return 1.0f + (dx * dir_x + dy * dir_y) / axis_px_len;
}

bool gizmo_arm_layout(const Mat4& view_proj, float x0, float y0, float x1, float y1, float ox,
                      float oy, float oz, const float axes[3][3], float len,
                      const float view_dir[3], GizmoArmLayout& out) {
    GizmoArmLayout l;
    if (!project_px(view_proj, x0, y0, x1, y1, ox, oy, oz, l.ox, l.oy)) {
        return false;
    }
    for (int a = 0; a < 3; ++a) {
        if (!project_px(view_proj, x0, y0, x1, y1, ox + axes[a][0] * len, oy + axes[a][1] * len,
                        oz + axes[a][2] * len, l.tip_x[a], l.tip_y[a])) {
            // Tip behind the camera (axis aims at the eye): park it on the
            // origin so the arm collapses instead of folding across the view.
            l.tip_x[a] = l.ox;
            l.tip_y[a] = l.oy;
        }
        l.depth[a] = axes[a][0] * view_dir[0] + axes[a][1] * view_dir[1] + axes[a][2] * view_dir[2];
        l.order[a] = a;
    }
    // Insertion sort far -> near (larger depth = pointing away = drawn first).
    for (int i = 1; i < 3; ++i) {
        const int key = l.order[i];
        int j = i - 1;
        while (j >= 0 && l.depth[l.order[j]] < l.depth[key]) {
            l.order[j + 1] = l.order[j];
            --j;
        }
        l.order[j + 1] = key;
    }
    out = l;
    return true;
}

float gizmo_point_to_segment(float mx, float my, float ax, float ay, float bx, float by) {
    const float abx = bx - ax;
    const float aby = by - ay;
    const float len_sq = abx * abx + aby * aby;
    if (len_sq <= 1e-12f) {
        return -1.0f;
    }
    float t = ((mx - ax) * abx + (my - ay) * aby) / len_sq;
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    const float px = ax + abx * t;
    const float py = ay + aby * t;
    const float dx = mx - px;
    const float dy = my - py;
    return std::sqrt(dx * dx + dy * dy);
}

bool gizmo_point_in_quad(float mx, float my, const float q[4][2]) {
    // Convex quad, consistent winding: same-side test on all edges.
    bool pos = false;
    bool neg = false;
    for (int i = 0; i < 4; ++i) {
        const float* a = q[i];
        const float* b = q[(i + 1) % 4];
        const float cross = (b[0] - a[0]) * (my - a[1]) - (b[1] - a[1]) * (mx - a[0]);
        if (cross > 1e-6f) {
            pos = true;
        } else if (cross < -1e-6f) {
            neg = true;
        }
        if (pos && neg) {
            return false;
        }
    }
    return true;
}

GizmoHandle gizmo_hit_arms(const GizmoArmLayout& l, float mx, float my, float grab_px,
                           float tip_half, float center_half, float plane_frac, bool want_planes,
                           GizmoMode mode) {
    static const GizmoHandle kAxis[3] = {GizmoHandle::AxisX, GizmoHandle::AxisY,
                                         GizmoHandle::AxisZ};
    const float grab = grab_px > 0.0f ? grab_px : 1.0f;
    // 1. Tip boxes (small targets win over the shaft behind them).
    // order[] runs far -> near, so scan it backwards: a near tip wins ties.
    if (tip_half > 0.0f) {
        for (int i = 2; i >= 0; --i) {
            const int a = l.order[i];
            if (std::fabs(mx - l.tip_x[a]) <= tip_half &&
                std::fabs(my - l.tip_y[a]) <= tip_half) {
                return kAxis[a];
            }
        }
    }
    // 2. Center box.
    if (center_half > 0.0f && std::fabs(mx - l.ox) <= center_half &&
        std::fabs(my - l.oy) <= center_half) {
        return GizmoHandle::Center;
    }
    // 3. Shafts: nearest within grab, ties broken far -> near.
    int best = -1;
    float best_d = grab;
    for (int i = 0; i < 3; ++i) {
        const int a = l.order[i];
        const float d = gizmo_point_to_segment(mx, my, l.ox, l.oy, l.tip_x[a], l.tip_y[a]);
        if (d >= 0.0f && d <= best_d) {
            best_d = d;
            best = a;
        }
    }
    if (best >= 0) {
        return kAxis[best];
    }
    // 4. Plane quads near the origin (translate mode only).
    if (want_planes && plane_frac > 0.0f && mode == GizmoMode::Translate) {
        static const int kPlanes[3][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}};
        static const GizmoHandle kPlaneH[3] = {GizmoHandle::PlaneXY, GizmoHandle::PlaneXZ,
                                               GizmoHandle::PlaneYZ};
        for (int p = 0; p < 3; ++p) {
            const int a = kPlanes[p][0];
            const int b = kPlanes[p][1];
            const float q[4][2] = {
                {l.ox, l.oy},
                {l.ox + (l.tip_x[a] - l.ox) * plane_frac,
                 l.oy + (l.tip_y[a] - l.oy) * plane_frac},
                {l.ox + (l.tip_x[a] - l.ox + l.tip_x[b] - l.ox) * plane_frac,
                 l.oy + (l.tip_y[a] - l.oy + l.tip_y[b] - l.oy) * plane_frac},
                {l.ox + (l.tip_x[b] - l.ox) * plane_frac,
                 l.oy + (l.tip_y[b] - l.oy) * plane_frac},
            };
            if (gizmo_point_in_quad(mx, my, q)) {
                return kPlaneH[p];
            }
        }
    }
    return GizmoHandle::None;
}

int gizmo_ring_samples(const Mat4& view_proj, float x0, float y0, float x1, float y1, float ox,
                       float oy, float oz, const float axes[3][3], int axis, float len,
                       float* out_xy, int max_samples, float ring_step) {
    if (out_xy == nullptr || max_samples < 2 || axis < 0 || axis > 2) {
        return 0;
    }
    float u[3]{}, v[3]{};
    plane_basis(axes[axis][0], axes[axis][1], axes[axis][2], u, v);
    const float step = ring_step > 0.0f ? ring_step : 0.15f;
    int n = 0;
    for (float a = 0.0f; a < 2.0f * kPi && n < max_samples; a += step) {
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        const float wx = ox + (u[0] * ca + v[0] * sa) * len;
        const float wy = oy + (u[1] * ca + v[1] * sa) * len;
        const float wz = oz + (u[2] * ca + v[2] * sa) * len;
        float sx = 0.0f, sy = 0.0f;
        if (!project_px(view_proj, x0, y0, x1, y1, wx, wy, wz, sx, sy)) {
            continue; // segment behind the camera: skip the sample
        }
        out_xy[n * 2] = sx;
        out_xy[n * 2 + 1] = sy;
        ++n;
    }
    return n;
}

GizmoHandle gizmo_hit_rings(const Mat4& view_proj, float x0, float y0, float x1, float y1,
                            float ox, float oy, float oz, const float axes[3][3], float len,
                            float mx, float my, float grab_px, float ring_step) {
    static const GizmoHandle kAxis[3] = {GizmoHandle::AxisX, GizmoHandle::AxisY,
                                         GizmoHandle::AxisZ};
    const float grab = grab_px > 0.0f ? grab_px : 1.0f;
    const float step = ring_step > 0.0f ? ring_step : 0.15f;
    float samples[128 * 2];
    int best = -1;
    float best_d = grab;
    for (int a = 0; a < 3; ++a) {
        const int n = gizmo_ring_samples(view_proj, x0, y0, x1, y1, ox, oy, oz, axes, a, len,
                                         samples, 128, step);
        for (int i = 0; i < n; ++i) {
            const float* p = &samples[i * 2];
            const float* q = &samples[((i + 1) % n) * 2];
            const float d = gizmo_point_to_segment(mx, my, p[0], p[1], q[0], q[1]);
            if (d >= 0.0f && d < best_d) {
                best_d = d;
                best = a;
            }
        }
    }
    return best >= 0 ? kAxis[best] : GizmoHandle::None;
}

float gizmo_world_length(float size_px, float viewport_h_px, float dist, float fov_y_rad) {
    if (viewport_h_px <= 1e-6f || dist <= 1e-6f) {
        return 1.0f;
    }
    const float world_per_px = 2.0f * dist * std::tan(fov_y_rad * 0.5f) / viewport_h_px;
    const float len = size_px * world_per_px;
    return len > 1e-4f ? len : 1e-4f;
}

} // namespace nf::editor
