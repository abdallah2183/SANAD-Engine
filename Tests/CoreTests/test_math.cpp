// CoreTests/test_math.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Math.hpp>

using namespace nf;

NF_TEST(test_vec2_basic) {
    Vec2 a(1, 2);
    Vec2 b(3, 4);

    auto c = a + b;
    NF_CHECK_EQ(c.x, 4.0f);
    NF_CHECK_EQ(c.y, 6.0f);

    auto d = a * 2.0f;
    NF_CHECK_EQ(d.x, 2.0f);
    NF_CHECK_EQ(d.y, 4.0f);

    NF_CHECK_NEAR(a.dot(b), 11.0f, EPSILON);
}

NF_TEST(test_vec3_cross) {
    Vec3 x(1, 0, 0);
    Vec3 y(0, 1, 0);
    Vec3 z = x.cross(y);
    NF_CHECK_NEAR(z.x, 0.0f, EPSILON);
    NF_CHECK_NEAR(z.y, 0.0f, EPSILON);
    NF_CHECK_NEAR(z.z, 1.0f, EPSILON);
}

NF_TEST(test_vec3_normalize) {
    Vec3 v(3, 4, 0);
    Vec3 n = v.normalized();
    NF_CHECK_NEAR(n.length(), 1.0f, EPSILON);
}

NF_TEST(test_mat4_identity) {
    Mat4 m = Mat4::identity();
    Vec4 v(1, 2, 3, 1);
    Vec4 r = m * v;
    NF_CHECK_NEAR(r.x, 1.0f, EPSILON);
    NF_CHECK_NEAR(r.y, 2.0f, EPSILON);
    NF_CHECK_NEAR(r.z, 3.0f, EPSILON);
}

NF_TEST(test_mat4_translate) {
    Mat4 t = Mat4::translate({1, 2, 3});
    Vec3 p(0, 0, 0);
    Vec3 r = t.transform_point(p);
    NF_CHECK_NEAR(r.x, 1.0f, EPSILON);
    NF_CHECK_NEAR(r.y, 2.0f, EPSILON);
    NF_CHECK_NEAR(r.z, 3.0f, EPSILON);
}

NF_TEST(test_mat4_perspective) {
    Mat4 p = Mat4::perspective(to_radians(60.0f), 16.0f/9.0f, 0.1f, 100.0f);
    // Just check it doesn't produce NaN
    Vec3 point(0, 0, -5);
    Vec3 projected = p.transform_point(point);
    NF_CHECK(projected.x == projected.x); // not NaN
}

