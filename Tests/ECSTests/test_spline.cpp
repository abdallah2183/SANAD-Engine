#include <NF/Test/TestFramework.hpp>
#include <NF/Scene/Spline.hpp>
#include <NF/ECS/ECS.hpp>
#include <cmath>

using namespace nf;
using namespace nf::scene;
using namespace nf::ecs;

NF_TEST(spline_linear_straight_path) {
    Spline3D spline;
    spline.add_point(Vec3(0.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(10.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(20.0f, 0.0f, 0.0f), SplinePointType::Linear);

    NF_CHECK_EQ(spline.point_count(), 3u);
    NF_CHECK_EQ(spline.segment_count(), 2u);

    spline.rebuild_arc_length_table(16);
    NF_CHECK(std::fabs(spline.total_length() - 20.0f) < 0.05f);

    // Halfway sample at distance 10
    auto sample = spline.evaluate_at_distance(10.0f);
    NF_CHECK(std::fabs(sample.position.x - 10.0f) < 0.1f);
    NF_CHECK(std::fabs(sample.position.y) < 0.01f);
    NF_CHECK(std::fabs(sample.position.z) < 0.01f);

    // Tangent must point along +X
    NF_CHECK(sample.tangent.nearly_equals(Vec3(1.0f, 0.0f, 0.0f), 0.01f));
    // Curvature on a straight line is zero
    NF_CHECK(sample.curvature < 0.001f);
}

NF_TEST(spline_catmull_rom_smooth_curve_and_tangents) {
    Spline3D spline;
    spline.add_point(Vec3(0.0f, 0.0f, 0.0f), SplinePointType::CurveAuto);
    spline.add_point(Vec3(10.0f, 10.0f, 0.0f), SplinePointType::CurveAuto);
    spline.add_point(Vec3(20.0f, -10.0f, 0.0f), SplinePointType::CurveAuto);
    spline.add_point(Vec3(30.0f, 0.0f, 0.0f), SplinePointType::CurveAuto);

    spline.rebuild_arc_length_table(32);
    NF_CHECK(spline.total_length() > 30.0f);

    auto start_sample = spline.evaluate(0.0f);
    NF_CHECK(start_sample.position.nearly_equals(Vec3(0.0f, 0.0f, 0.0f), 0.01f));

    auto end_sample = spline.evaluate(1.0f);
    NF_CHECK(end_sample.position.nearly_equals(Vec3(30.0f, 0.0f, 0.0f), 0.01f));

    // Mid curve sample should have non-zero curvature
    auto mid_sample = spline.evaluate(0.5f);
    NF_CHECK(std::isfinite(mid_sample.position.x));
    NF_CHECK(std::isfinite(mid_sample.position.y));
    NF_CHECK(std::isfinite(mid_sample.curvature));
    NF_CHECK(mid_sample.tangent.length() > 0.99f);
}

NF_TEST(spline_arc_length_constant_speed_reparameterization) {
    // Non-uniform spacing: 0 -> 2 (dist 2), 2 -> 22 (dist 20)
    Spline3D spline;
    spline.add_point(Vec3(0.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(2.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(22.0f, 0.0f, 0.0f), SplinePointType::Linear);

    spline.rebuild_arc_length_table(16);
    NF_CHECK(std::fabs(spline.total_length() - 22.0f) < 0.05f);

    // Exact halfway distance is 11.0f.
    // In parametric t, t=0.5 would be at point 1 (x=2.0f), which is NOT halfway in space!
    // But evaluate_at_distance(11.0f) MUST be at x=11.0f!
    auto dist_sample = spline.evaluate_at_distance(11.0f);
    NF_CHECK(std::fabs(dist_sample.position.x - 11.0f) < 0.1f);

    // Verify round-trip distance <-> time
    f32 test_dist = 7.5f;
    f32 t = spline.distance_to_time(test_dist);
    f32 rt_dist = spline.time_to_distance(t);
    NF_CHECK(std::fabs(test_dist - rt_dist) < 0.05f);
}

NF_TEST(spline_closed_loop_track) {
    Spline3D spline;
    spline.add_point(Vec3(0.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(10.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(10.0f, 10.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(0.0f, 10.0f, 0.0f), SplinePointType::Linear);
    spline.set_closed(true);

    NF_CHECK(spline.is_closed());
    NF_CHECK_EQ(spline.segment_count(), 4u);

    spline.rebuild_arc_length_table(16);
    // Perimeter = 10 + 10 + 10 + 10 = 40
    NF_CHECK(std::fabs(spline.total_length() - 40.0f) < 0.1f);

    auto start_sample = spline.evaluate(0.0f);
    auto end_sample = spline.evaluate(1.0f);
    NF_CHECK(start_sample.position.nearly_equals(end_sample.position, 0.01f));
}

NF_TEST(spline_follower_modes_and_banking) {
    Spline3D spline;
    spline.add_point(Vec3(0.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(50.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.rebuild_arc_length_table(16);

    SplineFollower follower(&spline);
    follower.set_speed(10.0f);
    follower.set_mode(SplineFollowMode::Once);

    // 2 seconds @ 10 m/s = 20m
    follower.update(2.0f);
    NF_CHECK(std::fabs(follower.current_distance() - 20.0f) < 0.1f);
    NF_CHECK(!follower.is_finished());

    // 4 more seconds (total 60m > 50m) -> finishes and clamps to 50m
    follower.update(4.0f);
    NF_CHECK(follower.is_finished());
    NF_CHECK(std::fabs(follower.current_distance() - 50.0f) < 0.01f);

    // Test PingPong mode
    follower.reset();
    follower.set_mode(SplineFollowMode::PingPong);
    // 6 seconds @ 10 m/s = 60m -> hits 50m, reverses 10m -> distance 40m
    follower.update(6.0f);
    NF_CHECK(follower.is_reverse());
    NF_CHECK(std::fabs(follower.current_distance() - 40.0f) < 0.1f);
}

NF_TEST(spline_find_nearest_point_projection) {
    Spline3D spline;
    spline.add_point(Vec3(0.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.add_point(Vec3(100.0f, 0.0f, 0.0f), SplinePointType::Linear);
    spline.rebuild_arc_length_table(16);

    // Query point at (42.0, 15.0, 8.0)
    f32 nearest_dist = 0.0f;
    auto nearest = spline.find_nearest_point(Vec3(42.0f, 15.0f, 8.0f), nearest_dist);

    NF_CHECK(std::fabs(nearest.position.x - 42.0f) < 0.2f);
    NF_CHECK(std::fabs(nearest.position.y) < 0.01f);
    NF_CHECK(std::fabs(nearest.position.z) < 0.01f);
    NF_CHECK(std::fabs(nearest_dist - 42.0f) < 0.2f);
}

NF_TEST(spline_ecs_component_integration) {
    World world;
    Entity ent = world.create_entity();

    SplineComponent comp;
    comp.spline.add_point(Vec3(0.0f, 0.0f, 0.0f));
    comp.spline.add_point(Vec3(0.0f, 10.0f, 0.0f));
    comp.spline.rebuild_arc_length_table(16);

    world.add<SplineComponent>(ent, std::move(comp));
    NF_CHECK(world.has<SplineComponent>(ent));

    const auto* comp_ptr = world.get<SplineComponent>(ent);
    NF_CHECK(comp_ptr != nullptr);
    NF_CHECK_EQ(comp_ptr->spline.point_count(), 2u);
    NF_CHECK(std::fabs(comp_ptr->spline.total_length() - 10.0f) < 0.1f);
}
