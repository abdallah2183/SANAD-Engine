#pragma once

// NF/Editor/TransformGizmo.hpp — the in-viewport transform gizmo model.
//
// What the screenshots ask for: Unity-style handles drawn over the viewport
// (translate arrows + plane quads + move-all box, rotate rings, scale arms
// with box tips + uniform box), hover highlight, and axis-constrained drags.
//
// ImGui-free on purpose: every function here is pure float math over an
// explicit screen rect, so EditorTests pins the geometry (projection,
// hit-testing, drag deltas) without a window. The ImGui drawing lives in
// ui/TransformGizmoView.cpp and only consumes the structs below.
//
// Screen convention: pixels, origin top-left, +Y DOWN (ImGui draw space).

#include <NF/Core/Math.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Editor/Gizmo.hpp>

#include <cstddef>
#include <cstdint>

namespace nf::editor {

// One draggable handle of the gizmo.
enum class GizmoHandle : uint8_t {
    None = 0,
    AxisX, // +X arrow / X ring / X scale arm
    AxisY,
    AxisZ,
    PlaneXY, // translate plane quads (translate mode only)
    PlaneXZ,
    PlaneYZ,
    Center, // camera-plane move (translate) / uniform scale (scale)
};

// Axis index (0/1/2) behind an Axis handle. False for anything else.
bool gizmo_handle_axis(GizmoHandle h, int& out_axis);

// The two spanning axes and the normal axis behind a Plane handle.
// False for anything else.
bool gizmo_handle_plane(GizmoHandle h, int& out_axis_a, int& out_axis_b, int& out_normal);

// --- Ray geometry (mouse ray from Gizmo::pick_ray + gizmo origin/axes) ------
//
// All three are the professional-drag primitives: an axis drag slides along
// the closest point of the mouse ray to the axis line, a plane drag follows
// the ray/plane hit, and a ring drag measures the angle the pointer sweeps
// around the axis. Each is incremental-friendly: call at press (snapshot)
// and per motion event, feed the difference.

// Closest-point parameter t on the line (p + s*d, |d| == 1) to `ray`.
// False when the ray runs parallel to the line (nothing to track).
bool gizmo_axis_param(const Ray& ray, float px, float py, float pz, float dx, float dy, float dz,
                      float& out_t);

// Ray/plane hit point. Plane through (px,py,pz) with unit normal (nx,ny,nz).
// False when the ray is parallel to the plane.
bool gizmo_plane_point(const Ray& ray, float px, float py, float pz, float nx, float ny,
                       float nz, float out_p[3]);

// Signed angle (radians, right-hand rule about the unit axis n) from the
// press hit `a` to the current hit `b`, both relative to the gizmo origin.
// a/b are the gizmo_plane_point hits of the press/current rays against the
// ring plane, so both already lie in the plane.
float gizmo_ring_angle(float ax, float ay, float az, float bx, float by, float bz, float nx,
                       float ny, float nz);

// Uniform-scale factor from radial pointer motion: how much further the
// pointer is from the gizmo origin (in px) than at press, relative to the
// gizmo's screen radius. Outward = grow.
float gizmo_uniform_factor(float ox, float oy, float press_x, float press_y, float cur_x,
                           float cur_y, float radius_px);

// Per-axis scale factor from pointer motion along the axis' screen direction:
// 1 + (pixels slid along the axis) / (axis screen length).
float gizmo_axis_factor(float press_x, float press_y, float cur_x, float cur_y, float dir_x,
                        float dir_y, float axis_px_len);

// --- Screen layout -----------------------------------------------------------
//
// One frame's worth of handle geometry in pixels. The view builds it from the
// selection (origin + axes + world length), hit-tests the pointer against it,
// and draws it back. Depth order (far -> near) resolves overlapping shafts.

struct GizmoArmLayout {
    float ox = 0.0f, oy = 0.0f; // origin, px
    float tip_x[3] = {0.0f, 0.0f, 0.0f}; // axis tips, px
    float tip_y[3] = {0.0f, 0.0f, 0.0f};
    float depth[3] = {0.0f, 0.0f, 0.0f}; // axis . view_dir (draw far -> near)
    int order[3] = {0, 1, 2}; // axis indices sorted far -> near
};

// Projects the gizmo arms: origin + axes[a]*len (world) through view_proj
// into the px rect [x0,y0]x[x1,y1]. Axes are unit world directions (already
// in world or local frame — the caller decides). False when the origin is
// behind the camera.
bool gizmo_arm_layout(const Mat4& view_proj, float x0, float y0, float x1, float y1,
                      float ox, float oy, float oz, const float axes[3][3], float len,
                      const float view_dir[3], GizmoArmLayout& out);

// Hit-tests a translate/scale arm layout. Priority (small targets win):
// tip boxes (half-size tip_half, 0 = skip) -> center box (half-size
// center_half, 0 = skip) -> shafts within grab_px (nearest wins, ties broken
// far -> near so a front shaft does not steal a grab meant behind it) ->
// plane quads (square of side plane_frac*len at the origin, only when
// want_planes). Returns GizmoHandle::None on a miss.
GizmoHandle gizmo_hit_arms(const GizmoArmLayout& layout, float mx, float my, float grab_px,
                           float tip_half, float center_half, float plane_frac, bool want_planes,
                           GizmoMode mode);

// Hit-tests the three rotate rings (radius = len around origin in the plane
// perpendicular to each axis). A ring grabs when the pointer passes within
// grab_px of its projected polyline. Returns the axis handle or None.
// ring_step controls the polyline resolution (radians per segment).
GizmoHandle gizmo_hit_rings(const Mat4& view_proj, float x0, float y0, float x1, float y1,
                            float ox, float oy, float oz, const float axes[3][3], float len,
                            float mx, float my, float grab_px, float ring_step = 0.15f);

// Samples one ring (circle of radius len around origin, perpendicular to
// axes[axis]) into px pairs. Returns the sample count (<= max_samples),
// 0 when the origin is behind the camera.
int gizmo_ring_samples(const Mat4& view_proj, float x0, float y0, float x1, float y1, float ox,
                       float oy, float oz, const float axes[3][3], int axis, float len,
                       float* out_xy, int max_samples, float ring_step = 0.15f);

// World length of a fixed-pixel-size gizmo: the length that spans
// size_px at `dist` from a perspective camera with vertical fov fov_y_rad.
float gizmo_world_length(float size_px, float viewport_h_px, float dist, float fov_y_rad);

// Shared screen size: the axis length (translate/scale) and ring diameter
// reference the view draws, and the EditorApp drag uses for its screen-space
// snapshots (uniform-scale radius). One constant so the two can never drift.
constexpr float kTransformGizmoAxisPx = 110.0f;

// Point-in-convex-quad (px), for plane-quad hit-testing and drawing.
bool gizmo_point_in_quad(float mx, float my, const float q[4][2]);

// Distance from a point to a segment (px). -1 when the segment is degenerate.
float gizmo_point_to_segment(float mx, float my, float ax, float ay, float bx, float by);

} // namespace nf::editor
