#include <NF/Physics/WindField.hpp>
#include <NF/Test/TestFramework.hpp>
#include <cmath>

using namespace nf;
using namespace nf::physics;

NF_TEST(wind_zone_directional_uniform_velocity) {
    WindZone zone;
    zone.type = WindZoneType::Directional;
    zone.direction = Vec3{1, 0, 0};
    zone.strength = 10.0f;
    zone.radius = 0.0f; // unbounded

    const Vec3 w1 = zone.sample(Vec3{0, 0, 0}, 0.0f);
    const Vec3 w2 = zone.sample(Vec3{100, 50, -20}, 0.0f);

    NF_CHECK_NEAR(w1.x, 10.0f, 1e-4f);
    NF_CHECK_NEAR(w1.y, 0.0f, 1e-4f);
    NF_CHECK_NEAR(w2.x, 10.0f, 1e-4f);
}

NF_TEST(wind_zone_directional_bounded_radius_falloff) {
    WindZone zone;
    zone.type = WindZoneType::Directional;
    zone.position = Vec3{0, 0, 0};
    zone.direction = Vec3{0, 0, 1};
    zone.strength = 10.0f;
    zone.radius = 20.0f;
    zone.falloff_exponent = 1.0f;

    const Vec3 center = zone.sample(Vec3{0, 0, 0}, 0.0f);
    NF_CHECK_NEAR(center.z, 10.0f, 1e-4f);

    const Vec3 mid = zone.sample(Vec3{0, 0, 10.0f}, 0.0f);
    NF_CHECK_NEAR(mid.z, 5.0f, 1e-4f); // 1 - 10/20 = 0.5

    const Vec3 outside = zone.sample(Vec3{0, 0, 25.0f}, 0.0f);
    NF_CHECK_NEAR(outside.z, 0.0f, 1e-4f);
}

NF_TEST(wind_zone_spherical_blast_radial_direction) {
    WindZone zone;
    zone.type = WindZoneType::Spherical;
    zone.position = Vec3{0, 0, 0};
    zone.strength = 20.0f;
    zone.radius = 10.0f;

    const Vec3 east = zone.sample(Vec3{5, 0, 0}, 0.0f);
    NF_CHECK(east.x > 0.0f);
    NF_CHECK_NEAR(east.y, 0.0f, 1e-4f);

    const Vec3 north = zone.sample(Vec3{0, 0, 5}, 0.0f);
    NF_CHECK(north.z > 0.0f);
    NF_CHECK_NEAR(north.x, 0.0f, 1e-4f);
}

NF_TEST(wind_zone_spherical_distance_falloff) {
    WindZone zone;
    zone.type = WindZoneType::Spherical;
    zone.position = Vec3{0, 0, 0};
    zone.strength = 10.0f;
    zone.radius = 10.0f;
    zone.falloff_exponent = 2.0f; // Quadratic falloff

    const Vec3 near_pt = zone.sample(Vec3{2, 0, 0}, 0.0f);
    const Vec3 mid_pt = zone.sample(Vec3{5, 0, 0}, 0.0f);

    // At dist=2: atten = (1 - 0.2)^2 = 0.64 -> 6.4 m/s
    NF_CHECK_NEAR(near_pt.length(), 6.4f, 0.01f);
    // At dist=5: atten = (1 - 0.5)^2 = 0.25 -> 2.5 m/s
    NF_CHECK_NEAR(mid_pt.length(), 2.5f, 0.01f);
}

NF_TEST(wind_zone_vortex_tangential_rotation) {
    WindZone zone;
    zone.type = WindZoneType::Vortex;
    zone.position = Vec3{0, 0, 0};
    zone.direction = Vec3{0, 1, 0}; // Y-axis vortex
    zone.strength = 15.0f;
    zone.radius = 10.0f;

    // Point on positive X axis (5, 0, 0)
    // Tangent = Axis (0,1,0) x Radial (1,0,0) = (0, 0, -1)
    const Vec3 v = zone.sample(Vec3{5, 0, 0}, 0.0f);
    NF_CHECK(v.z < 0.0f);
    NF_CHECK_NEAR(v.x, 0.0f, 1e-4f);
    // Should carry an upward component
    NF_CHECK(v.y > 0.0f);
}

