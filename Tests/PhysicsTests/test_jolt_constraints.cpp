// PhysicsTests — extended Jolt constraints + ragdoll.

#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/JoltRagdoll.hpp>
#include <NF/Physics/JoltVehicle.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

namespace {

BodyDesc static_plane() {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_plane(Vec3{0, 1, 0});
    return d;
}

void step(JoltWorld& world, int n, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < n; ++i) world.step(dt);
}

} // namespace

// --- Slider constraint ---
NF_TEST(jolt_slider_constrains_to_axis) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc a;
    a.shape = Shape::make_sphere(0.3f);
    a.position = Vec3{0, 5, 0};
    const JoltBody ba = world.add_body(a);
    BodyDesc b;
    b.shape = Shape::make_sphere(0.3f);
    b.position = Vec3{2, 5, 0};
    const JoltBody bb = world.add_body(b);
    const JoltConstraint slider = world.add_slider(ba, bb, Vec3{1, 5, 0}, Vec3{1, 0, 0});
    NF_CHECK(slider.valid());
    step(world, 180);
    const Vec3 pa = world.state(ba).position;
    const Vec3 pb = world.state(bb).position;
    // Both fell under gravity but stayed on the x-axis (y equalized).
    NF_CHECK_NEAR(pa.y, pb.y, 0.3f);
    NF_CHECK(pa.y > 0.2f); // both resting, not through the floor
}

// --- Distance constraint ---
NF_TEST(jolt_distance_preserves_max_length) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc a;
    a.shape = Shape::make_box(Vec3{0.3f, 0.3f, 0.3f});
    a.position = Vec3{0, 5, 0};
    const JoltBody ba = world.add_body(a);
    BodyDesc b = a;
    b.position = Vec3{1.5f, 5, 0};
    const JoltBody bb = world.add_body(b);
    const JoltConstraint dist = world.add_distance(ba, bb, Vec3{0, 5, 0}, Vec3{1.5f, 5, 0}, 0.5f, 2.0f);
    NF_CHECK(dist.valid());
    step(world, 300);
    const Vec3 pa = world.state(ba).position;
    const Vec3 pb = world.state(bb).position;
    const float d = (pb - pa).length();
    NF_CHECK(d <= 2.0f + 0.1f); // max distance honored
    NF_CHECK(d >= 0.5f - 0.1f); // min distance honored
}

// --- Cone constraint ---
NF_TEST(jolt_cone_limits_angle) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc parent;
    parent.type = BodyType::Static;
    parent.shape = Shape::make_box(Vec3{0.1f, 0.1f, 0.1f});
    parent.position = Vec3{0, 5, 0};
    const JoltBody pp = world.add_body(parent);
    BodyDesc bob;
    bob.shape = Shape::make_sphere(0.2f);
    bob.position = Vec3{0, 4, 0};
    const JoltBody pb = world.add_body(bob);
    const float cone_angle = 30.0f * 0.0174533f; // 30 degrees
    const JoltConstraint cone = world.add_cone(pp, pb, Vec3{0, 5, 0}, Vec3{0, -1, 0}, cone_angle);
    NF_CHECK(cone.valid());
    world.set_linear_velocity(pb, Vec3{5, 0, 0}); // push sideways
    float max_angle = 0.0f;
    int measured = 0;
    for (int i = 0; i < 300; ++i) {
        world.step(1.0f / 60.0f);
        // Solver needs a few steps to pull the violently-pushed bob back
        // inside the cone: skip the transient, measure the steady state.
        if (i < 60) continue;
        const Vec3 p = world.state(pb).position;
        const float dx = p.x; // horizontal offset
        const float dy = 5.0f - p.y; // vertical drop (positive = below anchor)
        if (dy > 0.01f) {
            max_angle = std::max(max_angle, std::atan2(dx, dy));
            ++measured;
        }
    }
    NF_CHECK(measured > 0);
    NF_CHECK(max_angle <= cone_angle + 0.1f);
}

