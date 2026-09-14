// Tests/PhysicsTests/test_shapes.cpp — shape geometry and mass properties
//
// The inertia tensor is the one thing here that is easy to get subtly wrong and
// hard to notice: a wrong tensor still produces motion, just the wrong motion.
// So these check against the closed forms rather than against the implementation.

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/Shapes.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

NF_TEST(shape_sphere_volume_matches_the_closed_form) {
    const f32 r = 2.0f;
    const Shape s = Shape::make_sphere(r);
    NF_CHECK(s.type == ShapeType::Sphere);
    NF_CHECK_NEAR(s.volume(), (4.0f / 3.0f) * PI * r * r * r, 1e-4f);
}

NF_TEST(shape_sphere_inertia_matches_the_closed_form) {
    const f32 r = 1.5f;
    const f32 m = 7.0f;
    const Shape s = Shape::make_sphere(r);
    const Vec3 i = s.inertia_diagonal(m);
    const f32 expected = 0.4f * m * r * r; // 2/5 m r²
    // A sphere is isotropic, so all three axes must agree.
    NF_CHECK_NEAR(i.x, expected, 1e-4f);
    NF_CHECK_NEAR(i.y, expected, 1e-4f);
    NF_CHECK_NEAR(i.z, expected, 1e-4f);
}

NF_TEST(shape_box_volume_and_inertia_match_the_closed_forms) {
    const Vec3 h(0.5f, 1.0f, 2.0f);
    const f32 m = 3.0f;
    const Shape s = Shape::make_box(h);

    NF_CHECK_NEAR(s.volume(), 8.0f * h.x * h.y * h.z, 1e-4f);

    const Vec3 i = s.inertia_diagonal(m);
    NF_CHECK_NEAR(i.x, (m / 3.0f) * (h.y * h.y + h.z * h.z), 1e-4f);
    NF_CHECK_NEAR(i.y, (m / 3.0f) * (h.x * h.x + h.z * h.z), 1e-4f);
    NF_CHECK_NEAR(i.z, (m / 3.0f) * (h.x * h.x + h.y * h.y), 1e-4f);
}

NF_TEST(shape_cube_inertia_is_m_s_squared_over_six) {
    // The cube case has a second closed form worth pinning independently, so a
    // sign or factor error in the general form cannot hide.
    const f32 side = 2.0f;               // half-extent 1
    const f32 m = 12.0f;
    const Shape s = Shape::make_box(Vec3(side * 0.5f));
    const Vec3 i = s.inertia_diagonal(m);
    const f32 expected = m * side * side / 6.0f;
    NF_CHECK_NEAR(i.x, expected, 1e-4f);
    NF_CHECK_NEAR(i.y, expected, 1e-4f);
    NF_CHECK_NEAR(i.z, expected, 1e-4f);
}

