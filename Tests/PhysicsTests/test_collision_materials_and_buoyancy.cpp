#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/CollisionFilter.hpp>
#include <NF/Physics/PhysicsMaterial.hpp>
#include <NF/Physics/BuoyancySolver.hpp>
#include <cmath>

using namespace nf;
using namespace nf::physics;

NF_TEST(collision_matrix_layer_filtering) {
    CollisionLayerMatrix matrix;

    // By default, everything collides
    NF_CHECK(matrix.can_layers_collide(0, 1));
    NF_CHECK(matrix.can_layers_collide(0, 2));

    // Disable Player (0) colliding with PlayerBullet (2)
    matrix.set_layer_collision(0, 2, false);

    // Must be false in both directions (symmetric)
    NF_CHECK(!matrix.can_layers_collide(0, 2));
    NF_CHECK(!matrix.can_layers_collide(2, 0));

    // Enemy (1) still collides with PlayerBullet (2)
    NF_CHECK(matrix.can_layers_collide(1, 2));
    NF_CHECK(matrix.can_layers_collide(2, 1));

    // Re-enable
    matrix.set_layer_collision(0, 2, true);
    NF_CHECK(matrix.can_layers_collide(0, 2));
}

NF_TEST(collision_filter_group_id_override) {
    CollisionLayerMatrix matrix;
    matrix.set_layer_collision(0, 1, false); // Layers 0 and 1 disabled in matrix

    CollisionFilter f1 = CollisionFilter::all(0);
    CollisionFilter f2 = CollisionFilter::all(1);

    // Matrix alone prevents collision
    NF_CHECK(!matrix.should_collide(f1, f2));

    // Positive group: forces collision even when matrix says false
    f1.group_id = 42;
    f2.group_id = 42;
    NF_CHECK(matrix.should_collide(f1, f2));

    // Negative group: prevents collision even when matrix says true
    matrix.set_layer_collision(0, 1, true);
    f1.group_id = -7;
    f2.group_id = -7;
    NF_CHECK(!matrix.should_collide(f1, f2));

    // Different negative groups revert to normal matrix check
    f2.group_id = -8;
    NF_CHECK(matrix.should_collide(f1, f2));
}

NF_TEST(physics_material_combine_rules) {
    const auto std_mat = PhysicsMaterial::standard();
    const auto ice_mat = PhysicsMaterial::ice();
    const auto rubber_mat = PhysicsMaterial::rubber();

    // Standard + Ice: Ice uses Minimum friction combine
    auto comb_ice = combine_materials(std_mat, ice_mat);
    NF_CHECK_NEAR(comb_ice.static_friction, ice_mat.static_friction, 1e-4f);
    NF_CHECK_NEAR(comb_ice.dynamic_friction, ice_mat.dynamic_friction, 1e-4f);

    // Standard + Rubber: Rubber uses Maximum restitution combine
    auto comb_rubber = combine_materials(std_mat, rubber_mat);
    NF_CHECK_NEAR(comb_rubber.restitution, rubber_mat.restitution, 1e-4f);

    // Custom material with Multiply combine
    PhysicsMaterial bouncy;
    bouncy.restitution = 0.8f;
    bouncy.restitution_combine = CombineMode::Multiply;

    PhysicsMaterial absorptive;
    absorptive.restitution = 0.5f;
    absorptive.restitution_combine = CombineMode::Average;

    auto comb_mult = combine_materials(bouncy, absorptive);
    NF_CHECK_NEAR(comb_mult.restitution, 0.4f, 1e-4f); // 0.8 * 0.5 = 0.4
}

NF_TEST(buoyancy_sphere_floating_and_archimedes) {
    WaterSurface water;
    water.height = 0.0f;
    water.fluid_density = 1000.0f; // kg/m^3
    const f32 gravity = 9.81f;
    const f32 radius = 1.0f;
    const f32 total_vol = (4.0f / 3.0f) * 3.14159265f * (radius * radius * radius);

    // 1. Completely above water (y = 5.0)
    auto res_above = BuoyancySolver::solve_sphere(water, Vec3(0, 5, 0), radius, Vec3(0, 0, 0), Vec3(0, 0, 0), gravity);
    NF_CHECK(!res_above.is_submerged);
    NF_CHECK_EQ(res_above.submerged_fraction, 0.0f);
    NF_CHECK_EQ(res_above.buoyant_force.y, 0.0f);

    // 2. Completely submerged (y = -5.0)
    auto res_sub = BuoyancySolver::solve_sphere(water, Vec3(0, -5, 0), radius, Vec3(0, 0, 0), Vec3(0, 0, 0), gravity);
    NF_CHECK(res_sub.is_submerged);
    NF_CHECK_EQ(res_sub.submerged_fraction, 1.0f);
    const f32 expected_force = 1000.0f * total_vol * gravity;
    NF_CHECK_NEAR(res_sub.buoyant_force.y, expected_force, 5.0f);

    // 3. Half submerged (y = 0.0)
    auto res_half = BuoyancySolver::solve_sphere(water, Vec3(0, 0, 0), radius, Vec3(0, 0, 0), Vec3(0, 0, 0), gravity);
    NF_CHECK(res_half.is_submerged);
    NF_CHECK_NEAR(res_half.submerged_fraction, 0.5f, 0.02f);
    NF_CHECK_NEAR(res_half.buoyant_force.y, expected_force * 0.5f, 5.0f);
    NF_CHECK(res_half.center_of_buoyancy.y < 0.0f); // Centroid is below water surface
}

NF_TEST(buoyancy_box_and_fluid_drag) {
    WaterSurface water;
    water.height = 0.0f;
    water.flow_velocity = Vec3(2.0f, 0.0f, 0.0f); // 2 m/s current in +X
    water.linear_drag = 2.0f;

    const Vec3 box_pos(0.0f, -0.5f, 0.0f); // Submerged
    const Vec3 half_extents(1.0f, 1.0f, 1.0f);
    const Quat orientation = Quat::identity();
    const Vec3 linear_vel(0.0f, 0.0f, 0.0f); // Stationary body relative to world
    const Vec3 angular_vel(0.0f, 1.0f, 0.0f);

    auto res = BuoyancySolver::solve_box(water, box_pos, half_extents, orientation, linear_vel, angular_vel);
    NF_CHECK(res.is_submerged);
    NF_CHECK(res.submerged_fraction > 0.0f);
    NF_CHECK(res.buoyant_force.y > 0.0f);

    // Water flow relative velocity = 0 - 2 = -2 in X, drag force opposes relative velocity -> pushes body along +X flow
    NF_CHECK(res.fluid_drag_force.x > 0.0f);

    // Angular drag opposes positive rotation
    NF_CHECK(res.fluid_drag_torque.y < 0.0f);
}