// --- Ragdoll spawn and settle ---
NF_TEST(jolt_ragdoll_spawns_and_settles_on_ground) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    std::vector<RagdollJointDesc> joints;
    // Root
    joints.push_back({"pelvis", "", Vec3{0, 0, 0}, 0.18f, 5.0f});
    // Spine above pelvis
    joints.push_back({"spine", "pelvis", Vec3{0, 0.3f, 0}, 0.16f, 3.0f});
    // Head above spine
    joints.push_back({"head", "spine", Vec3{0, 0.3f, 0}, 0.12f, 2.0f});
    // Left thigh
    joints.push_back({"l_thigh", "pelvis", Vec3{-0.15f, -0.2f, 0}, 0.1f, 2.0f});
    // Left shin
    joints.push_back({"l_shin", "l_thigh", Vec3{-0.15f, -0.45f, 0}, 0.08f, 1.5f});

    JoltRagdoll ragdoll(world, joints, Vec3{0, 3, 0});
    NF_CHECK(ragdoll.valid());
    NF_CHECK(ragdoll.joint_count() == 5);
    step(world, 180);
    auto states = ragdoll.joint_states();
    NF_CHECK(states.size() == 5);
    // Pelvis should have fallen and be above the ground
    NF_CHECK(states[0].position.y > 0.1f);
    NF_CHECK(states[0].position.y < 2.5f); // fell from 3m but not through floor
    // Head should be above pelvis (approximately, within constraint limits)
    NF_CHECK(states[2].position.y > states[0].position.y - 0.3f);
}

// --- Ragdoll falls with gravity ---
NF_TEST(jolt_ragdoll_falls_with_gravity) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    std::vector<RagdollJointDesc> joints;
    joints.push_back({"root", "", Vec3{0, 0, 0}, 0.15f, 2.0f});
    joints.push_back({"child", "root", Vec3{0, 0.3f, 0}, 0.12f, 1.0f});

    JoltRagdoll ragdoll(world, joints, Vec3{0, 5, 0});
    NF_CHECK(ragdoll.valid());
    auto s0 = ragdoll.joint_states();
    step(world, 120);
    auto s1 = ragdoll.joint_states();
    // Root fell down (y decreased)
    NF_CHECK(s1[0].position.y < s0[0].position.y - 0.5f);
    // And is above ground
    NF_CHECK(s1[0].position.y > 0.1f);
}

// --- Ragdoll apply_impulse ---
NF_TEST(jolt_ragdoll_impulse_launches_root) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    std::vector<RagdollJointDesc> joints;
    joints.push_back({"root", "", Vec3{0, 0, 0}, 0.15f, 2.0f});
    joints.push_back({"child", "root", Vec3{0, 0.3f, 0}, 0.12f, 1.0f});

    JoltRagdoll ragdoll(world, joints, Vec3{0, 3, 0});
    step(world, 120); // settle
    const Vec3 p0 = ragdoll.joint_states()[0].position;
    // Impulse magnitude is mass-relative: root is 2kg, so 5 Ns barely beats
    // one frame of gravity + the parent constraint. Use 20 Ns and compare
    // against an un-kicked control ragdoll over the same 30 steps.
    JoltRagdoll control(world, joints, Vec3{5, 3, 0});
    step(world, 0);
    ragdoll.apply_impulse(0, Vec3{0, 20, 0}); // upward impulse
    step(world, 30);
    const Vec3 p1 = ragdoll.joint_states()[0].position;
    const Vec3 pc = control.joint_states()[0].position;
    NF_CHECK(p1.y > p0.y - 0.05f); // kicked root did not just keep falling...
    NF_CHECK(p1.y > pc.y); // ...it sits higher than the free-falling control
}

// --- Hinge constrains rotation to a plane ---
NF_TEST(jolt_hinge_limits_rotation_axis) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc pivot;
    pivot.type = BodyType::Static;
    pivot.shape = Shape::make_box(Vec3{0.1f, 0.1f, 0.1f});
    pivot.position = Vec3{0, 8, 0};
    const JoltBody pa = world.add_body(pivot);
    BodyDesc bob;
    bob.shape = Shape::make_sphere(0.3f);
    bob.position = Vec3{0, 5, 0};
    const JoltBody pb = world.add_body(bob);
    const JoltConstraint hinge = world.add_hinge(pa, pb, Vec3{0, 8, 0}, Vec3{0, 0, 1});
    NF_CHECK(hinge.valid());
    // Kick with a z-component too: a ball-and-socket would let z drift, but a
    // z-axis hinge must keep the bob in the x-y plane.
    world.set_linear_velocity(pb, Vec3{3, 0, 1});
    float max_x = 0.0f;
    float max_z_dev = 0.0f;
    for (int i = 0; i < 300; ++i) {
        world.step(1.0f / 60.0f);
        const Vec3 p = world.state(pb).position;
        max_x = std::max(max_x, std::abs(p.x));
        max_z_dev = std::max(max_z_dev, std::abs(p.z)); // start z is 0
    }
    NF_CHECK(max_x > 0.3f);    // swung in the rotation plane
    NF_CHECK(max_z_dev < 0.15f); // ...and stayed on it
}

