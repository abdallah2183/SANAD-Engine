// Tests/PhysicsTests/test_narrowphase.cpp — contact generation
//
// Two properties are checked on every pair, not just the depths:
//   1. the normal points from A towards B (getting this backwards makes objects
//      accelerate into each other, and the resulting motion still "looks like
//      physics" in a screenshot);
//   2. a face contact produces at least three points, because a single point
//      lets a resting box pivot and tip instead of sitting still.

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/Narrowphase.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

namespace {

bool normal_is(const Manifold& m, const Vec3& expected, f32 tol = 1e-3f) {
    return m.normal.nearly_equals(expected, tol);
}

} // namespace

// --- Sphere vs sphere -------------------------------------------------------

NF_TEST(narrowphase_sphere_sphere_separated_reports_nothing) {
    Manifold m;
    NF_CHECK(!collide_sphere_sphere(1.0f, Vec3::zero, 1.0f, Vec3(3.0f, 0.0f, 0.0f), m));
    NF_CHECK_EQ(m.point_count, 0u);
}

NF_TEST(narrowphase_sphere_sphere_overlap_has_the_right_depth_and_direction) {
    Manifold m;
    NF_CHECK(collide_sphere_sphere(1.0f, Vec3::zero, 1.0f, Vec3(0.0f, 1.5f, 0.0f), m));
    NF_CHECK_EQ(m.point_count, 1u);
    // A is below B, so A -> B is up.
    NF_CHECK(normal_is(m, Vec3(0.0f, 1.0f, 0.0f)));
    NF_CHECK_NEAR(m.points[0].penetration, 0.5f, 1e-4f);
}

NF_TEST(narrowphase_sphere_sphere_exact_touch_is_not_a_contact) {
    // Touching without overlap must not generate a contact, or resting objects
    // would jitter forever.
    Manifold m;
    NF_CHECK(!collide_sphere_sphere(1.0f, Vec3::zero, 1.0f, Vec3(2.0f, 0.0f, 0.0f), m));
}

NF_TEST(narrowphase_sphere_sphere_coincident_centres_do_not_produce_nan) {
    // A degenerate case that must not poison the solver with NaNs.
    Manifold m;
    NF_CHECK(collide_sphere_sphere(1.0f, Vec3::zero, 1.0f, Vec3::zero, m));
    NF_CHECK_EQ(m.point_count, 1u);
    NF_CHECK(std::isfinite(m.normal.x) && std::isfinite(m.normal.y) && std::isfinite(m.normal.z));
    NF_CHECK_NEAR(m.normal.length(), 1.0f, 1e-4f);
    NF_CHECK_NEAR(m.points[0].penetration, 2.0f, 1e-4f);
}

// --- Sphere vs plane --------------------------------------------------------

NF_TEST(narrowphase_sphere_plane_normal_points_from_the_sphere_to_the_plane) {
    Manifold m;
    // Sphere centred at y = 0.5 with radius 1: penetrates the y = 0 plane by 0.5.
    NF_CHECK(collide_sphere_plane(1.0f, Vec3(0.0f, 0.5f, 0.0f),
                                  Vec3(0.0f, 1.0f, 0.0f), Vec3::zero, Quat::identity(), m));
    NF_CHECK_EQ(m.point_count, 1u);
    // The plane is below the sphere, so A -> B is down.
    NF_CHECK(normal_is(m, Vec3(0.0f, -1.0f, 0.0f)));
    NF_CHECK_NEAR(m.points[0].penetration, 0.5f, 1e-4f);
}

NF_TEST(narrowphase_sphere_plane_separated_reports_nothing) {
    Manifold m;
    NF_CHECK(!collide_sphere_plane(1.0f, Vec3(0.0f, 2.0f, 0.0f),
                                   Vec3(0.0f, 1.0f, 0.0f), Vec3::zero, Quat::identity(), m));
}

