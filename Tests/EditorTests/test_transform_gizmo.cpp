// Tests/EditorTests/test_transform_gizmo.cpp — in-viewport transform gizmo.
//
// The gizmo's ImGui drawing is not testable headless; everything it depends
// on is: handle classification, ray/line + ray/plane drag primitives, ring
// angles, screen-space scale factors, arm layout + hit-testing, and the
// arbitrary-axis / per-axis extensions to the gizmo delta model. A bug in any
// of these shows up as a handle that grabs the wrong axis or a drag that
// moves the wrong way — exactly what these pin.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Gizmo.hpp>
#include <NF/Editor/TransformGizmo.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>

using namespace nf;
using namespace nf::editor;

namespace {

constexpr float kPi = 3.14159265358979323846f;

Ray straight_ray() {
    Ray r;
    r.ox = 0.0f;
    r.oy = 0.0f;
    r.oz = 5.0f;
    r.dx = 0.0f;
    r.dy = 0.0f;
    r.dz = -1.0f;
    return r;
}

} // namespace

NF_TEST(gizmo_handle_classification) {
    int axis = -1;
    NF_CHECK(gizmo_handle_axis(GizmoHandle::AxisX, axis) && axis == 0);
    NF_CHECK(gizmo_handle_axis(GizmoHandle::AxisY, axis) && axis == 1);
    NF_CHECK(gizmo_handle_axis(GizmoHandle::AxisZ, axis) && axis == 2);
    NF_CHECK(!gizmo_handle_axis(GizmoHandle::Center, axis));
    NF_CHECK(!gizmo_handle_axis(GizmoHandle::None, axis));
    int a = -1, b = -1, n = -1;
    NF_CHECK(gizmo_handle_plane(GizmoHandle::PlaneXY, a, b, n) && a == 0 && b == 1 && n == 2);
    NF_CHECK(gizmo_handle_plane(GizmoHandle::PlaneXZ, a, b, n) && a == 0 && b == 2 && n == 1);
    NF_CHECK(gizmo_handle_plane(GizmoHandle::PlaneYZ, a, b, n) && a == 1 && b == 2 && n == 0);
    NF_CHECK(!gizmo_handle_plane(GizmoHandle::AxisX, a, b, n));
}