// --- SixDOF constraint (previously untested) ---
NF_TEST(jolt_sixdof_locks_translation) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc box;
    box.shape = Shape::make_box(Vec3{0.3f, 0.3f, 0.3f});
    box.position = Vec3{0, 3, 0};
    const JoltBody ba = world.add_body(box);
    box.position = Vec3{0, 3, 1.0f};
    const JoltBody bb = world.add_body(box);

    // Lock every axis at 0 (translation AND rotation). Translation alone would
    // pin the shared point but still let the centres swing around it, so the
    // rotation axes are locked too to keep the centre separation constant.
    Vec3 limit_min[6], limit_max[6];
    for (int i = 0; i < 6; ++i) {
        limit_min[i] = Vec3{0, 0, 0};
        limit_max[i] = Vec3{0, 0, 0};
    }
    const JoltConstraint joint =
        world.add_sixdof(ba, bb, Vec3{0, 3, 0.5f}, limit_min, limit_max);
    NF_CHECK(joint.valid());
    const float d0 = (world.state(bb).position - world.state(ba).position).length();
    float min_d = 1e9f, max_d = 0.0f;
    for (int i = 0; i < 120; ++i) {
        world.step(1.0f / 60.0f);
        const float d = (world.state(bb).position - world.state(ba).position).length();
        if (d < min_d) min_d = d;
        if (d > max_d) max_d = d;
    }
    NF_CHECK_NEAR(min_d, d0, 0.1f);
    NF_CHECK_NEAR(max_d, d0, 0.1f);
}

// --- Ragdoll destroyed mid-flight, world stays healthy ---
NF_TEST(jolt_ragdoll_destroy_during_activity) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    std::vector<RagdollJointDesc> joints;
    joints.push_back({"root", "", Vec3{0, 0, 0}, 0.15f, 2.0f});
    joints.push_back({"child", "root", Vec3{0, 0.3f, 0}, 0.12f, 1.0f});
    {
        JoltRagdoll ragdoll(world, joints, Vec3{0, 3, 0});
        NF_CHECK(ragdoll.valid());
        step(world, 30);
        ragdoll.apply_impulse(0, Vec3{5, 0, 0});
        step(world, 30);
    } // destroyed while moving
    step(world, 120);
    BodyDesc ball;
    ball.shape = Shape::make_sphere(0.5f);
    ball.position = Vec3{0, 5, 0};
    const JoltBody b = world.add_body(ball);
    step(world, 180);
    NF_CHECK(world.state(b).position.y > 0.1f);
    NF_CHECK(world.body_count() == 2); // plane + ball; ragdoll bodies removed
}

// --- Ragdoll body access + tight swing limit ---
NF_TEST(jolt_ragdoll_body_at_and_limits) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    std::vector<RagdollJointDesc> joints;
    joints.push_back({"root", "", Vec3{0, 0, 0}, 0.18f, 2.0f});
    joints.push_back({"child", "root", Vec3{0, 0.3f, 0}, 0.12f, 1.0f, 5.0f});
    JoltRagdoll ragdoll(world, joints, Vec3{0, 3, 0});
    NF_CHECK(ragdoll.valid());
    NF_CHECK(ragdoll.body_at(0).valid());
    NF_CHECK(!ragdoll.body_at(999).valid());

    // Violent sideways push on the child. RagdollJointState exposes position
    // but not orientation, so the swing angle itself cannot be asserted here;
    // instead we verify the swing-twist constraint keeps the chain coherent
    // (the child never flies free of its parent) and the ragdoll survives.
    ragdoll.apply_impulse(1, Vec3{15, 0, 0});
    float max_sep = 0.0f;
    for (int i = 0; i < 240; ++i) {
        world.step(1.0f / 60.0f);
        if (i < 60) continue; // skip the solve transient
        auto st = ragdoll.joint_states();
        const float sep = (st[1].position - st[0].position).length();
        if (sep > max_sep) max_sep = sep;
    }
    NF_CHECK(max_sep < 1.5f);
}