NF_TEST(narrowphase_sphere_plane_respects_a_rotated_plane) {
    // A plane whose local normal is +y, rotated 90 degrees about Z, points along
    // -x in world space. A sphere at x = -0.5 then penetrates it.
    const Quat rot = Quat::from_axis_angle(Vec3(0.0f, 0.0f, 1.0f), HALF_PI);
    Manifold m;
    NF_CHECK(collide_sphere_plane(1.0f, Vec3(-0.5f, 0.0f, 0.0f),
                                  Vec3(0.0f, 1.0f, 0.0f), Vec3::zero, rot, m));
    NF_CHECK_EQ(m.point_count, 1u);
    NF_CHECK_NEAR(m.points[0].penetration, 0.5f, 1e-3f);
}

// --- Box vs plane -----------------------------------------------------------

NF_TEST(narrowphase_box_plane_resting_gives_four_contacts) {
    // A box sitting flat: all four bottom corners penetrate, so the manifold
    // must carry four points. Fewer means the box will rock.
    Manifold m;
    NF_CHECK(collide_box_plane(Vec3(1.0f), Vec3(0.0f, 0.9f, 0.0f), Quat::identity(),
                               Vec3(0.0f, 1.0f, 0.0f), Vec3::zero, Quat::identity(), m));
    NF_CHECK_EQ(m.point_count, 4u);
    NF_CHECK(normal_is(m, Vec3(0.0f, -1.0f, 0.0f)));
    NF_CHECK_NEAR(m.points[0].penetration, 0.1f, 1e-4f);
    NF_CHECK_NEAR(m.max_penetration(), 0.1f, 1e-4f);
}

NF_TEST(narrowphase_box_plane_separated_reports_nothing) {
    Manifold m;
    NF_CHECK(!collide_box_plane(Vec3(1.0f), Vec3(0.0f, 1.5f, 0.0f), Quat::identity(),
                                Vec3(0.0f, 1.0f, 0.0f), Vec3::zero, Quat::identity(), m));
}

NF_TEST(narrowphase_box_plane_penetration_follows_a_rotation) {
    // Tilt a box 45 degrees about Z: its lowest corner is at -sqrt(2) from the
    // centre instead of -1, so at the same height it penetrates further.
    const Quat rot = Quat::from_axis_angle(Vec3(0.0f, 0.0f, 1.0f), to_radians(45.0f));
    Manifold m;
    NF_CHECK(collide_box_plane(Vec3(1.0f), Vec3(0.0f, 0.0f, 0.0f), rot,
                               Vec3(0.0f, 1.0f, 0.0f), Vec3::zero, Quat::identity(), m));
    NF_CHECK(m.point_count >= 1);
    NF_CHECK_NEAR(m.max_penetration(), std::sqrt(2.0f), 1e-3f);
}

// --- Sphere vs box ----------------------------------------------------------

NF_TEST(narrowphase_sphere_box_face_contact) {
    // Sphere above a unit box's top face. Box A at origin (half 1), sphere B at
    // y = 1.5 with radius 1: it overlaps the top face by 0.5.
    Manifold m;
    NF_CHECK(collide_sphere_box(1.0f, Vec3(0.0f, 1.5f, 0.0f),
                                Vec3(1.0f), Vec3::zero, Quat::identity(), m));
    NF_CHECK_EQ(m.point_count, 1u);
    // A is the sphere, B the box: A -> B points down, into the box.
    NF_CHECK(normal_is(m, Vec3(0.0f, -1.0f, 0.0f)));
    NF_CHECK_NEAR(m.points[0].penetration, 0.5f, 1e-4f);
}

NF_TEST(narrowphase_sphere_box_separated_reports_nothing) {
    Manifold m;
    NF_CHECK(!collide_sphere_box(0.5f, Vec3(0.0f, 3.0f, 0.0f),
                                 Vec3(1.0f), Vec3::zero, Quat::identity(), m));
}