NF_TEST(shape_plane_has_no_volume_and_no_inertia) {
    const Shape s = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));
    NF_CHECK(s.type == ShapeType::Plane);
    NF_CHECK(s.is_unbounded());
    NF_CHECK_NEAR(s.volume(), 0.0f, 1e-6f);
    // Zero inertia is what marks a body as immovable; the solver relies on it.
    const Vec3 i = s.inertia_diagonal(1000.0f);
    NF_CHECK_NEAR(i.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(i.y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(i.z, 0.0f, 1e-6f);
}

NF_TEST(shape_degenerate_dimensions_are_clamped) {
    // A zero radius or half-extent would make the inertia tensor singular and
    // the support function divide by zero, so the factories clamp instead of
    // letting a bad number travel into the solver.
    const Shape zero_sphere = Shape::make_sphere(0.0f);
    NF_CHECK(zero_sphere.sphere.radius > 0.0f);

    const Shape negative = Shape::make_sphere(-3.0f);
    NF_CHECK(negative.sphere.radius > 0.0f);

    const Shape flat_box = Shape::make_box(Vec3(1.0f, 0.0f, -2.0f));
    NF_CHECK(flat_box.box.half_extents.y > 0.0f);
    NF_CHECK(flat_box.box.half_extents.z > 0.0f);
    NF_CHECK_NEAR(flat_box.box.half_extents.x, 1.0f, 1e-6f);
}

NF_TEST(shape_plane_normal_is_normalized) {
    const Shape s = Shape::make_plane(Vec3(0.0f, 5.0f, 0.0f));
    NF_CHECK_NEAR(s.plane.normal.length(), 1.0f, 1e-5f);
    NF_CHECK_NEAR(s.plane.normal.y, 1.0f, 1e-5f);

    // A zero normal would leave the plane's collision direction arbitrary; it
    // falls back to up rather than producing NaNs downstream.
    const Shape degenerate = Shape::make_plane(Vec3::zero);
    NF_CHECK_NEAR(degenerate.plane.normal.length(), 1.0f, 1e-5f);
}

NF_TEST(shape_sphere_aabb_is_centred_and_radius_sized) {
    const Shape s = Shape::make_sphere(0.75f);
    Vec3 mn, mx;
    s.compute_aabb(Vec3(1.0f, 2.0f, 3.0f), Quat::identity(), mn, mx);
    NF_CHECK(mn.nearly_equals(Vec3(0.25f, 1.25f, 2.25f), 1e-5f));
    NF_CHECK(mx.nearly_equals(Vec3(1.75f, 2.75f, 3.75f), 1e-5f));
}

NF_TEST(shape_box_aabb_is_tight_when_axis_aligned) {
    const Shape s = Shape::make_box(Vec3(1.0f, 2.0f, 3.0f));
    Vec3 mn, mx;
    s.compute_aabb(Vec3::zero, Quat::identity(), mn, mx);
    NF_CHECK(mn.nearly_equals(Vec3(-1.0f, -2.0f, -3.0f), 1e-5f));
    NF_CHECK(mx.nearly_equals(Vec3(1.0f, 2.0f, 3.0f), 1e-5f));
}

NF_TEST(shape_box_aabb_grows_when_the_box_is_rotated) {
    // A 0.5 half-extent cube turned 45° about Y has an AABB half-width of
    // 0.5*sqrt(2) in x and z, and is unchanged in y. This is the case that
    // catches an AABB computed from the unrotated extents.
    const Shape s = Shape::make_box(Vec3(0.5f));
    Vec3 mn, mx;
    s.compute_aabb(Vec3::zero, Quat::from_axis_angle(Vec3::up, to_radians(45.0f)), mn, mx);

    const f32 expected = 0.5f * std::sqrt(2.0f);
    NF_CHECK_NEAR(mx.x, expected, 1e-4f);
    NF_CHECK_NEAR(mx.z, expected, 1e-4f);
    NF_CHECK_NEAR(mx.y, 0.5f, 1e-4f);
    // And it must stay symmetric about the body origin.
    NF_CHECK_NEAR(mn.x, -expected, 1e-4f);
    NF_CHECK_NEAR(mn.y, -0.5f, 1e-4f);
}

NF_TEST(shape_box_aabb_is_unchanged_by_a_full_turn) {
    // A 90° turn about Y swaps the x and z extents; a 180° restores them.
    const Shape s = Shape::make_box(Vec3(1.0f, 2.0f, 3.0f));
    Vec3 mn, mx;
    s.compute_aabb(Vec3::zero, Quat::from_axis_angle(Vec3::up, PI), mn, mx);
    NF_CHECK(mx.nearly_equals(Vec3(1.0f, 2.0f, 3.0f), 1e-4f));

    s.compute_aabb(Vec3::zero, Quat::from_axis_angle(Vec3::up, HALF_PI), mn, mx);
    NF_CHECK(mx.nearly_equals(Vec3(3.0f, 2.0f, 1.0f), 1e-4f));
}

NF_TEST(shape_sphere_support_lies_on_the_sphere) {
    const Shape s = Shape::make_sphere(2.0f);
    const Vec3 centre(1.0f, 1.0f, 1.0f);
    const Vec3 p = s.support(centre, Quat::identity(), Vec3(1.0f, 0.0f, 0.0f));
    NF_CHECK_NEAR((p - centre).length(), 2.0f, 1e-4f);
    NF_CHECK_NEAR(p.x, 3.0f, 1e-4f);
}

NF_TEST(shape_box_support_returns_the_matching_corner) {
    const Shape s = Shape::make_box(Vec3(1.0f, 2.0f, 3.0f));

    // Each direction must select the corner whose sign matches on every axis.
    const Vec3 ppp = s.support(Vec3::zero, Quat::identity(), Vec3(1.0f, 1.0f, 1.0f));
    NF_CHECK(ppp.nearly_equals(Vec3(1.0f, 2.0f, 3.0f), 1e-5f));

    const Vec3 nnn = s.support(Vec3::zero, Quat::identity(), Vec3(-1.0f, -1.0f, -1.0f));
    NF_CHECK(nnn.nearly_equals(Vec3(-1.0f, -2.0f, -3.0f), 1e-5f));

    const Vec3 mixed = s.support(Vec3::zero, Quat::identity(), Vec3(1.0f, -1.0f, 1.0f));
    NF_CHECK(mixed.nearly_equals(Vec3(1.0f, -2.0f, 3.0f), 1e-5f));
}

NF_TEST(shape_box_support_follows_the_body_orientation) {
    // Rotate 90° about Y: the local +x axis now points along world -z, so the
    // support in world +x is the corner at local -z... whichever corner it is,
    // the answer must be exactly one corner of the rotated box.
    const Shape s = Shape::make_box(Vec3(1.0f, 2.0f, 3.0f));
    const Quat q = Quat::from_axis_angle(Vec3::up, HALF_PI);
    const Vec3 p = s.support(Vec3::zero, q, Vec3(1.0f, 0.0f, 0.0f));
    // The rotated box's half-extent along world x is now 3 (the old z extent).
    NF_CHECK_NEAR(p.x, 3.0f, 1e-4f);
    NF_CHECK_NEAR(p.y, 2.0f, 1e-4f);
}

NF_TEST(shape_bounding_radius_covers_the_shape) {
    const Shape sphere = Shape::make_sphere(1.25f);
    NF_CHECK_NEAR(sphere.bounding_radius(), 1.25f, 1e-5f);

    const Shape box = Shape::make_box(Vec3(1.0f, 2.0f, 2.0f));
    // Half the AABB diagonal: sqrt(1 + 4 + 4).
    NF_CHECK_NEAR(box.bounding_radius(), 3.0f, 1e-5f);

    // An unbounded shape must report an unbounded radius so the broadphase can
    // recognise it without special-casing the type.
    const Shape plane = Shape::make_plane(Vec3::up);
    NF_CHECK(plane.bounding_radius() > 1.0e8f);
}
