#include <NF/Test/TestFramework.hpp>
#include <NF/Water/RiverFlow.hpp>

#include <cmath>

using namespace nf;
using namespace nf::water;

NF_TEST(river_corridor_add_waypoints_and_length) {
    RiverCorridor river("Amazon");
    NF_CHECK_EQ(river.name(), "Amazon");
    NF_CHECK_EQ(river.waypoint_count(), 0ULL);
    NF_CHECK_NEAR(river.total_length(), 0.0f, 1e-4f);

    RiverWaypoint wp1{Vec3{0.0f, 10.0f, 0.0f}, 12.0f, 3.0f, 2.5f};
    RiverWaypoint wp2{Vec3{0.0f, 8.0f, 100.0f}, 14.0f, 3.5f, 2.8f};
    RiverWaypoint wp3{Vec3{50.0f, 5.0f, 200.0f}, 16.0f, 4.0f, 3.0f};

    river.add_waypoint(wp1);
    river.add_waypoint(wp2);
    river.add_waypoint(wp3);

    NF_CHECK_EQ(river.waypoint_count(), 3ULL);
    NF_CHECK(river.total_length() > 200.0f);
}

NF_TEST(river_sample_outside_channel) {
    RiverCorridor river("Nile");
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 0.0f}, 10.0f, 2.0f, 2.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 100.0f}, 10.0f, 2.0f, 2.0f});

    // Sample 50m to the right (+X=50), far beyond half-width (5m)
    auto s = river.sample(Vec3{50.0f, 0.0f, 50.0f});
    NF_CHECK(!s.in_river);
}

NF_TEST(river_sample_centerline) {
    RiverCorridor river("Rhine");
    // Straight river along +Z from Z=0 to Z=100
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 10.0f, 0.0f}, 20.0f, 4.0f, 3.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 10.0f, 100.0f}, 20.0f, 4.0f, 3.0f});

    // Sample right in the center at Z=50
    auto s = river.sample(Vec3{0.0f, 10.0f, 50.0f});
    NF_CHECK(s.in_river);
    NF_CHECK_NEAR(s.normalized_cross, 0.0f, 1e-3f);
    NF_CHECK_NEAR(s.water_surface_y, 10.0f, 1e-3f);
    NF_CHECK_NEAR(s.depth, 4.0f, 1e-3f);
    NF_CHECK_NEAR(s.riverbed_y, 6.0f, 1e-3f);

    // Full current speed along +Z
    NF_CHECK_NEAR(s.flow_velocity.z, 3.0f, 1e-3f);
    NF_CHECK_NEAR(s.flow_velocity.x, 0.0f, 1e-3f);
}

NF_TEST(river_sample_left_and_right_banks) {
    RiverCorridor river("Danube");
    // River along +Z with width = 20m (half-width = 10m)
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 0.0f}, 20.0f, 4.0f, 2.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 100.0f}, 20.0f, 4.0f, 2.0f});

    // Point near right bank (+X = +9.5m)
    auto s_right = river.sample(Vec3{9.5f, 0.0f, 50.0f});
    NF_CHECK(s_right.in_river);
    NF_CHECK(s_right.normalized_cross > 0.9f);
    NF_CHECK(s_right.normalized_cross <= 1.0f);
    // Depth tapers to near zero at the edge
    NF_CHECK(s_right.depth < 1.0f);

    // Point near left bank (-X = -9.5m)
    auto s_left = river.sample(Vec3{-9.5f, 0.0f, 50.0f});
    NF_CHECK(s_left.in_river);
    NF_CHECK(s_left.normalized_cross < -0.9f);
    NF_CHECK(s_left.normalized_cross >= -1.0f);
}

NF_TEST(river_sample_bank_foam) {
    RiverCorridor river("Congo");
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 0.0f}, 20.0f, 2.0f, 2.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 100.0f}, 20.0f, 2.0f, 2.0f});

    // Center has zero foam
    auto s_center = river.sample(Vec3{0.0f, 0.0f, 50.0f});
    NF_CHECK_NEAR(s_center.bank_foam_factor, 0.0f, 1e-3f);

    // Near bank (X = 9m -> cross = 0.9) has significant foam
    auto s_bank = river.sample(Vec3{9.0f, 0.0f, 50.0f});
    NF_CHECK(s_bank.bank_foam_factor > 0.5f);
}

