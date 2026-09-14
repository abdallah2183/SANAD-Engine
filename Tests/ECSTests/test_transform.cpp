// Tests/ECSTests/test_transform.cpp — Transform hierarchy

#include <NF/Test/TestFramework.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>

namespace {

using namespace nf;
using namespace nf::ecs;
using namespace nf::scene;

} // namespace

NF_TEST(transform_parent_child) {
    World world;
    Entity parent = world.create_entity();
    Entity child = world.create_entity();
    world.add<Transform>(parent, Transform{});
    world.add<Transform>(child, Transform{});
    auto* pt = world.get<Transform>(parent);
    auto* ct = world.get<Transform>(child);
    pt->local_x = 10; pt->local_y = 0; pt->local_z = 0;
    ct->local_x = 5; ct->local_y = 0; ct->local_z = 0;
    set_parent(world, child, parent);
    NF_CHECK(get_parent(world, child) == parent);
    auto children = get_children(world, parent);
    NF_CHECK(children.size()==1 && children[0]==child);
    propagate_transforms(world);
    NF_CHECK(ct->world_x == 15);
    NF_CHECK(pt->world_x == 10);
}

NF_TEST(transform_hierarchy_propagation_three_levels) {
    World world;
    Entity grand = world.create_entity();
    Entity parent = world.create_entity();
    Entity child = world.create_entity();
    world.add<Transform>(grand, Transform{});
    world.add<Transform>(parent, Transform{});
    world.add<Transform>(child, Transform{});
    world.get<Transform>(grand)->local_x = 10;
    world.get<Transform>(parent)->local_x = 5;
    world.get<Transform>(child)->local_x = 2;
    set_parent(world, parent, grand);
    set_parent(world, child, parent);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(grand)->world_x == 10);
    NF_CHECK(world.get<Transform>(parent)->world_x == 15);
    NF_CHECK(world.get<Transform>(child)->world_x == 17);
}

NF_TEST(transform_reparent) {
    World world;
    Entity a = world.create_entity();
    Entity b = world.create_entity();
    Entity c = world.create_entity();
    world.add<Transform>(a, Transform{}); world.add<Transform>(b, Transform{}); world.add<Transform>(c, Transform{});
    world.get<Transform>(a)->local_x = 10;
    world.get<Transform>(b)->local_x = 5;
    world.get<Transform>(c)->local_x = 1;
    set_parent(world, b, a);
    set_parent(world, c, b);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(c)->world_x == 16);
    // Reparent c to a directly
    set_parent(world, c, a);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(c)->world_x == 11);
    NF_CHECK(get_parent(world, c) == a);
    auto children_of_a = get_children(world, a);
    NF_CHECK(children_of_a.size()==2); // b and c
}

NF_TEST(transform_detach) {
    World world;
    Entity p = world.create_entity();
    Entity ch = world.create_entity();
    world.add<Transform>(p, Transform{}); world.add<Transform>(ch, Transform{});
    world.get<Transform>(p)->local_x = 7;
    world.get<Transform>(ch)->local_x = 3;
    set_parent(world, ch, p);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(ch)->world_x == 10);
    remove_parent(world, ch);
    NF_CHECK(!get_parent(world, ch).valid());
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(ch)->world_x == 3);
}

NF_TEST(transform_prevents_cycle) {
    World world;
    Entity a = world.create_entity();
    Entity b = world.create_entity();
    world.add<Transform>(a, Transform{}); world.add<Transform>(b, Transform{});
    set_parent(world, b, a);
    // Try to make a child of b (would create cycle a->b->a)
    set_parent(world, a, b);
    // Should have been rejected, so a should still have no parent
    NF_CHECK(!get_parent(world, a).valid());
}

// --- Euler <-> quaternion round trip ---------------------------------------
//
// The physics solver works in quaternions; the Transform stores XYZ euler
// degrees. Writing a rotating body back into a Transform needs the two to agree
// on the convention exactly, and a mismatch produces a body that drifts or spins
// about the wrong axis — which looks like a physics bug and is not one.

namespace {

/// compose_trs with unit scale, as a Mat4, for comparison.
nf::Mat4 trs_matrix(float rx, float ry, float rz) {
    float m16[16];
    nf::scene::compose_trs(0.0f, 0.0f, 0.0f, rx, ry, rz, 1.0f, 1.0f, 1.0f, m16);
    nf::Mat4 m = nf::Mat4::identity();
    for (int i = 0; i < 16; ++i) {
        m.m[i / 4][i % 4] = m16[i]; // m16 is column-major [col*4 + row]; Mat4 is m[col][row]
    }
    return m;
}

bool matrices_match(const nf::Mat4& a, const nf::Mat4& b, float tol) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::fabs(a.m[c][r] - b.m[c][r]) > tol) return false;
        }
    }
    return true;
}

} // namespace

NF_TEST(euler_round_trips_through_a_quaternion) {
    const float angles[6][3] = {
        {0.0f, 0.0f, 0.0f},
        {30.0f, 0.0f, 0.0f},
        {0.0f, 45.0f, 0.0f},
        {0.0f, 0.0f, 60.0f},
        {20.0f, -35.0f, 50.0f},
        {-80.0f, 15.0f, -120.0f},
    };
    for (const auto& a : angles) {
        const Mat4 original = trs_matrix(a[0], a[1], a[2]);
        const Quat q = Quat::from_matrix(original);

        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        scene::euler_xyz_degrees_from_quat(q, rx, ry, rz);

        // The angles may differ (euler representations are not unique) but the
        // ROTATION they produce must be identical.
        const Mat4 rebuilt = trs_matrix(rx, ry, rz);
        NF_CHECK(matrices_match(original, rebuilt, 1e-3f));
    }
}

NF_TEST(euler_round_trips_at_gimbal_lock) {
    // cos(x) == 0: Y and Z become the same axis and the decomposition has to
    // pick a convention rather than divide by zero.
    const Mat4 original = trs_matrix(90.0f, 40.0f, 25.0f);
    const Quat q = Quat::from_matrix(original);

    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    scene::euler_xyz_degrees_from_quat(q, rx, ry, rz);

    NF_CHECK(std::isfinite(rx) && std::isfinite(ry) && std::isfinite(rz));
    NF_CHECK(matrices_match(original, trs_matrix(rx, ry, rz), 1e-3f));
}

NF_TEST(euler_of_identity_is_zero) {
    float rx = 999.0f, ry = 999.0f, rz = 999.0f;
    scene::euler_xyz_degrees_from_quat(Quat::identity(), rx, ry, rz);
    NF_CHECK_NEAR(rx, 0.0f, 1e-4f);
    NF_CHECK_NEAR(ry, 0.0f, 1e-4f);
    NF_CHECK_NEAR(rz, 0.0f, 1e-4f);
}

NF_TEST(euler_forward_and_inverse_are_consistent) {
    const float angles[4][3] = {
        {0.0f, 0.0f, 0.0f},
        {25.0f, 0.0f, 0.0f},
        {0.0f, 70.0f, 0.0f},
        {15.0f, -40.0f, 65.0f},
    };
    for (const auto& a : angles) {
        const Quat q = scene::quat_from_euler_xyz_degrees(a[0], a[1], a[2]);
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        scene::euler_xyz_degrees_from_quat(q, rx, ry, rz);
        // Round trip through both directions must reproduce the same rotation,
        // which is what a physics body written back into a Transform relies on.
        NF_CHECK(matrices_match(trs_matrix(a[0], a[1], a[2]), trs_matrix(rx, ry, rz), 1e-3f));
    }
}
