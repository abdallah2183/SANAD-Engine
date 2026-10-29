#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Geometry.hpp>

using namespace nf;

NF_TEST(geometry_plane_distance_and_normalization) {
    Plane p(Vec3(0.0f, 2.0f, 0.0f), -10.0f);
    p.normalize();
    NF_CHECK_NEAR(p.normal.y, 1.0f, 1e-5f);
    NF_CHECK_NEAR(p.distance, -5.0f, 1e-5f);

    // Point at y = 8 should be distance +3 from plane at y = 5
    Vec3 pt(0.0f, 8.0f, 0.0f);
    NF_CHECK_NEAR(p.signed_distance(pt), 3.0f, 1e-5f);

    // Point at y = 2 should be distance -3 from plane at y = 5
    Vec3 pt2(0.0f, 2.0f, 0.0f);
    NF_CHECK_NEAR(p.signed_distance(pt2), -3.0f, 1e-5f);
}

NF_TEST(geometry_aabb_expansion_and_contains) {
    AABB box;
    NF_CHECK(!box.is_valid());

    box.expand(Vec3(-2.0f, -1.0f, -3.0f));
    box.expand(Vec3(4.0f, 5.0f, 3.0f));
    NF_CHECK(box.is_valid());

    NF_CHECK_NEAR(box.center().x, 1.0f, 1e-5f);
    NF_CHECK_NEAR(box.center().y, 2.0f, 1e-5f);
    NF_CHECK_NEAR(box.center().z, 0.0f, 1e-5f);

    NF_CHECK_NEAR(box.size().x, 6.0f, 1e-5f);
    NF_CHECK_NEAR(box.size().y, 6.0f, 1e-5f);
    NF_CHECK_NEAR(box.size().z, 6.0f, 1e-5f);

    NF_CHECK(box.contains(Vec3(1.0f, 2.0f, 0.0f)));
    NF_CHECK(!box.contains(Vec3(10.0f, 2.0f, 0.0f)));

    AABB inner(Vec3(0.0f, 0.0f, 0.0f), Vec3(2.0f, 2.0f, 2.0f));
    NF_CHECK(box.contains(inner));

    AABB overlapping(Vec3(3.0f, 3.0f, 2.0f), Vec3(8.0f, 8.0f, 8.0f));
    NF_CHECK(box.intersects(overlapping));
    NF_CHECK(!box.contains(overlapping));
}

NF_TEST(geometry_aabb_transform) {
    AABB unit_box(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f));
    // Translate by (10, 0, 0) and scale by 2
    Mat4 m = Mat4::scale(Vec3(2.0f, 2.0f, 2.0f)) * Mat4::translate(Vec3(10.0f, 0.0f, 0.0f));
    AABB world_box = unit_box.transformed(m);

    NF_CHECK_NEAR(world_box.min.x, 8.0f, 1e-5f);
    NF_CHECK_NEAR(world_box.max.x, 12.0f, 1e-5f);
    NF_CHECK_NEAR(world_box.min.y, -2.0f, 1e-5f);
    NF_CHECK_NEAR(world_box.max.y, 2.0f, 1e-5f);
}

NF_TEST(geometry_sphere_aabb_intersection) {
    BoundingSphere sphere(Vec3(0.0f, 0.0f, 0.0f), 2.0f);
    AABB box_touching(Vec3(1.5f, 0.0f, 0.0f), Vec3(3.0f, 1.0f, 1.0f));
    NF_CHECK(sphere.intersects(box_touching));

    AABB box_outside(Vec3(5.0f, 5.0f, 5.0f), Vec3(6.0f, 6.0f, 6.0f));
    NF_CHECK(!sphere.intersects(box_outside));
}

NF_TEST(geometry_ray_intersections) {
    Ray ray(Vec3(0.0f, 0.0f, -10.0f), Vec3(0.0f, 0.0f, 1.0f));

    // Test Ray vs Sphere
    BoundingSphere sphere(Vec3(0.0f, 0.0f, 0.0f), 2.0f);
    f32 t_sphere = 0.0f;
    NF_CHECK(ray.intersects(sphere, t_sphere));
    NF_CHECK_NEAR(t_sphere, 8.0f, 1e-4f);

    // Test Ray vs AABB
    AABB box(Vec3(-1.0f, -1.0f, 2.0f), Vec3(1.0f, 1.0f, 4.0f));
    f32 t_box = 0.0f;
    NF_CHECK(ray.intersects(box, t_box));
    NF_CHECK_NEAR(t_box, 12.0f, 1e-4f);

    // Test Ray vs Triangle
    Vec3 v0(-2.0f, -2.0f, 5.0f);
    Vec3 v1(2.0f, -2.0f, 5.0f);
    Vec3 v2(0.0f, 2.0f, 5.0f);
    f32 t_tri = 0.0f;
    Vec2 bary;
    NF_CHECK(ray.intersects_triangle(v0, v1, v2, t_tri, bary));
    NF_CHECK_NEAR(t_tri, 15.0f, 1e-4f);
}

NF_TEST(geometry_frustum_culling) {
    // Standard perspective camera looking down -Z
    Mat4 proj = Mat4::perspective(to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    Mat4 view = Mat4::look_at(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    Mat4 vp = view * proj;

    Frustum frustum = Frustum::from_view_projection(vp);

    // Center point in view
    NF_CHECK(frustum.contains_point(Vec3(0.0f, 0.0f, 0.0f)));

    // Point far behind camera
    NF_CHECK(!frustum.contains_point(Vec3(0.0f, 0.0f, 20.0f)));

    // AABB in center of view
    AABB box_inside(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f));
    NF_CHECK(frustum.intersects_aabb(box_inside));

    // AABB far to the right outside frustum
    AABB box_outside(Vec3(50.0f, 0.0f, 0.0f), Vec3(52.0f, 2.0f, 2.0f));
    NF_CHECK(!frustum.intersects_aabb(box_outside));
}