NF_TEST(narrowphase_sphere_box_corner_contact) {
    // Diagonally off a corner: the closest feature is the corner, and the
    // penetration is measured along the diagonal, not along a face normal. The
    // centre sits 0.5*sqrt(3) from the corner, so the radius has to exceed that
    // for there to be a contact at all.
    const Vec3 sphere_pos(1.5f, 1.5f, 1.5f);
    const f32 corner_distance = 0.5f * std::sqrt(3.0f);
    Manifold m;
    NF_CHECK(collide_sphere_box(1.0f, sphere_pos, Vec3(1.0f), Vec3::zero, Quat::identity(), m));
    NF_CHECK_EQ(m.point_count, 1u);
    NF_CHECK_NEAR(m.points[0].penetration, 1.0f - corner_distance, 1e-3f);
    // The normal is the diagonal direction, not a face axis.
    NF_CHECK(normal_is(m, -Vec3(1.0f, 1.0f, 1.0f).normalized(), 1e-3f));

    // Just out of reach is not a contact.
    Manifold none;
    NF_CHECK(!collide_sphere_box(corner_distance - 0.01f, sphere_pos, Vec3(1.0f),
                                 Vec3::zero, Quat::identity(), none));
}

NF_TEST(narrowphase_sphere_box_centre_inside_exits_the_nearest_face) {
    // A sphere whose centre is inside the box has no closest-point direction, so
    // the implementation must pick the nearest face. Near the +y face here.
    Manifold m;
    NF_CHECK(collide_sphere_box(0.25f, Vec3(0.0f, 0.8f, 0.0f),
                                Vec3(1.0f), Vec3::zero, Quat::identity(), m));
    NF_CHECK_EQ(m.point_count, 1u);
    // Nearest face is +y (0.2 away) versus 1.0 on x and z, so A -> B is down.
    NF_CHECK(normal_is(m, Vec3(0.0f, -1.0f, 0.0f)));
    // To clear the box the centre must travel 0.2 to reach the face plus 0.25 to
    // push the surface out: radius + nearest-face distance.
    NF_CHECK_NEAR(m.points[0].penetration, 0.25f + 0.2f, 1e-3f);
}

NF_TEST(narrowphase_sphere_box_respects_a_rotated_box) {
    // Rotate the box 90 degrees about Z so its local +y axis points along world
    // -x. A sphere at world +x = 1.5 then hits that face.
    const Quat rot = Quat::from_axis_angle(Vec3(0.0f, 0.0f, 1.0f), HALF_PI);
    Manifold m;
    NF_CHECK(collide_sphere_box(0.5f, Vec3(1.4f, 0.0f, 0.0f),
                                Vec3(1.0f), Vec3::zero, rot, m));
    NF_CHECK_EQ(m.point_count, 1u);
    NF_CHECK_NEAR(m.points[0].penetration, 0.1f, 1e-3f);
}

// --- Box vs box -------------------------------------------------------------

NF_TEST(narrowphase_box_box_separated_reports_nothing) {
    Manifold m;
    NF_CHECK(!collide_box_box(Vec3(1.0f), Vec3::zero, Quat::identity(),
                              Vec3(1.0f), Vec3(5.0f, 0.0f, 0.0f), Quat::identity(), m));
    NF_CHECK_EQ(m.point_count, 0u);
}

NF_TEST(narrowphase_box_box_stacked_faces_give_a_full_manifold) {
    // Box B resting on box A with 0.1 of overlap. This is the case that decides
    // whether a stack stands up: fewer than three points and the upper box
    // pivots and topples.
    Manifold m;
    NF_CHECK(collide_box_box(Vec3(1.0f), Vec3::zero, Quat::identity(),
                             Vec3(1.0f), Vec3(0.0f, 1.9f, 0.0f), Quat::identity(), m));
    NF_CHECK(m.point_count >= 3);
    NF_CHECK(normal_is(m, Vec3(0.0f, 1.0f, 0.0f)));
    NF_CHECK_NEAR(m.max_penetration(), 0.1f, 1e-3f);
}

