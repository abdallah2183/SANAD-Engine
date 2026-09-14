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