NF_TEST(river_sample_parabolic_flow_speed) {
    RiverCorridor river("Thames");
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 0.0f}, 20.0f, 3.0f, 4.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 100.0f}, 20.0f, 3.0f, 4.0f});

    auto s_center = river.sample(Vec3{0.0f, 0.0f, 50.0f});
    auto s_edge = river.sample(Vec3{9.0f, 0.0f, 50.0f});

    // Center flows faster than near banks due to bank friction
    NF_CHECK(s_center.flow_velocity.length() > s_edge.flow_velocity.length());
}

NF_TEST(river_flow_system_multi_river) {
    RiverFlowSystem sys;

    RiverCorridor r1("MainStream");
    r1.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 0.0f}, 10.0f, 2.0f, 2.0f});
    r1.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 100.0f}, 10.0f, 2.0f, 2.0f});

    RiverCorridor r2("Tributary");
    r2.add_waypoint(RiverWaypoint{Vec3{100.0f, 0.0f, 0.0f}, 10.0f, 2.0f, 2.0f});
    r2.add_waypoint(RiverWaypoint{Vec3{100.0f, 0.0f, 100.0f}, 10.0f, 2.0f, 2.0f});

    sys.add_river(std::move(r1));
    sys.add_river(std::move(r2));
    NF_CHECK_EQ(sys.river_count(), 2ULL);

    // Sample near MainStream
    auto s1 = sys.sample(Vec3{0.0f, 0.0f, 20.0f});
    NF_CHECK(s1.in_river);

    // Sample near Tributary
    auto s2 = sys.sample(Vec3{100.0f, 0.0f, 20.0f});
    NF_CHECK(s2.in_river);

    // Sample between rivers
    auto s3 = sys.sample(Vec3{50.0f, 0.0f, 20.0f});
    NF_CHECK(!s3.in_river);
}

NF_TEST(river_drift_horizontal_acceleration) {
    RiverFlowSystem sys;
    RiverCorridor river("FastCreek");
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 0.0f}, 20.0f, 3.0f, 5.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 100.0f}, 20.0f, 3.0f, 5.0f});
    sys.add_river(std::move(river));

    // A boat floating at rest at (0, 0, 10)
    Vec3 pos{0.0f, 0.0f, 10.0f};
    Vec3 vel{0.0f, 0.0f, 0.0f};

    // Step physics with water drag
    for (int i = 0; i < 10; ++i) {
        sys.apply_drift(pos, vel, 20.0f, 5.0f, 0.1f);
    }

    // Velocity should have accelerated downstream (+Z) towards current speed (5 m/s)
    NF_CHECK(vel.z > 2.0f);
    NF_CHECK(pos.z > 10.5f);
}

NF_TEST(river_drift_buoyancy_equilibrium) {
    RiverFlowSystem sys;
    RiverCorridor river("LakeRiver");
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 5.0f, 0.0f}, 20.0f, 5.0f, 0.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 5.0f, 100.0f}, 20.0f, 5.0f, 0.0f});
    sys.add_river(std::move(river));

    // Object pushed 1 meter underwater at Y=4.0 (surface is at Y=5.0)
    Vec3 pos{0.0f, 4.0f, 50.0f};
    Vec3 vel{0.0f, 0.0f, 0.0f};

    // Advance drift physics
    for (int i = 0; i < 20; ++i) {
        sys.apply_drift(pos, vel, 30.0f, 8.0f, 0.05f);
    }

    // Buoyant force pushes the object upward towards surface level (Y ~ 5.0)
    NF_CHECK(pos.y > 4.5f);
}

NF_TEST(river_drift_outside_river_freefall) {
    RiverFlowSystem sys;
    RiverCorridor river("Canal");
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 0.0f}, 10.0f, 2.0f, 5.0f});
    river.add_waypoint(RiverWaypoint{Vec3{0.0f, 0.0f, 100.0f}, 10.0f, 2.0f, 5.0f});
    sys.add_river(std::move(river));

    // Object high on dry land (X = 50m)
    Vec3 pos{50.0f, 10.0f, 50.0f};
    Vec3 vel{0.0f, 0.0f, 0.0f};

    sys.apply_drift(pos, vel, 20.0f, 5.0f, 0.1f, 9.81f);

    // Free fall under gravity: vel.y < 0, pos.y < 10.0, no horizontal drift (vel.z == 0)
    NF_CHECK(vel.y < 0.0f);
    NF_CHECK(pos.y < 10.0f);
    NF_CHECK_NEAR(vel.z, 0.0f, 1e-4f);
}