NF_TEST(wind_zone_periodic_gust_pulse) {
    WindZone zone;
    zone.type = WindZoneType::Directional;
    zone.direction = Vec3{1, 0, 0};
    zone.strength = 10.0f;
    zone.pulse_frequency = 1.0f; // 1 Hz (T = 1.0s)
    zone.pulse_magnitude = 0.5f; // Oscillates between 5.0 and 15.0 m/s

    const Vec3 t0 = zone.sample(Vec3{0, 0, 0}, 0.0f);   // sin(0) = 0 -> 10.0
    const Vec3 t_quarter = zone.sample(Vec3{0, 0, 0}, 0.25f); // sin(pi/2) = 1 -> 15.0
    const Vec3 t_three_q = zone.sample(Vec3{0, 0, 0}, 0.75f); // sin(3pi/2) = -1 -> 5.0

    NF_CHECK_NEAR(t0.x, 10.0f, 0.01f);
    NF_CHECK_NEAR(t_quarter.x, 15.0f, 0.01f);
    NF_CHECK_NEAR(t_three_q.x, 5.0f, 0.01f);
}

NF_TEST(wind_field_manager_global_wind_baseline) {
    WindFieldManager mgr;
    mgr.set_turbulence(0.0f, 1.0f); // Zero turbulence for exact baseline test
    mgr.set_global_wind(Vec3{0, 0, 1}, 8.0f);

    const Vec3 w = mgr.sample_wind(Vec3{10, 20, 30}, 0.0f);
    NF_CHECK_NEAR(w.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(w.y, 0.0f, 1e-4f);
    NF_CHECK_NEAR(w.z, 8.0f, 1e-4f);
    NF_CHECK_NEAR(mgr.sample_speed(Vec3{10, 20, 30}, 0.0f), 8.0f, 1e-4f);
}

NF_TEST(wind_field_manager_spatial_turbulence) {
    WindFieldManager mgr;
    mgr.set_global_wind(Vec3{1, 0, 0}, 0.0f); // No directional wind
    mgr.set_turbulence(2.0f, 0.5f);

    const Vec3 p1 = mgr.sample_wind(Vec3{0, 0, 0}, 0.0f);
    const Vec3 p2 = mgr.sample_wind(Vec3{50, 10, -30}, 0.0f);

    // Turbulence produces non-zero varying air vectors across space
    NF_CHECK(p1.length() > 0.1f);
    NF_CHECK(p2.length() > 0.1f);
    NF_CHECK(!p1.nearly_equals(p2, 0.1f));
}

NF_TEST(wind_field_manager_multiple_zones_superposition) {
    WindFieldManager mgr;
    mgr.set_global_wind(Vec3{1, 0, 0}, 2.0f);
    mgr.set_turbulence(0.0f, 1.0f);

    WindZone z1;
    z1.type = WindZoneType::Directional;
    z1.direction = Vec3{1, 0, 0};
    z1.strength = 3.0f;
    mgr.add_zone(z1);

    WindZone z2;
    z2.type = WindZoneType::Directional;
    z2.direction = Vec3{0, 1, 0};
    z2.strength = 4.0f;
    mgr.add_zone(z2);

    // Total = (2+3, 4, 0) = (5, 4, 0) -> speed = sqrt(25 + 16) = sqrt(41) ~ 6.403
    const Vec3 w = mgr.sample_wind(Vec3{0, 0, 0}, 0.0f);
    NF_CHECK_NEAR(w.x, 5.0f, 1e-4f);
    NF_CHECK_NEAR(w.y, 4.0f, 1e-4f);
    NF_CHECK_NEAR(mgr.sample_speed(Vec3{0, 0, 0}, 0.0f), std::sqrt(41.0f), 0.01f);
}

NF_TEST(wind_field_manager_clear_and_zone_removal) {
    WindFieldManager mgr;
    mgr.set_turbulence(0.0f, 1.0f);
    mgr.set_global_wind(Vec3{1, 0, 0}, 1.0f);

    WindZone z;
    z.strength = 5.0f;
    const size_t idx = mgr.add_zone(z);
    NF_CHECK_EQ(mgr.zone_count(), 1u);

    mgr.remove_zone(idx);
    NF_CHECK_EQ(mgr.zone_count(), 0u);

    mgr.add_zone(z);
    mgr.add_zone(z);
    NF_CHECK_EQ(mgr.zone_count(), 2u);
    mgr.clear();
    NF_CHECK_EQ(mgr.zone_count(), 0u);
}