NF_TEST(narrowphase_box_box_side_contact_normal_points_a_to_b) {
    Manifold m;
    NF_CHECK(collide_box_box(Vec3(1.0f), Vec3::zero, Quat::identity(),
                             Vec3(1.0f), Vec3(1.9f, 0.0f, 0.0f), Quat::identity(), m));
    NF_CHECK(m.point_count >= 3);
    NF_CHECK(normal_is(m, Vec3(1.0f, 0.0f, 0.0f)));
    NF_CHECK_NEAR(m.max_penetration(), 0.1f, 1e-3f);
}

NF_TEST(narrowphase_box_box_penetration_is_the_smallest_axis_overlap) {
    // Deeply overlapped on two axes; SAT must report the axis of least
    // penetration, because that is the direction that separates them cheapest.
    Manifold m;
    NF_CHECK(collide_box_box(Vec3(1.0f), Vec3::zero, Quat::identity(),
                             Vec3(1.0f), Vec3(0.0f, 0.25f, 1.5f), Quat::identity(), m));
    // y overlap = 2 - 0.25 = 1.75, z overlap = 2 - 1.5 = 0.5. So z wins.
    NF_CHECK(normal_is(m, Vec3(0.0f, 0.0f, 1.0f), 1e-3f));
    NF_CHECK_NEAR(m.max_penetration(), 0.5f, 1e-3f);
}

NF_TEST(narrowphase_box_box_rotated_box_still_collides) {
    // A 45-degree box dropped onto an axis-aligned one: the contact is an edge
    // or a face, but it must be found and the normal must be finite.
    const Quat rot = Quat::from_axis_angle(Vec3(0.0f, 0.0f, 1.0f), to_radians(45.0f));
    Manifold m;
    NF_CHECK(collide_box_box(Vec3(1.0f), Vec3::zero, Quat::identity(),
                             Vec3(1.0f), Vec3(0.0f, 1.5f, 0.0f), rot, m));
    NF_CHECK(m.point_count >= 1);
    NF_CHECK_NEAR(m.normal.length(), 1.0f, 1e-4f);
    NF_CHECK(m.max_penetration() > 0.0f);
}

NF_TEST(narrowphase_box_box_just_touching_is_not_a_contact) {
    Manifold m;
    NF_CHECK(!collide_box_box(Vec3(1.0f), Vec3::zero, Quat::identity(),
                              Vec3(1.0f), Vec3(2.0f, 0.0f, 0.0f), Quat::identity(), m));
}

// --- Manifold bookkeeping ---------------------------------------------------

NF_TEST(narrowphase_manifold_keeps_the_deepest_four_points) {
    Manifold m;
    for (u32 i = 0; i < 8; ++i) {
        m.add_point(Vec3(static_cast<f32>(i), 0.0f, 0.0f), static_cast<f32>(i), i);
    }
    NF_CHECK_EQ(m.point_count, Manifold::kMaxPoints);
    // Depths 4..7 survive; the shallow ones are evicted.
    NF_CHECK_NEAR(m.max_penetration(), 7.0f, 1e-5f);
    for (u32 i = 0; i < m.point_count; ++i) {
        NF_CHECK(m.points[i].penetration >= 4.0f);
    }
}

// --- Dispatcher -------------------------------------------------------------