// --- Two ragdolls coexist in one world ---
NF_TEST(jolt_two_ragdolls_coexist) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    std::vector<RagdollJointDesc> joints;
    joints.push_back({"root", "", Vec3{0, 0, 0}, 0.15f, 2.0f});
    joints.push_back({"child", "root", Vec3{0, 0.3f, 0}, 0.12f, 1.0f});

    JoltRagdoll a(world, joints, Vec3{0, 3, 0});
    JoltRagdoll b(world, joints, Vec3{2, 3, 0});
    NF_CHECK(a.valid());
    NF_CHECK(b.valid());
    step(world, 180);
    auto sa = a.joint_states();
    auto sb = b.joint_states();
    NF_CHECK(sa.size() == 2 && sb.size() == 2);
    NF_CHECK(sa[0].position.y > 0.1f && sa[0].position.y < 2.5f);
    NF_CHECK(sb[0].position.y > 0.1f && sb[0].position.y < 2.5f);
    NF_CHECK(world.body_count() == 5); // plane + 2 bodies per ragdoll
}

// --- Ragdoll invalid is safe ---
NF_TEST(jolt_ragdoll_invalid_is_safe) {
    JoltWorld world;
    std::vector<RagdollJointDesc> joints;
    JoltRagdoll ragdoll(world, joints, Vec3{0, 0, 0});
    NF_CHECK(!ragdoll.valid());
    NF_CHECK(ragdoll.joint_count() == 0);
    auto states = ragdoll.joint_states();
    NF_CHECK(states.empty());
    ragdoll.apply_impulse(0, Vec3{1, 0, 0}); // no-op, no crash
    ragdoll.activate(); // no-op, no crash
}

// ---------------------------------------------------------------------------
// clone_constraint (نسخ القيود): stamp a joint configuration onto a fresh pair.
// ---------------------------------------------------------------------------

namespace {

std::pair<JoltBody, JoltBody> two_boxes(JoltWorld& world, Vec3 a_pos, Vec3 b_pos) {
    BodyDesc a;
    a.shape = Shape::make_box(Vec3{0.4f, 0.4f, 0.4f});
    a.position = a_pos;
    const JoltBody ba = world.add_body(a);
    BodyDesc b = a;
    b.position = b_pos;
    const JoltBody bb = world.add_body(b);
    return {ba, bb};
}

} // namespace

NF_TEST(jolt_clone_constraint_rejects_bad_input) {
    JoltWorld world;
    world.add_body(static_plane());
    auto [ba, bb] = two_boxes(world, Vec3{0, 4, 0}, Vec3{0, 5.2f, 0});
    const JoltConstraint weld = world.add_fixed(ba, bb);
    NF_CHECK(weld.valid());
    NF_CHECK(!world.clone_constraint(weld, ba, ba).valid()); // same body twice
    NF_CHECK(!world.clone_constraint(weld, ba, JoltBody{}).valid()); // dead body
    NF_CHECK(!world.clone_constraint(JoltConstraint{}, ba, bb).valid()); // dead source
    world.remove_constraint(weld);
    // The source is gone now: cloning a removed constraint must fail cleanly.
    NF_CHECK(!world.clone_constraint(weld, ba, bb).valid());
}

NF_TEST(jolt_clone_fixed_holds_new_pair_together) {
    JoltWorld world;
    world.add_body(static_plane());
    auto [ba, bb] = two_boxes(world, Vec3{0, 4, 0}, Vec3{0, 5.2f, 0});
    const JoltConstraint weld = world.add_fixed(ba, bb);
    NF_CHECK(weld.valid());

    // A second pair, elsewhere in the world, gets a copy of the same weld.
    auto [bc, bd] = two_boxes(world, Vec3{6, 4, 0}, Vec3{6, 5.2f, 0});
    const JoltConstraint clone = world.clone_constraint(weld, bc, bd);
    NF_CHECK(clone.valid());
    NF_CHECK(clone != weld);

    step(world, 300);
    // The template pair stayed 1.2 apart (box centers, edge-to-edge stack).
    const float sep_template =
        (world.state(bb).position - world.state(ba).position).length();
    // ...and so did the cloned pair, at its own spawn offset.
    const float sep_clone = (world.state(bd).position - world.state(bc).position).length();
    NF_CHECK_NEAR(sep_template, 1.2f, 0.2f);
    NF_CHECK_NEAR(sep_clone, 1.2f, 0.2f);
    NF_CHECK_NEAR(sep_template, sep_clone, 0.1f); // identical behavior

    // The clone is a first-class constraint: removing it frees only it.
    world.remove_constraint(clone);
    step(world, 120);
    NF_CHECK(world.is_alive(bc) && world.is_alive(bd));
}

