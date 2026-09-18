// PhysicsTests — Jolt backend: falling, resting, stacking, determinism.

#include <NF/Physics/JoltWorld.hpp>
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

NF_TEST(jolt_sphere_falls_and_rests_on_plane) {
    JoltWorld world;
    NF_CHECK(world.valid());
    NF_CHECK(world.add_body(static_plane()).valid());

    BodyDesc ball;
    ball.shape = Shape::make_sphere(0.5f);
    ball.position = Vec3{0, 5, 0};
    const JoltBody b = world.add_body(ball);
    NF_CHECK(b.valid());
    NF_CHECK(world.is_alive(b));
    NF_CHECK(world.body_count() == 2);

    step(world, 300);
    const JoltBodyState st = world.state(b);
    // Resting center one radius above the plane, nearly still.
    NF_CHECK_NEAR(st.position.y, 0.5f, 0.05f);
    NF_CHECK(std::abs(st.linear_velocity.y) < 0.5f);
}

NF_TEST(jolt_box_stack_settles) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc box;
    box.shape = Shape::make_box(Vec3{0.5f, 0.5f, 0.5f});
    box.position = Vec3{0, 0.6f, 0};
    const JoltBody lower = world.add_body(box);
    box.position = Vec3{0, 1.7f, 0};
    const JoltBody upper = world.add_body(box);
    step(world, 600);
    const JoltBodyState lo = world.state(lower);
    const JoltBodyState hi = world.state(upper);
    // Lower rests on the plane, upper rests on the lower: centers ~0.5, ~1.5.
    NF_CHECK_NEAR(lo.position.y, 0.5f, 0.1f);
    NF_CHECK_NEAR(hi.position.y, 1.5f, 0.15f);

    world.remove_body(upper);
    NF_CHECK(!world.is_alive(upper));
    NF_CHECK(world.body_count() == 2);
    world.remove_body(lower);
    NF_CHECK(world.body_count() == 1);
}

NF_TEST(jolt_state_reports_body_orientation) {
    JoltWorld world;
    BodyDesc box;
    box.type = BodyType::Static; // static: nothing integrates over the spawn pose
    box.shape = Shape::make_box(Vec3{0.5f, 0.5f, 0.5f});
    box.position = Vec3{0, 1, 0};
    const Quat yaw = Quat::from_axis_angle(Vec3{0, 1, 0}, 1.5707963f); // 90 degrees
    box.orientation = yaw;
    const JoltBody b = world.add_body(box);
    NF_CHECK(b.valid());

    const JoltBodyState st = world.state(b);
    NF_CHECK_NEAR(st.rotation.length_sq(), 1.0f, 1e-5f);
    // Compare the rotation by what it DOES, not by its components: q and -q
    // describe the same orientation, and which of the two Jolt hands back is
    // not a contract anyone should depend on.
    const Vec3 axis{0, 0, 1};
    const Vec3 want = yaw.rotate(axis);
    const Vec3 got = st.rotation.rotate(axis);
    NF_CHECK_NEAR(got.x, want.x, 1e-4f);
    NF_CHECK_NEAR(got.y, want.y, 1e-4f);
    NF_CHECK_NEAR(got.z, want.z, 1e-4f);

    // An unrotated body reports identity; so does a dead handle.
    BodyDesc plain;
    plain.type = BodyType::Static;
    plain.shape = Shape::make_box(Vec3{0.5f, 0.5f, 0.5f});
    const JoltBody p = world.add_body(plain);
    NF_CHECK_NEAR(world.state(p).rotation.w, 1.0f, 1e-5f);
    NF_CHECK_NEAR(world.state(JoltBody{}).rotation.w, 1.0f, 1e-6f);
}

NF_TEST(jolt_set_velocity_moves_bodies) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc ball;
    ball.shape = Shape::make_sphere(0.5f);
    ball.position = Vec3{0, 2, 0};
    const JoltBody b = world.add_body(ball);
    world.set_linear_velocity(b, Vec3{4, 0, 0});
    step(world, 60);
    const JoltBodyState st = world.state(b);
    NF_CHECK(st.position.x > 2.0f); // flew sideways instead of dropping straight
}

NF_TEST(jolt_determinism_same_steps_same_state) {
    auto run = [] {
        JoltWorld world;
        world.add_body(static_plane());
        BodyDesc ball;
        ball.shape = Shape::make_sphere(0.5f);
        ball.position = Vec3{0.3f, 4, -0.2f};
        const JoltBody b = world.add_body(ball);
        step(world, 240);
        return world.state(b).position;
    };
    const Vec3 a = run();
    const Vec3 b = run();
    NF_CHECK_NEAR(a.x, b.x, 1e-6f);
    NF_CHECK_NEAR(a.y, b.y, 1e-6f);
    NF_CHECK_NEAR(a.z, b.z, 1e-6f);
}

NF_TEST(jolt_invalid_handles_are_safe) {
    JoltWorld world;
    const JoltBody dead;
    NF_CHECK(!dead.valid());
    NF_CHECK(!world.is_alive(dead));
    const JoltBodyState st = world.state(dead);
    NF_CHECK_NEAR(st.position.x, 0.0f, 1e-6f);
    world.remove_body(dead); // no-op, no crash
    world.set_linear_velocity(dead, Vec3{1, 0, 0});
    world.step(0.0f); // no-op
    world.step(-1.0f); // no-op
}