NF_TEST(narrowphase_dispatcher_agrees_with_the_pair_functions) {
    const Shape sphere = Shape::make_sphere(1.0f);
    const Shape box = Shape::make_box(Vec3(1.0f));
    const Shape plane = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));

    Manifold direct, dispatched;
    NF_CHECK(collide_sphere_sphere(1.0f, Vec3::zero, 1.0f, Vec3(0.0f, 1.5f, 0.0f), direct));
    NF_CHECK(collide(sphere, Vec3::zero, Quat::identity(),
                     sphere, Vec3(0.0f, 1.5f, 0.0f), Quat::identity(), dispatched));
    NF_CHECK_EQ(dispatched.point_count, direct.point_count);
    NF_CHECK(normal_is(dispatched, direct.normal));

    Manifold box_box_direct, box_box_dispatched;
    NF_CHECK(collide_box_box(Vec3(1.0f), Vec3::zero, Quat::identity(),
                             Vec3(1.0f), Vec3(0.0f, 1.9f, 0.0f), Quat::identity(), box_box_direct));
    NF_CHECK(collide(box, Vec3::zero, Quat::identity(),
                     box, Vec3(0.0f, 1.9f, 0.0f), Quat::identity(), box_box_dispatched));
    NF_CHECK_EQ(box_box_dispatched.point_count, box_box_direct.point_count);

    Manifold plane_direct, plane_dispatched;
    NF_CHECK(collide_sphere_plane(1.0f, Vec3(0.0f, 0.5f, 0.0f),
                                  Vec3(0.0f, 1.0f, 0.0f), Vec3::zero, Quat::identity(), plane_direct));
    NF_CHECK(collide(sphere, Vec3(0.0f, 0.5f, 0.0f), Quat::identity(),
                     plane, Vec3::zero, Quat::identity(), plane_dispatched));
    NF_CHECK_EQ(plane_dispatched.point_count, plane_direct.point_count);
    NF_CHECK(normal_is(plane_dispatched, plane_direct.normal));
}

NF_TEST(narrowphase_dispatcher_flips_the_normal_when_operands_swap) {
    // The normal is defined as A -> B, so swapping the arguments must negate it.
    // This is what catches an inconsistent convention between the pair
    // functions, which is otherwise invisible until objects fly apart.
    const Shape sphere = Shape::make_sphere(1.0f);
    const Shape box = Shape::make_box(Vec3(1.0f));
    const Shape plane = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));

    struct Case {
        const Shape& a;
        const Shape& b;
        Vec3 pa, pb;
    };
    const Case cases[3] = {
        {sphere, box,   Vec3(0.0f, 1.5f, 0.0f), Vec3::zero},
        {box,    sphere, Vec3::zero, Vec3(0.0f, 1.5f, 0.0f)},
        {sphere, plane, Vec3(0.0f, 0.5f, 0.0f), Vec3::zero},
    };

    for (const Case& c : cases) {
        Manifold forward, backward;
        const bool hit_f = collide(c.a, c.pa, Quat::identity(), c.b, c.pb, Quat::identity(), forward);
        const bool hit_b = collide(c.b, c.pb, Quat::identity(), c.a, c.pa, Quat::identity(), backward);
        NF_CHECK(hit_f);
        NF_CHECK(hit_b);
        if (hit_f && hit_b) {
            NF_CHECK(forward.normal.nearly_equals(-backward.normal, 1e-3f));
            NF_CHECK_EQ(forward.point_count, backward.point_count);
        }
    }
}

NF_TEST(narrowphase_plane_vs_plane_reports_nothing) {
    // Two infinite planes have no meaningful contact; reporting one would put a
    // normal in the solver with no sensible direction.
    const Shape plane = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));
    Manifold m;
    NF_CHECK(!collide(plane, Vec3::zero, Quat::identity(),
                      plane, Vec3(0.0f, 1.0f, 0.0f), Quat::identity(), m));
}

NF_TEST(narrowphase_every_contact_normal_is_unit_length) {
    // A non-unit normal silently scales every impulse the solver computes.
    const Shape sphere = Shape::make_sphere(0.7f);
    const Shape box = Shape::make_box(Vec3(0.5f, 1.0f, 1.5f));
    const Shape plane = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));
    const Shape shapes[3] = {sphere, box, plane};

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (shapes[i].type == ShapeType::Plane && shapes[j].type == ShapeType::Plane) {
                continue;
            }
            Manifold m;
            const Quat rot = Quat::from_axis_angle(Vec3(0.2f, 1.0f, 0.3f), to_radians(23.0f));
            if (collide(shapes[i], Vec3::zero, rot, shapes[j], Vec3(0.3f, 0.4f, 0.2f), Quat::identity(), m)) {
                NF_CHECK_NEAR(m.normal.length(), 1.0f, 1e-4f);
                NF_CHECK(m.point_count > 0);
            }
        }
    }
}
