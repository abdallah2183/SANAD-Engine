// Tests/AITests/test_flow_field.cpp — Flow Field navigation grid tests (Phase 121)

#include <NF/Test/TestFramework.hpp>
#include <NF/AI/FlowField.hpp>

#include <cmath>

using namespace nf;
using namespace nf::ai;

NF_TEST(flow_field_grid_init) {
    FlowFieldGrid grid(16, 16, 1.0f);
    NF_CHECK_EQ(grid.width(), 16u);
    NF_CHECK_EQ(grid.height(), 16u);
    NF_CHECK_NEAR(grid.cell_size(), 1.0f, 1e-4f);
    NF_CHECK_EQ(grid.get_cost(0, 0), 1u);
    NF_CHECK_EQ(grid.get_integration(0, 0), kIntegrationMax);
    Vec2 v = grid.get_vector(0, 0);
    NF_CHECK_NEAR(v.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(v.y, 0.0f, 1e-4f);
}

NF_TEST(flow_field_single_target_destination) {
    FlowFieldGrid grid(10, 10, 1.0f);
    grid.compute_field(5, 5);

    // Target cell itself has 0 integration cost and 0 velocity vector
    NF_CHECK_EQ(grid.get_integration(5, 5), 0u);
    Vec2 v = grid.get_vector(5, 5);
    NF_CHECK_NEAR(v.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(v.y, 0.0f, 1e-4f);
}

NF_TEST(flow_field_direct_cardinal_flow) {
    FlowFieldGrid grid(10, 10, 1.0f);
    grid.compute_field(5, 5);

    // Cell (7, 5) is 2 units to the right; flow must point West (-1, 0)
    Vec2 v_east = grid.get_vector(7, 5);
    NF_CHECK_NEAR(v_east.x, -1.0f, 1e-3f);
    NF_CHECK_NEAR(v_east.y, 0.0f, 1e-3f);

    // Cell (5, 2) is 3 units above; flow must point South (0, 1)
    Vec2 v_north = grid.get_vector(5, 2);
    NF_CHECK_NEAR(v_north.x, 0.0f, 1e-3f);
    NF_CHECK_NEAR(v_north.y, 1.0f, 1e-3f);
}

NF_TEST(flow_field_direct_diagonal_flow) {
    FlowFieldGrid grid(10, 10, 1.0f);
    grid.compute_field(5, 5);

    // Cell (7, 7) is southeast; flow must point northwest (-0.7071, -0.7071)
    Vec2 v = grid.get_vector(7, 7);
    NF_CHECK(v.x < -0.5f);
    NF_CHECK(v.y < -0.5f);
}

NF_TEST(flow_field_obstacle_avoidance) {
    FlowFieldGrid grid(10, 10, 1.0f);
    // Wall between x=5 and x=8 at y=4
    for (u32 x = 5; x <= 8; ++x) {
        grid.set_blocked(x, 4, true);
    }

    grid.compute_field(6, 6);

    // Cell (6, 3) is behind the wall; it must route around the wall, not through it
    Vec2 v = grid.get_vector(6, 3);
    NF_CHECK(v.x != 0.0f || v.y != 0.0f);
    // Impassable wall cell itself has no flow vector
    Vec2 v_wall = grid.get_vector(6, 4);
    NF_CHECK_NEAR(v_wall.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(v_wall.y, 0.0f, 1e-4f);
}

NF_TEST(flow_field_variable_terrain_cost) {
    FlowFieldGrid grid(10, 5, 1.0f);
    // Straight path through (1..8, 2) has high mud cost = 20
    for (u32 x = 1; x <= 8; ++x) {
        grid.set_cost(x, 2, 20);
    }

    grid.compute_field(9, 2);

    // Adjacent cells in clear row (y=1) should have lower integration cost than mud cells
    u16 clear_cost = grid.get_integration(5, 1);
    u16 mud_cost   = grid.get_integration(5, 2);
    NF_CHECK(clear_cost < mud_cost);
}

NF_TEST(flow_field_multi_goal_convergence) {
    FlowFieldGrid grid(20, 20, 1.0f);

    std::vector<FlowCellCoord> goals;
    goals.push_back(FlowCellCoord{2, 2});
    goals.push_back(FlowCellCoord{18, 18});
    grid.compute_field_multi(goals);

    // Near goal 1
    Vec2 v1 = grid.get_vector(4, 2);
    NF_CHECK(v1.x < 0.0f); // Points towards (2, 2)

    // Near goal 2
    Vec2 v2 = grid.get_vector(16, 18);
    NF_CHECK(v2.x > 0.0f); // Points towards (18, 18)
}

NF_TEST(flow_field_unreachable_cells) {
    FlowFieldGrid grid(8, 8, 1.0f);

    // Enclose corner (0, 0) completely with walls
    grid.set_blocked(0, 1, true);
    grid.set_blocked(1, 1, true);
    grid.set_blocked(1, 0, true);

    grid.compute_field(7, 7);

    // Isolated cell (0, 0) cannot reach goal
    NF_CHECK_EQ(grid.get_integration(0, 0), kIntegrationMax);
    Vec2 v = grid.get_vector(0, 0);
    NF_CHECK_NEAR(v.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(v.y, 0.0f, 1e-4f);
}

NF_TEST(flow_field_world_transforms) {
    FlowFieldGrid grid(10, 10, 2.0f, Vec2{10.0f, 20.0f});

    i32 gx = 0, gy = 0;
    bool ok = grid.world_to_grid(Vec2{11.0f, 21.0f}, gx, gy);
    NF_CHECK(ok);
    NF_CHECK_EQ(gx, 0);
    NF_CHECK_EQ(gy, 0);

    Vec2 world_center = grid.grid_to_world(1, 1);
    // Origin 10 + 1.5 * 2 = 13, 20 + 1.5 * 2 = 23
    NF_CHECK_NEAR(world_center.x, 13.0f, 1e-4f);
    NF_CHECK_NEAR(world_center.y, 23.0f, 1e-4f);
}

NF_TEST(flow_field_world_sampling_bilinear) {
    FlowFieldGrid grid(10, 10, 1.0f, Vec2{0.0f, 0.0f});
    grid.compute_field(5, 5);

    // Continuous query at world position
    Vec2 steer = grid.sample_flow_world(Vec2{7.2f, 5.0f});
    NF_CHECK(steer.x < -0.8f);
    NF_CHECK_NEAR(steer.length(), 1.0f, 0.05f);
}
