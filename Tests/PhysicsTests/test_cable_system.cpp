// Tests/PhysicsTests/test_cable_system.cpp — Dynamic Cable & Rope Physics tests (Phase 118)

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/CableSystem.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

NF_TEST(cable_initial_state) {
    CableConfig config;
    config.particle_count = 10;
    config.total_length = 9.0f;

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 5.0f, 0.0f}, Vec3{9.0f, 5.0f, 0.0f});

    NF_CHECK_EQ(cable.particle_count(), 10u);
    NF_CHECK(!cable.is_broken());

    const auto positions = cable.get_positions();
    NF_CHECK_EQ(positions.size(), 10u);
    NF_CHECK_NEAR(positions.front().x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(positions.front().y, 5.0f, 1e-4f);
    NF_CHECK_NEAR(positions.back().x, 9.0f, 1e-4f);
    NF_CHECK_NEAR(positions.back().y, 5.0f, 1e-4f);
}

NF_TEST(cable_pinned_endpoints_immobile) {
    CableConfig config;
    config.particle_count = 8;
    config.gravity = Vec3{0.0f, -9.81f, 0.0f};

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 10.0f, 0.0f}, Vec3{7.0f, 10.0f, 0.0f});

    for (int i = 0; i < 30; ++i) {
        cable.step(0.016f);
    }

    const auto positions = cable.get_positions();
    // Endpoints must stay locked in place
    NF_CHECK_NEAR(positions.front().x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(positions.front().y, 10.0f, 1e-4f);
    NF_CHECK_NEAR(positions.back().x, 7.0f, 1e-4f);
    NF_CHECK_NEAR(positions.back().y, 10.0f, 1e-4f);
}

NF_TEST(cable_gravity_catenary_sag) {
    CableConfig config;
    config.particle_count = 9;
    config.total_length = 10.0f;
    config.gravity = Vec3{0.0f, -9.81f, 0.0f};

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 10.0f, 0.0f}, Vec3{8.0f, 10.0f, 0.0f});

    for (int i = 0; i < 60; ++i) {
        cable.step(0.016f);
    }

    const auto positions = cable.get_positions();
    usize mid_idx = positions.size() / 2;
    // Midpoint should sag downwards
    NF_CHECK(positions[mid_idx].y < 9.5f);
}

NF_TEST(cable_length_conservation) {
    CableConfig config;
    config.particle_count = 6;
    config.total_length = 5.0f;
    config.stiffness = 0.98f;
    config.solver_iterations = 12;

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 5.0f, 0.0f}, Vec3{4.0f, 5.0f, 0.0f});

    for (int i = 0; i < 40; ++i) {
        cable.step(0.016f);
    }

    const auto positions = cable.get_positions();
    f32 measured_len = 0.0f;
    for (usize i = 0; i + 1 < positions.size(); ++i) {
        measured_len += (positions[i + 1] - positions[i]).length();
    }

    // Measured length should be close to rest length (within 5% compliance)
    NF_CHECK_NEAR(measured_len, 5.0f, 0.25f);
}

NF_TEST(cable_tension_computation) {
    CableConfig config;
    config.particle_count = 8;
    config.total_length = 8.0f;

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 5.0f, 0.0f}, Vec3{7.0f, 5.0f, 0.0f});

    for (int i = 0; i < 30; ++i) {
        cable.step(0.016f);
    }

    NF_CHECK(cable.start_tension() > 0.0f);
    NF_CHECK(cable.end_tension() > 0.0f);
    NF_CHECK(cable.max_current_tension() > 0.0f);
}

NF_TEST(cable_move_pinned_anchor) {
    CableConfig config;
    config.particle_count = 6;

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 5.0f, 0.0f}, Vec3{5.0f, 5.0f, 0.0f});

    cable.move_pinned_particle(0, Vec3{-2.0f, 7.0f, 0.0f});
    cable.step(0.016f);

    const auto positions = cable.get_positions();
    NF_CHECK_NEAR(positions.front().x, -2.0f, 1e-4f);
    NF_CHECK_NEAR(positions.front().y, 7.0f, 1e-4f);
}

NF_TEST(cable_unpin_anchor) {
    CableConfig config;
    config.particle_count = 6;
    config.gravity = Vec3{0.0f, -9.81f, 0.0f};

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 10.0f, 0.0f}, Vec3{5.0f, 10.0f, 0.0f});

    // Unpin start anchor
    cable.unpin_particle(0);

    for (int i = 0; i < 40; ++i) {
        cable.step(0.016f);
    }

    const auto positions = cable.get_positions();
    // Particle 0 should have fallen significantly
    NF_CHECK(positions.front().y < 9.0f);
    // End anchor was kept pinned
    NF_CHECK_NEAR(positions.back().y, 10.0f, 1e-4f);
}

NF_TEST(cable_ground_plane_collision) {
    CableConfig config;
    config.particle_count = 8;
    config.total_length = 10.0f;
    config.gravity = Vec3{0.0f, -9.81f, 0.0f};

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 2.0f, 0.0f}, Vec3{6.0f, 2.0f, 0.0f});
    cable.set_ground_plane(true, 1.0f); // ground floor at y = 1.0

    for (int i = 0; i < 60; ++i) {
        cable.step(0.016f);
    }

    const auto positions = cable.get_positions();
    for (const auto& p : positions) {
        NF_CHECK(p.y >= 0.999f); // Never dips below ground
    }
}

NF_TEST(cable_sphere_obstacle_deflection) {
    CableConfig config;
    config.particle_count = 12;
    config.total_length = 8.0f;
    config.gravity = Vec3{0.0f, -9.81f, 0.0f};

    CableSimulation cable(config);
    cable.initialize_endpoints(Vec3{0.0f, 5.0f, 0.0f}, Vec3{6.0f, 5.0f, 0.0f});

    // Sphere placed right under cable midpoint
    cable.add_sphere_obstacle(Vec3{3.0f, 4.0f, 0.0f}, 1.0f);

    for (int i = 0; i < 60; ++i) {
        cable.step(0.016f);
    }

    const auto positions = cable.get_positions();
    const Vec3 center{3.0f, 4.0f, 0.0f};
    for (const auto& p : positions) {
        f32 dist = (p - center).length();
        NF_CHECK(dist >= 0.99f); // Deflected outside sphere radius
    }
}

NF_TEST(cable_tension_snapping_break) {
    CableConfig config;
    config.particle_count = 4;
    config.total_length = 3.0f;
    config.max_tension = 100.0f; // Low breaking limit
    config.breakable = true;
    config.solver_iterations = 6;

    CableSimulation cable(config);
    // Stretch cable endpoints beyond rest length (distance 6m > rest length 3m)
    cable.initialize_endpoints(Vec3{0.0f, 0.0f, 0.0f}, Vec3{6.0f, 0.0f, 0.0f});

    cable.step(0.016f);

    NF_CHECK(cable.is_broken());
}