NF_TEST(test_mat4_look_at_points_down_minus_z) {
    // Absolute anchor: eye at the origin looking down -Z with up +Y must be
    // the identity rotation. (A typo once wrote the up vector's z into
    // m[2][2] instead of m[1][2]; with the usual up=(0,1,0) that reads back
    // as row 1 == (0,1,0) either way, so only absolute, tilted cases below
    // can catch a wrong rotation block.)
    Mat4 v = Mat4::look_at({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    NF_CHECK_NEAR(v.m[0][0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(v.m[1][1], 1.0f, 1e-6f);
    NF_CHECK_NEAR(v.m[2][2], 1.0f, 1e-6f);
    NF_CHECK_NEAR(v.m[0][1], 0.0f, 1e-6f);
    NF_CHECK_NEAR(v.m[1][0], 0.0f, 1e-6f);
    NF_CHECK_NEAR(v.m[1][2], 0.0f, 1e-6f);
    NF_CHECK_NEAR(v.m[2][1], 0.0f, 1e-6f);

    // Eye at (0,0,5) looking at the origin: the world origin must land at
    // view-space (0,0,-5).
    Mat4 v2 = Mat4::look_at({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    Vec3 moved = v2.transform_point({0, 0, 0});
    NF_CHECK_NEAR(moved.x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(moved.y, 0.0f, 1e-5f);
    NF_CHECK_NEAR(moved.z, -5.0f, 1e-5f);

    // Tilted up gets orthonormalized, not passed through: row 1 is the
    // orthonormalized up's transpose slot (s.y, u.y, -f.y), here (0,1,0).
    Mat4 v3 = Mat4::look_at({0, 0, 5}, {0, 0, 0}, {0, 1, 1});
    NF_CHECK_NEAR(v3.m[1][0], 0.0f, 1e-5f);
    NF_CHECK_NEAR(v3.m[1][1], 1.0f, 1e-5f);
    NF_CHECK_NEAR(v3.m[1][2], 0.0f, 1e-5f);

    // Genuinely tilted view: the eye must map to the origin and the target
    // onto the -Z axis. An untransposed rotation block (rows holding the
    // basis instead of its transpose) passes every axis-aligned case and
    // fails exactly here.
    Mat4 v4 = Mat4::look_at({1, 2, 3}, {0, 0, 0}, {0, 1, 0});
    Vec3 eye_at_origin = v4.transform_point({1, 2, 3});
    NF_CHECK_NEAR(eye_at_origin.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(eye_at_origin.y, 0.0f, 1e-4f);
    NF_CHECK_NEAR(eye_at_origin.z, 0.0f, 1e-4f);
    Vec3 target_on_axis = v4.transform_point({0, 0, 0});
    const float dist = std::sqrt(1.0f + 4.0f + 9.0f);
    NF_CHECK_NEAR(target_on_axis.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(target_on_axis.y, 0.0f, 1e-4f);
    NF_CHECK_NEAR(target_on_axis.z, -dist, 1e-4f);
}

NF_TEST(test_mat4_perspective_maps_near_to_zero_far_to_one) {
    // Pins the Vulkan [0,1] depth convention: a second, GL-style [-1,1]
    // projection once lived beside this one and the two are only visibly
    // different at the near plane.
    Mat4 p = Mat4::perspective(to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    Vec3 near = p.transform_point({0, 0, -0.1f});
    Vec3 far = p.transform_point({0, 0, -100.0f});
    NF_CHECK_NEAR(near.z, 0.0f, 1e-4f);
    NF_CHECK_NEAR(far.z, 1.0f, 1e-4f);
}

NF_TEST(test_mat4_transpose_swaps_off_diagonal) {
    Mat4 r = Mat4::rotate_z(0.7f);
    Mat4 t = r.transposed();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            NF_CHECK_NEAR(t.m[row][col], r.m[col][row], 1e-6f);
        }
    }
    // Double transpose is identity — the property the CPU/GPU upload
    // contract rests on (row-major-flat of M == column-major-flat of M^T).
    Mat4 back = t.transposed();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            NF_CHECK_NEAR(back.m[row][col], r.m[row][col], 1e-6f);
        }
    }
}

NF_TEST(test_quat_identity) {
    Quat q;
    Mat4 m = q.to_matrix();
    Vec3 v(1, 2, 3);
    Vec3 r = m.transform_direction(v);
    NF_CHECK_NEAR(r.x, 1.0f, EPSILON);
    NF_CHECK_NEAR(r.y, 2.0f, EPSILON);
    NF_CHECK_NEAR(r.z, 3.0f, EPSILON);
}

NF_TEST(test_quat_rotation) {
    Quat q = Quat::from_axis_angle(Vec3::up, to_radians(90.0f));
    Mat4 m = q.to_matrix();
    Vec3 r = m.transform_direction(Vec3::forward);
    // 90° rotation around Y: forward → right
    NF_CHECK_NEAR(r.x, 1.0f, 0.001f);
    NF_CHECK_NEAR(r.y, 0.0f, 0.001f);
    NF_CHECK_NEAR(r.z, 0.0f, 0.001f);
}

// --- Quaternion helpers added for the physics phase ------------------------

NF_TEST(test_quat_rotate_matches_matrix) {
    // q.rotate(v) exists because it is the hot path in the solver; it must agree
    // with the matrix form exactly, or one of the two is wrong.
    const Quat q = Quat::from_axis_angle(Vec3(0.3f, 1.0f, -0.2f), to_radians(37.0f));
    const Mat4 m = q.to_matrix();
    const Vec3 probes[4] = {Vec3::right, Vec3::up, Vec3::forward, Vec3(1.5f, -2.5f, 0.75f)};
    for (const Vec3& v : probes) {
        const Vec3 via_quat = q.rotate(v);
        const Vec3 via_matrix = m.transform_direction(v);
        NF_CHECK(via_quat.nearly_equals(via_matrix, 1e-4f));
    }
}

NF_TEST(test_quat_conjugate_undoes_a_rotation) {
    const Quat q = Quat::from_axis_angle(Vec3(1.0f, 2.0f, 3.0f), to_radians(120.0f));
    const Vec3 v(0.4f, -1.2f, 3.3f);
    const Vec3 rotated = q.rotate(v);
    const Vec3 restored = q.conjugate().rotate(rotated);
    NF_CHECK(restored.nearly_equals(v, 1e-4f));
}

NF_TEST(test_quat_inverse_handles_a_non_unit_quaternion) {
    // Integration produces slightly non-unit quaternions between
    // normalisations, so inverse() must divide by the squared norm rather than
    // merely flipping the vector part.
    //
    // Checked algebraically (q * q⁻¹ == identity) and NOT by rotating a vector:
    // rotate() deliberately assumes a unit quaternion because it is the solver's
    // hot path and normalising there would cost it a sqrt. Feeding it a non-unit
    // input would be testing a precondition violation, not inverse().
    const Quat unit = Quat::from_axis_angle(Vec3::up, to_radians(45.0f));
    const Quat scaled = unit * 3.0f;
    const Quat product = scaled * scaled.inverse();
    NF_CHECK_NEAR(product.x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(product.y, 0.0f, 1e-5f);
    NF_CHECK_NEAR(product.z, 0.0f, 1e-5f);
    NF_CHECK_NEAR(product.w, 1.0f, 1e-5f);
}

NF_TEST(test_quat_from_matrix_round_trips) {
    const f32 angles[4] = {0.0f, 15.0f, 90.0f, 175.0f};
    for (f32 deg : angles) {
        const Quat q = Quat::from_axis_angle(Vec3(0.5f, 1.0f, 0.25f), to_radians(deg));
        const Quat back = Quat::from_matrix(q.to_matrix());
        const Vec3 probe(0.3f, -0.7f, 0.5f);
        NF_CHECK(q.rotate(probe).nearly_equals(back.rotate(probe), 1e-3f));
    }
}

NF_TEST(test_quat_from_matrix_exercises_every_branch) {
    // Exactly 180 degrees is the case a trace-only extraction gets wrong, and it
    // is where Shepperd's x/y/z branches are selected instead of the w branch.
    // A physics body that flipped over lands here.
    const Vec3 axes[3] = {Vec3::right, Vec3::up, Vec3::forward};
    for (const Vec3& axis : axes) {
        const Quat q = Quat::from_axis_angle(axis, PI);
        const Quat back = Quat::from_matrix(q.to_matrix());
        // q and -q describe the same rotation, so compare the effect.
        const Vec3 probe(0.3f, -0.7f, 0.5f);
        NF_CHECK(q.rotate(probe).nearly_equals(back.rotate(probe), 1e-4f));
    }
}

NF_TEST(test_quat_identity_is_a_no_op) {
    const Vec3 v(2.0f, -3.0f, 4.0f);
    NF_CHECK(Quat::identity().rotate(v).nearly_equals(v, 1e-6f));
    NF_CHECK_NEAR(Quat::identity().length_sq(), 1.0f, 1e-6f);
}

// --- Vec3 component-wise helpers -------------------------------------------

NF_TEST(test_vec3_component_wise_ops) {
    const Vec3 a(1.0f, -5.0f, 3.0f);
    const Vec3 b(-2.0f, 4.0f, 3.0f);

    NF_CHECK(a.min(b).nearly_equals(Vec3(-2.0f, -5.0f, 3.0f)));
    NF_CHECK(a.max(b).nearly_equals(Vec3(1.0f, 4.0f, 3.0f)));
    NF_CHECK(a.abs().nearly_equals(Vec3(1.0f, 5.0f, 3.0f)));
    NF_CHECK(a.scaled(b).nearly_equals(Vec3(-2.0f, -20.0f, 9.0f)));

    NF_CHECK_NEAR(a.max_component(), 3.0f, 1e-6f);
    NF_CHECK_NEAR(a.min_component(), -5.0f, 1e-6f);
    // max_abs_component is what a box-extent or SAT projection needs.
    NF_CHECK_NEAR(a.max_abs_component(), 5.0f, 1e-6f);
}

NF_TEST(test_vec3_nearly_equals_respects_the_tolerance) {
    const Vec3 a(1.0f, 2.0f, 3.0f);
    NF_CHECK(a.nearly_equals(Vec3(1.0f, 2.0f, 3.0f)));
    NF_CHECK(a.nearly_equals(Vec3(1.00005f, 2.0f, 3.0f), 1e-3f));
    NF_CHECK(!a.nearly_equals(Vec3(1.001f, 2.0f, 3.0f), 1e-5f));
    NF_CHECK(!a.nearly_equals(Vec3(1.0f, 2.0f, 3.5f)));
}