NF_TEST(gizmo_axis_param_tracks_along_the_line) {
    // The returned value is the parameter ALONG THE AXIS (s), not the distance
    // along the ray. This test pins that: the case below is symmetric in both
    // directions, so the two quantities are not interchangeable — the old
    // expression returned the ray parameter, which scaled an axis drag by the
    // angle between the pointer ray and the axis. Dragging toward one screen
    // end therefore moved the object slowly and dragging the other way moved
    // it fast (and diverged as the ray neared parallel), which is the
    // "forward works, backward doesn't" report.
    float s = 0.0f;
    // Ray straight down -Z from (0,0,5); X axis through the origin. The
    // closest point on the axis IS the origin, so s = 0 (the ray parameter
    // would have been 5 — the number this test used to assert).
    NF_CHECK(gizmo_axis_param(straight_ray(), 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s));
    NF_CHECK_NEAR(s, 0.0f, 1e-5f);

    // Same ray against the Z axis (parallel): no tracking possible.
    NF_CHECK(!gizmo_axis_param(straight_ray(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, s));

    // Offset axis: X axis through (0,2,0), ray at x=0. Still the origin of the
    // axis, so s = 0.
    Ray r = straight_ray();
    NF_CHECK(gizmo_axis_param(r, 0.0f, 2.0f, 0.0f, 1.0f, 0.0f, 0.0f, s));
    NF_CHECK_NEAR(s, 0.0f, 1e-5f);

    // A ray that genuinely crosses the axis away from its origin: from
    // (2,0,5) heading down -Z, closest approach to the X axis is (2,0,0) —
    // two units ALONG the axis, not five units along the ray.
    Ray shifted = straight_ray();
    shifted.ox = 2.0f;
    NF_CHECK(gizmo_axis_param(shifted, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s));
    NF_CHECK_NEAR(s, 2.0f, 1e-5f);
}

// The axis drag must be symmetric: the same pointer displacement must move the
// object the same distance in either direction, and the sign must follow the
// pointer. This is the property the ray-parameter bug broke, so it is asserted
// directly rather than through a single expected value.
NF_TEST(gizmo_axis_param_is_symmetric_about_the_press_point) {
    // Camera on +Z looking at the origin, pointer rays fanning out in the X
    // direction (the common "drag the X arrow sideways" gesture).
    auto ray_at = [](float ndc_x) {
        const float t = ndc_x * std::tan(30.0f * kPi / 180.0f);
        Ray r;
        r.ox = 0.0f;
        r.oy = 0.0f;
        r.oz = 5.0f;
        r.dx = t;
        r.dy = 0.0f;
        r.dz = -1.0f;
        const float len = std::sqrt(r.dx * r.dx + r.dz * r.dz);
        r.dx /= len;
        r.dz /= len;
        return r;
    };
    float s_pos = 0.0f, s_neg = 0.0f, s_zero = 0.0f;
    NF_CHECK(gizmo_axis_param(ray_at(0.0f), 0, 0, 0, 1, 0, 0, s_zero));
    NF_CHECK(gizmo_axis_param(ray_at(0.2f), 0, 0, 0, 1, 0, 0, s_pos));
    NF_CHECK(gizmo_axis_param(ray_at(-0.2f), 0, 0, 0, 1, 0, 0, s_neg));
    // Symmetric about the press point: the two deltas are equal in magnitude.
    NF_CHECK_NEAR(s_pos - s_zero, -(s_neg - s_zero), 1e-4f);
    // And the sign follows the pointer: right -> +X, left -> -X.
    NF_CHECK(s_pos > s_zero);
    NF_CHECK(s_neg < s_zero);
}

NF_TEST(gizmo_plane_point_hits_where_expected) {
    float p[3]{};
    NF_CHECK(
        gizmo_plane_point(straight_ray(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, p));
    NF_CHECK_NEAR(p[0], 0.0f, 1e-5f);
    NF_CHECK_NEAR(p[1], 0.0f, 1e-5f);
    NF_CHECK_NEAR(p[2], 0.0f, 1e-5f);
    // A plane edge-on to the ray (normal X, ray along -Z) never hits.
    NF_CHECK(!gizmo_plane_point(straight_ray(), 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, p));
}

NF_TEST(gizmo_ring_angle_quarter_turn) {
    NF_CHECK_NEAR(gizmo_ring_angle(1, 0, 0, 0, 1, 0, 0, 0, 1), kPi / 2.0f, 1e-5f);
    NF_CHECK_NEAR(gizmo_ring_angle(1, 0, 0, 0, -1, 0, 0, 0, 1), -kPi / 2.0f, 1e-5f);
    NF_CHECK_NEAR(gizmo_ring_angle(1, 0, 0, 1, 0, 0, 0, 0, 1), 0.0f, 1e-5f);
}

NF_TEST(gizmo_scale_factors) {
    // Uniform: pointer drifts 10px outward from a 100px radius -> 1.10x.
    NF_CHECK_NEAR(gizmo_uniform_factor(0, 0, 100, 0, 110, 0, 100), 1.10f, 1e-5f);
    NF_CHECK_NEAR(gizmo_uniform_factor(0, 0, 100, 0, 90, 0, 100), 0.90f, 1e-5f);
    // Axis: 20px along a 100px arm -> 1.20x; perpendicular motion is ignored.
    NF_CHECK_NEAR(gizmo_axis_factor(0, 0, 20, 0, 1, 0, 100), 1.20f, 1e-5f);
    NF_CHECK_NEAR(gizmo_axis_factor(0, 0, 0, 20, 1, 0, 100), 1.0f, 1e-5f);
    // Degenerate lengths never divide by zero.
    NF_CHECK_NEAR(gizmo_uniform_factor(0, 0, 0, 0, 1, 1, 0), 1.0f, 1e-5f);
    NF_CHECK_NEAR(gizmo_axis_factor(0, 0, 5, 5, 1, 0, 0), 1.0f, 1e-5f);
}

NF_TEST(gizmo_arm_layout_identity_projection) {
    // Identity view-projection passes world straight to NDC, so the layout is
    // hand-computable: origin (0,0,0) -> (100,50) in a 200x100 rect, +X tip
    // (1,0,0) -> (200,50), +Y tip (0,1,0) -> (100,100).
    //
    // +Y lands on the LARGER row because render NDC is y-DOWN and the NDC -> px
    // mapping is therefore linear (Mat4::perspective already negates m[1][1];
    // the old extra 0.5 - ny * 0.5 flip mirrored the gizmo against the image).
    // Under a REAL projection world +Y therefore lands in the UPPER half — see
    // gizmo_projection_agrees_with_the_pointer_row, which pins that end.
    const Mat4 vp = Mat4::identity();
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float view_dir[3] = {0, 0, -1};
    GizmoArmLayout l;
    NF_CHECK(gizmo_arm_layout(vp, 0, 0, 200, 100, 0, 0, 0, axes, 1.0f, view_dir, l));
    NF_CHECK_NEAR(l.ox, 100.0f, 1e-4f);
    NF_CHECK_NEAR(l.oy, 50.0f, 1e-4f);
    NF_CHECK_NEAR(l.tip_x[0], 200.0f, 1e-4f);
    NF_CHECK_NEAR(l.tip_y[0], 50.0f, 1e-4f);
    NF_CHECK_NEAR(l.tip_x[1], 100.0f, 1e-4f);
    NF_CHECK_NEAR(l.tip_y[1], 100.0f, 1e-4f);
    // Z aims at the eye here (view -Z, axis +Z): its depth sorts last (near).
    NF_CHECK(l.order[2] == 2);
}

// The gizmo must land on the SAME pixel row as the framebuffer put the object,
// and the pointer must address that same row. Both sides are derived from ONE
// projection here, so the assertion cannot be satisfied by two conventions that
// happen to agree with each other while disagreeing with the image.
//
// The convention, stated once:
//   * the render NDC is y-DOWN (Mat4::perspective negates m[1][1] for Vulkan):
//     world +Y -> NDC y < 0 -> memory row 0 -> the TOP of the presented image;
//   * so render-NDC -> image pixel is linear: px = ny * 0.5 + 0.5;
//   * the POINTER NDC is y-UP (Panels' to_ndc maps the image top to +1), and
//     viewport_ndc_to_pixel() bridges it with (0.5 - n * 0.5).
//
// The gizmo used to apply that bridge a second time, so its whole layout was
// mirrored vertically against the render: handles floated off their object,
// hover missed the handle under the pointer, and an axis drag inverted. This is
// the "the gizmo is not on the object" report, pinned from both sides.
NF_TEST(gizmo_projection_agrees_with_the_pointer_row) {
    const float fov = 60.0f * kPi / 180.0f;
    // A real perspective camera looking down -Z, the same one the drag fixture
    // uses, so the numbers below are the numbers the editor actually sees.
    const Mat4 view = Mat4::look_at(Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f},
                                    Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = Mat4::perspective(fov, 1.0f, 0.1f, 100.0f);
    const Mat4 vp = view * proj;

    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float view_dir[3] = {0, 0, -1};
    GizmoArmLayout l;
    // The object sits at y = 2.5, clearly ABOVE the view centre.
    NF_CHECK(gizmo_arm_layout(vp, 0, 0, 100, 100, 0, 2.5f, 0, axes, 0.5f, view_dir, l));

    // Framebuffer side: project the same point, then map the render NDC to the
    // image row linearly (that IS what the renderer + ImGui::Image do).
    const Vec4 clip = vp * Vec4{0.0f, 2.5f, 0.0f, 1.0f};
    NF_CHECK(clip.w > 1e-5f);
    const float render_ndc_y = clip.y / clip.w;
    const float render_ndc_x = clip.x / clip.w;
    // Above the centre must project to the UPPER half of the row range.
    NF_CHECK(render_ndc_y < 0.0f);
    const float img_row = (render_ndc_y * 0.5f + 0.5f) * 100.0f;
    const float img_col = (render_ndc_x * 0.5f + 0.5f) * 100.0f;
    NF_CHECK(img_row < 50.0f);

    // The gizmo layout must equal that row, to the pixel.
    NF_CHECK_NEAR(l.oy, img_row, 1e-3f);
    NF_CHECK_NEAR(l.ox, img_col, 1e-3f);

    // And the POINTER must reach that same row for a click on that pixel.
    // Panels::to_ndc is the inverse here: fy = row / height, ndc_y = 1 - 2*fy.
    float ptr_col = 0.0f, ptr_row = 0.0f;
    // Panels::to_ndc is the inverse: fy = row / height, ndc_y = 1 - 2 * fy.
    // to_ndc's x is 2 * fx - 1 with fx = col / width, so the same inversion.
    const float pointer_ndc_y = 1.0f - 2.0f * (img_row / 100.0f);
    const float pointer_ndc_x = 2.0f * (img_col / 100.0f) - 1.0f;
    viewport_ndc_to_pixel(pointer_ndc_x, pointer_ndc_y, 100.0f, 100.0f, ptr_col, ptr_row);
    NF_CHECK_NEAR(ptr_row, img_row, 1e-3f);
    // X round-trips too: the click column must stay on the object.
    NF_CHECK_NEAR(ptr_col, img_col, 1e-3f);
}

NF_TEST(gizmo_hit_arms_priorities) {
    const Mat4 vp = Mat4::identity();
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float view_dir[3] = {0, 0, -1};
    GizmoArmLayout l;
    NF_CHECK(gizmo_arm_layout(vp, 0, 0, 200, 100, 0, 0, 0, axes, 1.0f, view_dir, l));
    // Tip box wins over the shaft behind it.
    NF_CHECK(gizmo_hit_arms(l, 200, 50, 6, 8, 0, 0, false, GizmoMode::Scale) ==
             GizmoHandle::AxisX);
    // Mid-shaft grabs the axis.
    NF_CHECK(gizmo_hit_arms(l, 150, 50, 6, 0, 0, 0, false, GizmoMode::Scale) ==
             GizmoHandle::AxisX);
    // The Y shaft, not the X one (the Y arm now runs toward the larger row —
    // see gizmo_arm_layout_identity_projection).
    NF_CHECK(gizmo_hit_arms(l, 100, 75, 6, 0, 0, 0, false, GizmoMode::Scale) ==
             GizmoHandle::AxisY);
    // Center box.
    NF_CHECK(gizmo_hit_arms(l, 100, 50, 6, 0, 7, 0, false, GizmoMode::Scale) ==
             GizmoHandle::Center);
    // Empty corner misses everything.
    NF_CHECK(gizmo_hit_arms(l, 10, 90, 6, 8, 7, 0.35f, false, GizmoMode::Scale) ==
             GizmoHandle::None);
    // Inside the XY plane quad (translate only): ~35% along each arm from
    // the origin is (135,68) — inside the quad, off both shafts.
    NF_CHECK(gizmo_hit_arms(l, 130, 62, 6, 0, 0, 0.35f, true, GizmoMode::Translate) ==
             GizmoHandle::PlaneXY);
}

NF_TEST(gizmo_hit_rings_circle) {
    // Identity projection: the Z ring is the unit circle on screen centered
    // at (100,50). Probe at 45 degrees (no other ring passes there — rings
    // only meet at the axis tips, so tips are useless as probes).
    const Mat4 vp = Mat4::identity();
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    // cos45 * 100 + 100, 50 + sin45 * 50 (the Y half runs toward larger rows).
    NF_CHECK(gizmo_hit_rings(vp, 0, 0, 200, 100, 0, 0, 0, axes, 1.0f, 170.71f, 85.36f,
                             8.0f) == GizmoHandle::AxisZ);
    // Far from every ring: miss.
    NF_CHECK(gizmo_hit_rings(vp, 0, 0, 200, 100, 0, 0, 0, axes, 1.0f, 10, 10, 8.0f) ==
             GizmoHandle::None);
}

NF_TEST(gizmo_world_length_scales_with_distance) {
    const float near = gizmo_world_length(110, 800, 5.0f, 60.0f * kPi / 180.0f);
    const float far = gizmo_world_length(110, 800, 10.0f, 60.0f * kPi / 180.0f);
    NF_CHECK_NEAR(far, near * 2.0f, 1e-5f);
    NF_CHECK(near > 0.0f);
}

NF_TEST(gizmo_delta_axis_angle_rotates_about_z) {
    scene::Transform base;
    GizmoDelta d;
    d.axis_x = 0.0f;
    d.axis_y = 0.0f;
    d.axis_z = 1.0f;
    d.axis_angle_deg = 90.0f;
    const scene::Transform out =
        apply_gizmo_delta(base, d, GizmoMode::Rotate, GizmoSpace::World, GizmoSnap{});
    // Single-axis rotation round-trips through the euler pair.
    NF_CHECK_NEAR(out.rot_x, 0.0f, 1e-3f);
    NF_CHECK_NEAR(out.rot_y, 0.0f, 1e-3f);
    NF_CHECK_NEAR(out.rot_z, 90.0f, 1e-3f);
    // Zero angle is a no-op: the base comes back bit-exact.
    GizmoDelta z;
    const scene::Transform same =
        apply_gizmo_delta(base, z, GizmoMode::Rotate, GizmoSpace::World, GizmoSnap{});
    NF_CHECK(same.rot_x == base.rot_x && same.rot_y == base.rot_y && same.rot_z == base.rot_z);
}

NF_TEST(gizmo_delta_per_axis_scale) {
    scene::Transform base;
    GizmoDelta d;
    d.scl_x = 1.0f; // 2x on X, the rest untouched
    const scene::Transform out =
        apply_gizmo_delta(base, d, GizmoMode::Scale, GizmoSpace::World, GizmoSnap{});
    NF_CHECK_NEAR(out.scale_x, 2.0f, 1e-5f);
    NF_CHECK_NEAR(out.scale_y, 1.0f, 1e-5f);
    NF_CHECK_NEAR(out.scale_z, 1.0f, 1e-5f);
    // Snapping applies to the factor grid: 1 + 0.3 = 1.3 -> 1.5 on a 0.5 grid.
    GizmoDelta s;
    s.scl_y = 0.3f;
    GizmoSnap snap;
    snap.scale_step = 0.5f;
    const scene::Transform snapped = apply_gizmo_delta(base, s, GizmoMode::Scale,
                                                       GizmoSpace::World, snap);
    NF_CHECK_NEAR(snapped.scale_y, 1.5f, 1e-5f);
    // Uniform still works alongside.
    GizmoDelta u;
    u.dscale = 1.0f;
    const scene::Transform uni =
        apply_gizmo_delta(base, u, GizmoMode::Scale, GizmoSpace::World, GizmoSnap{});
    NF_CHECK_NEAR(uni.scale_x, 2.0f, 1e-5f);
    NF_CHECK_NEAR(uni.scale_y, 2.0f, 1e-5f);
    NF_CHECK_NEAR(uni.scale_z, 2.0f, 1e-5f);
}

NF_TEST(gizmo_point_quad_and_segment_helpers) {
    const float q[4][2] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    NF_CHECK(gizmo_point_in_quad(5, 5, q));
    NF_CHECK(!gizmo_point_in_quad(15, 5, q));
    NF_CHECK_NEAR(gizmo_point_to_segment(5, 3, 0, 0, 10, 0), 3.0f, 1e-5f);
    NF_CHECK(gizmo_point_to_segment(0, 0, 1, 1, 1, 1) < 0.0f); // degenerate
}