NF_TEST(jolt_clone_hinge_preserves_anchor_and_axis) {
    // Pendulum built on a template pair, then cloned to a second pivot.
    JoltWorld world;
    world.add_body(static_plane());
    BodyDesc pivot_a;
    pivot_a.type = BodyType::Static;
    pivot_a.shape = Shape::make_box(Vec3{0.1f, 0.1f, 0.1f});
    pivot_a.position = Vec3{0, 8, 0};
    const JoltBody pa = world.add_body(pivot_a);
    BodyDesc bob_a;
    bob_a.shape = Shape::make_sphere(0.3f);
    bob_a.position = Vec3{0, 5, 0};
    const JoltBody ba = world.add_body(bob_a);
    const JoltConstraint hinge = world.add_hinge(pa, ba, Vec3{0, 8, 0}, Vec3{0, 0, 1});
    NF_CHECK(hinge.valid());

    // Clone at x=4: same local anchor geometry relative to the new bodies.
    BodyDesc pivot_b = pivot_a;
    pivot_b.position = Vec3{4, 8, 0};
    const JoltBody pb = world.add_body(pivot_b);
    BodyDesc bob_b = bob_a;
    bob_b.position = Vec3{4, 5, 0};
    const JoltBody bb = world.add_body(bob_b);
    const JoltConstraint clone = world.clone_constraint(hinge, pb, bb);
    NF_CHECK(clone.valid());

    world.set_linear_velocity(ba, Vec3{3, 0, 0});
    world.set_linear_velocity(bb, Vec3{3, 0, 0});
    float max_swing_a = 0.0f, max_swing_b = 0.0f;
    for (int i = 0; i < 300; ++i) {
        world.step(1.0f / 60.0f);
        if (i < 30) continue;
        const Vec3 pa_pos = world.state(ba).position;
        const Vec3 pb_pos = world.state(bb).position;
        max_swing_a = std::max(max_swing_a, std::abs(pa_pos.x));
        max_swing_b = std::max(max_swing_b, std::abs(pb_pos.x - 4.0f));
    }
    // Both pendulums swing around their own pivot instead of falling.
    NF_CHECK(max_swing_a > 0.3f);
    NF_CHECK(max_swing_b > 0.3f);
    NF_CHECK_NEAR(max_swing_a, max_swing_b, 0.25f);
}

NF_TEST(jolt_clone_constraint_source_untouched) {
    JoltWorld world;
    world.add_body(static_plane());
    auto [ba, bb] = two_boxes(world, Vec3{0, 4, 0}, Vec3{0, 5.2f, 0});
    const JoltConstraint weld = world.add_fixed(ba, bb);
    const usize before = world.body_count();

    auto [bc, bd] = two_boxes(world, Vec3{3, 4, 0}, Vec3{3, 5.2f, 0});
    const JoltConstraint clone = world.clone_constraint(weld, bc, bd);
    NF_CHECK(clone.valid());
    // Cloning never destroys or alters the template pair.
    NF_CHECK(world.is_alive(ba) && world.is_alive(bb));
    NF_CHECK(world.body_count() == before + 2);
}

NF_TEST(jolt_clone_constraint_safe_around_vehicles) {
    // Vehicles hold their own constraint type (not a two-body constraint).
    // Cloning ordinary constraints in a world that also runs a vehicle must
    // neither corrupt the vehicle nor be corrupted by it.
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    NF_CHECK(car.valid());

    auto [ba, bb] = two_boxes(world, Vec3{4, 4, 0}, Vec3{4, 5.2f, 0});
    const JoltConstraint weld = world.add_fixed(ba, bb);
    NF_CHECK(weld.valid());
    auto [bc, bd] = two_boxes(world, Vec3{8, 4, 0}, Vec3{8, 5.2f, 0});
    const JoltConstraint clone = world.clone_constraint(weld, bc, bd);
    NF_CHECK(clone.valid());

    for (int i = 0; i < 240; ++i) {
        car.drive(1.0f, 0.0f);
        world.step(1.0f / 60.0f);
    }
    NF_CHECK(car.speed_ms() > 0.5f); // the vehicle still drives
    const float sep_clone = (world.state(bd).position - world.state(bc).position).length();
    NF_CHECK_NEAR(sep_clone, 1.2f, 0.2f); // and the cloned weld still holds
}


