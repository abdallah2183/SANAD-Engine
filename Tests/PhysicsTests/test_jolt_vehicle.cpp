// PhysicsTests — Jolt vehicles + two-body constraints.

#include <NF/Physics/JoltVehicle.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Test/TestFramework.hpp>

#include <algorithm>
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

BodyDesc static_box(Vec3 half, Vec3 pos) {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_box(half);
    d.position = pos;
    return d;
}

void step(JoltWorld& world, int n, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < n; ++i) world.step(dt);
}

/// Full throttle, zero steer: returns the chassis state after `seconds` of
/// driving. Shared by the straight-line stability tests so they cannot drift
/// apart in their warm-up counts.
JoltBodyState cruise_straight(JoltVehicle& car, JoltWorld& world, int steps) {
    for (int i = 0; i < steps; ++i) {
        car.drive(1.0f, 0.0f);
        world.step(1.0f / 60.0f);
    }
    return car.chassis_state();
}

/// Chassis up-vector in world space: <0 means the car is upside down.
Vec3 chassis_up(const JoltBodyState& st) {
    return st.rotation.rotate(Vec3{0, 1, 0});
}

} // namespace

NF_TEST(jolt_vehicle_drives_straight_with_no_steer) {
    // No uncommanded yaw: full throttle and zero steer for 10 s must carry
    // the car down +Z, not into a spin. (A drift here reads in-game as the
    // car "veering on its own".)
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    step(world, 120); // settle, no input
    for (int i = 0; i < 600; ++i) {
        car.drive(1.0f, 0.0f);
        world.step(1.0f / 60.0f);
    }
    const JoltBodyState st = car.chassis_state();
    NF_CHECK(car.speed_ms() > 5.0f);
    NF_CHECK(st.position.z > 10.0f);
    NF_CHECK(std::fabs(st.position.x) < 0.30f * st.position.z); // mostly +Z
    const Vec3 fwd = st.rotation.rotate(Vec3{0, 0, 1});
    NF_CHECK(fwd.z > 0.95f); // heading still ~straight
    NF_CHECK(std::fabs(fwd.x) < 0.30f);
}

NF_TEST(jolt_vehicle_ctor_only_no_step) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    {
        JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
        NF_CHECK(car.valid());
        // No step: if this crashes, construction/teardown alone is broken.
    }
    NF_CHECK(true);
}

NF_TEST(jolt_vehicle_spawns_and_settles_on_wheels) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    JoltVehicleConfig cfg;
    JoltVehicle car(world, cfg, Vec3{0, 2, 0});
    NF_CHECK(car.valid());
    step(world, 180);
    const JoltBodyState st = car.chassis_state();
    // Settled on suspension: chassis center well under the 2m spawn, above ground.
    NF_CHECK(st.position.y < 1.5f);
    NF_CHECK(st.position.y > 0.2f);
    NF_CHECK_NEAR(car.speed_ms(), 0.0f, 0.5f);
}

NF_TEST(jolt_vehicle_drives_forward) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    JoltVehicleConfig cfg;
    JoltVehicle car(world, cfg, Vec3{0, 2, 0});
    step(world, 120); // settle first: launches from rest are unrealistic
    const Vec3 p0 = car.chassis_state().position;
    // Re-assert throttle every step: the Jolt wheeled controller consumes
    // driver input through the step listener, and a single pre-sleep call
    // does not keep the drivetrain engaged across 180 steps.
    for (int i = 0; i < 180; ++i) {
        car.drive(1.0f, 0.0f);
        world.step(1.0f / 60.0f);
    }
    car.drive(0.0f, 0.0f);
    const Vec3 p1 = car.chassis_state().position;
    const float dx = p1.x - p0.x;
    const float dz = p1.z - p0.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    NF_CHECK(dist > 3.0f); // it actually drives
    NF_CHECK(car.speed_ms() > 1.0f); // and carries speed
    NF_CHECK(p1.y > 0.1f && p1.y < 2.0f); // without launching to orbit
}

NF_TEST(jolt_vehicle_steering_changes_heading) {
    auto run = [](float steer) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
        for (int i = 0; i < 180; ++i) {
            car.drive(1.0f, steer);
            world.step(1.0f / 60.0f);
        }
        return car.chassis_state().position;
    };
    const Vec3 straight = run(0.0f);
    const Vec3 turned = run(0.8f);
    // Same throttle, different steering: different destinations.
    const float dx = turned.x - straight.x;
    const float dz = turned.z - straight.z;
    NF_CHECK(std::sqrt(dx * dx + dz * dz) > 1.0f);
}

NF_TEST(jolt_vehicle_chassis_state_carries_heading) {
    // A snapshot that carries position but no orientation renders a car
    // sliding sideways through a corner, so chassis_state() must report the
    // pose the renderer and the wire both need.
    auto run = [](float steer) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
        for (int i = 0; i < 180; ++i) {
            car.drive(1.0f, steer);
            world.step(1.0f / 60.0f);
        }
        return car.chassis_state();
    };
    const JoltBodyState straight = run(0.0f);
    const JoltBodyState turned = run(0.8f);
    NF_CHECK_NEAR(straight.rotation.length_sq(), 1.0f, 1e-4f);
    NF_CHECK_NEAR(turned.rotation.length_sq(), 1.0f, 1e-4f);
    // |dot| == 1 is the identical orientation; a real turn reads below it.
    NF_CHECK(std::fabs(straight.rotation.dot(turned.rotation)) < 0.999f);
}

NF_TEST(jolt_vehicle_reverses) {
    auto run = [](float throttle) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
        for (int i = 0; i < 180; ++i) {
            car.drive(throttle, 0.0f);
            world.step(1.0f / 60.0f);
        }
        return car.chassis_state().position;
    };
    const Vec3 forward = run(1.0f);
    const Vec3 reverse = run(-1.0f);
    // Forward throttle drives +z; negative throttle drives the opposite way.
    NF_CHECK(forward.z > 0.5f);
    NF_CHECK(reverse.z < -0.5f);
    const float dx = reverse.x;
    const float dz = reverse.z;
    NF_CHECK(std::sqrt(dx * dx + dz * dz) > 1.0f); // it really reversed
}

NF_TEST(jolt_vehicle_brakes_to_stop) {
    struct Result {
        float speed;
        float dist;
    };
    auto run = [](bool brake) -> Result {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f); // settle
        for (int i = 0; i < 120; ++i) { // accelerate both cars identically
            car.drive(1.0f, 0.0f);
            world.step(1.0f / 60.0f);
        }
        const Vec3 p0 = car.chassis_state().position;
        for (int i = 0; i < 120; ++i) { // coast, or brake
            car.drive(0.0f, 0.0f, brake ? 1.0f : 0.0f);
            world.step(1.0f / 60.0f);
        }
        const Vec3 p1 = car.chassis_state().position;
        const float dx = p1.x - p0.x;
        const float dz = p1.z - p0.z;
        return Result{car.speed_ms(), std::sqrt(dx * dx + dz * dz)};
    };
    const Result coast = run(false);
    const Result braked = run(true);
    // The braking car ends slower and covers less ground than the coaster.
    NF_CHECK(braked.speed < coast.speed);
    NF_CHECK(braked.dist < coast.dist);
}

NF_TEST(jolt_vehicle_destroy_mid_simulation) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    {
        JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
        NF_CHECK(car.valid());
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f); // settle
        for (int i = 0; i < 60; ++i) { // drive while active
            car.drive(1.0f, 0.0f);
            world.step(1.0f / 60.0f);
        }
        NF_CHECK(car.speed_ms() > 0.1f);
    } // destructor tears the vehicle down mid-simulation
    // The world keeps simulating, and a fresh body still settles normally.
    step(world, 120);
    BodyDesc ball;
    ball.shape = Shape::make_sphere(0.5f);
    ball.position = Vec3{0, 5, 0};
    const JoltBody b = world.add_body(ball);
    step(world, 180);
    NF_CHECK(world.state(b).position.y > 0.1f);
    NF_CHECK(world.state(b).position.y < 1.5f);
    NF_CHECK(world.body_count() == 2); // plane + ball; vehicle chassis gone
}

NF_TEST(jolt_vehicle_config_variants_are_valid) {
    auto run = [](const JoltVehicleConfig& cfg) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, cfg, Vec3{0, 2, 0});
        if (!car.valid()) return false;
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
        const Vec3 p0 = car.chassis_state().position;
        for (int i = 0; i < 180; ++i) {
            car.drive(1.0f, 0.0f);
            world.step(1.0f / 60.0f);
        }
        const Vec3 p1 = car.chassis_state().position;
        const float dx = p1.x - p0.x;
        const float dz = p1.z - p0.z;
        return std::sqrt(dx * dx + dz * dz) > 1.0f;
    };
    JoltVehicleConfig a;
    a.chassis_mass = 0.0f; // must fall back to the 1500kg default
    NF_CHECK(run(a));
    JoltVehicleConfig b;
    b.track_half_width *= 1.4f;
    NF_CHECK(run(b));
    JoltVehicleConfig c;
    c.max_steer_deg = 0.0f; // steering does nothing; straight still drives
    NF_CHECK(run(c));
}

NF_TEST(jolt_vehicle_deterministic) {
    auto run = [] {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
        for (int i = 0; i < 240; ++i) {
            car.drive(0.6f, 0.2f);
            world.step(1.0f / 60.0f);
        }
        return car.chassis_state().position;
    };
    const Vec3 a = run();
    const Vec3 b = run();
    NF_CHECK_NEAR(a.x, b.x, 1e-3f);
    NF_CHECK_NEAR(a.y, b.y, 1e-3f);
    NF_CHECK_NEAR(a.z, b.z, 1e-3f);
}

NF_TEST(jolt_fixed_weld_moves_as_one) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc a;
    a.shape = Shape::make_box(Vec3{0.5f, 0.5f, 0.5f});
    a.position = Vec3{0, 3, 0};
    const JoltBody ba = world.add_body(a);
    BodyDesc b = a;
    b.position = Vec3{0, 4.1f, 0};
    const JoltBody bb = world.add_body(b);
    const JoltConstraint weld = world.add_fixed(ba, bb);
    NF_CHECK(weld.valid());
    step(world, 300);
    const Vec3 pa = world.state(ba).position;
    const Vec3 pb = world.state(bb).position;
    // Welded pair fell together: 1.1 separation preserved, both above ground.
    NF_CHECK_NEAR((pb - pa).length(), 1.1f, 0.15f);
    NF_CHECK(pa.y > 0.3f && pb.y > pa.y);
    world.remove_constraint(weld);
    // After release both still simulate (no crash, bodies alive).
    step(world, 60);
    NF_CHECK(world.is_alive(ba) && world.is_alive(bb));
}

NF_TEST(jolt_hinge_swing_preserves_anchor_distance) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    // Pendulum bob hanging from a high anchor.
    BodyDesc bob;
    bob.shape = Shape::make_sphere(0.3f);
    bob.position = Vec3{0, 5, 0};
    // A static anchor block at the pivot (small, out of the way).
    BodyDesc pivot;
    pivot.type = BodyType::Static;
    pivot.shape = Shape::make_box(Vec3{0.1f, 0.1f, 0.1f});
    pivot.position = Vec3{0, 8, 0};
    const JoltBody pa = world.add_body(pivot);
    const JoltBody pb = world.add_body(bob);
    const JoltConstraint hinge =
        world.add_hinge(pa, pb, Vec3{0, 8, 0}, Vec3{0, 0, 1});
    NF_CHECK(hinge.valid());
    // Nudge sideways so it swings instead of hanging dead still.
    world.set_linear_velocity(pb, Vec3{3, 0, 0});
    float min_anchor_dist = 1e9f;
    float max_anchor_dist = 0.0f;
    float max_swing_x = 0.0f;
    for (int i = 0; i < 300; ++i) {
        world.step(1.0f / 60.0f);
        const Vec3 p = world.state(pb).position;
        const float d = std::sqrt(p.x * p.x + (p.y - 8.0f) * (p.y - 8.0f) + p.z * p.z);
        if (d < min_anchor_dist) min_anchor_dist = d;
        if (d > max_anchor_dist) max_anchor_dist = d;
        if (std::abs(p.x) > max_swing_x) max_swing_x = std::abs(p.x);
    }
    // Ball-and-socket distance from the pivot stays ~3 (rigid rod).
    NF_CHECK_NEAR(min_anchor_dist, 3.0f, 0.25f);
    NF_CHECK_NEAR(max_anchor_dist, 3.0f, 0.25f);
    // ...and it actually swung instead of hanging dead still.
    NF_CHECK(max_swing_x > 0.3f);
}

NF_TEST(jolt_point_joint_holds_dangling_body) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    // Static anchor with a dynamic box dangling from a shared world point.
    BodyDesc anchor;
    anchor.type = BodyType::Static;
    anchor.shape = Shape::make_box(Vec3{0.1f, 0.1f, 0.1f});
    anchor.position = Vec3{0, 6, 0};
    const JoltBody pa = world.add_body(anchor);
    BodyDesc box;
    box.shape = Shape::make_box(Vec3{0.4f, 0.4f, 0.4f});
    box.position = Vec3{0, 4, 0};
    const JoltBody pb = world.add_body(box);
    const JoltConstraint joint = world.add_point(pa, pb, Vec3{0, 6, 0});
    NF_CHECK(joint.valid());
    world.set_linear_velocity(pb, Vec3{2, 0, 0}); // swing it
    float min_dist = 1e9f, max_dist = 0.0f, max_x = 0.0f;
    for (int i = 0; i < 300; ++i) {
        world.step(1.0f / 60.0f);
        const Vec3 p = world.state(pb).position;
        const float d = std::sqrt(p.x * p.x + (p.y - 6.0f) * (p.y - 6.0f) + p.z * p.z);
        if (d < min_dist) min_dist = d;
        if (d > max_dist) max_dist = d;
        if (std::abs(p.x) > max_x) max_x = std::abs(p.x);
    }
    // The box's anchor point rides the joint: center stays ~2 below it.
    NF_CHECK_NEAR(min_dist, 2.0f, 0.25f);
    NF_CHECK_NEAR(max_dist, 2.0f, 0.25f);
    NF_CHECK(max_x > 0.2f); // swung, never fell to the plane
}

NF_TEST(jolt_constraints_reject_dead_handles) {
    JoltWorld world;
    const JoltBody dead;
    NF_CHECK(!world.add_fixed(dead, dead).valid());
    NF_CHECK(!world.add_hinge(dead, dead, Vec3{}, Vec3{0, 1, 0}).valid());
    NF_CHECK(!world.add_hinge(dead, dead, Vec3{}, Vec3{}).valid()); // zero axis
    NF_CHECK(!world.add_point(dead, dead, Vec3{}).valid());
    world.remove_constraint(JoltConstraint{}); // no-op
}

// ---------------------------------------------------------------------------
// Wheel state readback (rendering/surface feedback) + vehicle reset.
// ---------------------------------------------------------------------------

NF_TEST(jolt_vehicle_wheel_states_settle_on_ground) {
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    step(world, 180); // settle on the plane
    const std::vector<JoltWheelState> wheels = car.wheel_states();
    NF_CHECK(wheels.size() == 4); // FL, FR, RL, RR
    int grounded = 0;
    for (const JoltWheelState& w : wheels) {
        if (w.in_contact) ++grounded;
        // Wheels sit on the plane: above the floor, under the spawn height.
        NF_CHECK(w.position.y > 0.05f);
        NF_CHECK(w.position.y < 1.0f);
        // A grounded wheel reports an upward-ish contact normal.
        if (w.in_contact) NF_CHECK(w.contact_normal.y > 0.5f);
    }
    NF_CHECK(grounded >= 2); // at least half the wheels carry the car
}

NF_TEST(jolt_vehicle_wheel_states_airborne_and_driven) {
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 6, 0});
    // Freshly spawned high up: no wheel touches anything yet.
    for (const JoltWheelState& w : car.wheel_states()) NF_CHECK(!w.in_contact);
    step(world, 300); // fall and settle
    for (const JoltWheelState& w : car.wheel_states()) NF_CHECK(w.in_contact);

    // Drive: the driven wheels must spin (non-zero angular velocity).
    for (int i = 0; i < 180; ++i) {
        car.drive(1.0f, 0.0f);
        world.step(1.0f / 60.0f);
    }
    float max_spin = 0.0f;
    for (const JoltWheelState& w : car.wheel_states())
        max_spin = std::max(max_spin, std::abs(w.angular_velocity));
    NF_CHECK(max_spin > 0.5f);

    // Steering shows up as a steer angle on the front axle only.
    for (int i = 0; i < 60; ++i) {
        car.drive(0.0f, 1.0f);
        world.step(1.0f / 60.0f);
    }
    const std::vector<JoltWheelState> steered = car.wheel_states();
    NF_CHECK(steered.size() == 4);
    NF_CHECK(std::abs(steered[0].steer_angle) > 0.01f); // front-left steers
    NF_CHECK(std::abs(steered[1].steer_angle) > 0.01f); // front-right steers
    NF_CHECK(std::abs(steered[2].steer_angle) < 0.01f); // rear-left does not
    NF_CHECK(std::abs(steered[3].steer_angle) < 0.01f); // rear-right does not
}

NF_TEST(jolt_vehicle_reset_teleports_and_stops) {
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    step(world, 120);
    for (int i = 0; i < 180; ++i) { // build up speed
        car.drive(1.0f, 0.0f);
        world.step(1.0f / 60.0f);
    }
    NF_CHECK(car.speed_ms() > 1.0f);

    // Reset: teleport far away and zero the velocity.
    const Vec3 before = car.chassis_state().position;
    car.reset(Vec3{20, 2, 20});
    const Vec3 after = car.chassis_state().position;
    NF_CHECK((after - before).length() > 5.0f);
    NF_CHECK_NEAR(car.speed_ms(), 0.0f, 0.5f);
    NF_CHECK_NEAR(after.x, 20.0f, 0.5f);
    NF_CHECK_NEAR(after.z, 20.0f, 0.5f);

    // The vehicle survives the teleport and drives again from the new spot —
    // i.e. reset kept the constraint/wheel wiring intact.
    const Vec3 p0 = car.chassis_state().position;
    for (int i = 0; i < 240; ++i) {
        car.drive(1.0f, 0.0f);
        world.step(1.0f / 60.0f);
    }
    NF_CHECK(car.speed_ms() > 0.5f);
    NF_CHECK((car.chassis_state().position - p0).length() > 1.0f);
    // Wheels still report coherent state after the teleport.
    NF_CHECK(car.wheel_states().size() == 4);
}

NF_TEST(jolt_vehicle_reset_and_clone_coexist_with_world) {
    // A reset vehicle and a cloned constraint share one world without
    // interfering: this is the multiplayer-respawn + prefab-joint scenario.
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    step(world, 120);

    BodyDesc a;
    a.shape = Shape::make_box(Vec3{0.4f, 0.4f, 0.4f});
    a.position = Vec3{5, 4, 0};
    const JoltBody ba = world.add_body(a);
    BodyDesc b = a;
    b.position = Vec3{5, 5.2f, 0};
    const JoltBody bb = world.add_body(b);
    const JoltConstraint weld = world.add_fixed(ba, bb);

    BodyDesc c = a;
    c.position = Vec3{7, 4, 0};
    const JoltBody bc = world.add_body(c);
    BodyDesc d = a;
    d.position = Vec3{7, 5.2f, 0};
    const JoltBody bd = world.add_body(d);
    const JoltConstraint clone = world.clone_constraint(weld, bc, bd);
    NF_CHECK(clone.valid());

    car.reset(Vec3{0, 3, 0});
    for (int i = 0; i < 180; ++i) {
        car.drive(1.0f, 0.3f);
        world.step(1.0f / 60.0f);
    }
    NF_CHECK(car.speed_ms() > 0.5f);
    NF_CHECK_NEAR((world.state(bd).position - world.state(bc).position).length(), 1.2f, 0.2f);
}

NF_TEST(jolt_vehicle_sustained_full_throttle_stays_straight_and_upright) {
    // 30 s of full throttle, zero steer: the run must stay on +Z, near the
    // X = 0 plane, and rubber-side down. This is the regression guard for the
    // car "veering on its own" at top speed — a drift here is invisible in
    // the short existing drive test but obvious in the demo.
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    step(world, 120); // settle onto the wheels first
    const JoltBodyState st = cruise_straight(car, world, 1800); // 30 s

    NF_CHECK(st.position.z > 40.0f); // it covered ground
    NF_CHECK(car.speed_ms() > 8.0f);
    // Lateral drift stays a small fraction of the distance travelled.
    NF_CHECK(std::fabs(st.position.x) < 0.05f * st.position.z);
    const Vec3 fwd = st.rotation.rotate(Vec3{0, 0, 1});
    NF_CHECK(fwd.z > 0.97f);
    NF_CHECK(std::fabs(fwd.x) < 0.20f);
    // Upright the whole way: an arcing roll shows up here long before the
    // heading check catches it.
    NF_CHECK(chassis_up(st).y > 0.90f);
}

NF_TEST(jolt_vehicle_crosses_a_curb_at_speed_without_rolling) {
    // A low static curb across the path at full throttle: the suspension tune
    // (travel + damping) has to absorb it without putting the car on its roof.
    // The demo's obstacle field is the unscripted version of this.
    JoltWorld world;
    world.add_body(static_plane());
    world.add_body(static_box(Vec3{3.0f, 0.12f, 0.5f}, Vec3{0, 0.12f, 8.0f}));
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    step(world, 120);
    const JoltBodyState st = cruise_straight(car, world, 900); // 15 s, past the curb

    NF_CHECK(st.position.z > 20.0f); // made it over and kept going
    NF_CHECK(car.speed_ms() > 1.0f); // not stuck against the face
    NF_CHECK(chassis_up(st).y > 0.70f); // still rubber-side down-ish
}

NF_TEST(jolt_vehicle_handbrake_slows_the_car_without_spinning_it) {
    // Handbrake clamps only the rear pair at the hand-brake torque. It must
    // shed speed without swapping the car's ends when the wheel is held
    // straight — a 180 here reads as the handbrake being wired to all four
    // wheels or to the front axle.
    JoltWorld world;
    world.add_body(static_plane());
    JoltVehicle car(world, JoltVehicleConfig{}, Vec3{0, 2, 0});
    step(world, 120);
    const JoltBodyState fast = cruise_straight(car, world, 240);
    NF_CHECK(fast.position.z > 5.0f);

    for (int i = 0; i < 120; ++i) { // 2 s of handbrake, no steer, no throttle
        car.drive(0.0f, 0.0f, 0.0f, 1.0f);
        world.step(1.0f / 60.0f);
    }
    const JoltBodyState after = car.chassis_state();
    NF_CHECK(car.speed_ms() < 20.0f); // it shed speed rather than accelerating
    const Vec3 fwd = after.rotation.rotate(Vec3{0, 0, 1});
    NF_CHECK(fwd.z > 0.70f); // still pointing the way it was going
    NF_CHECK(chassis_up(after).y > 0.90f);
}

NF_TEST(jolt_vehicle_suspension_tune_sets_the_ride_height) {
    // FrequencyAndDamping makes the spring k = m * omega^2 per wheel, so a
    // softer car settles lower under the same chassis mass. This is the proof
    // that the config's suspension fields reach Jolt at all — a wiring break
    // (e.g. the spring being constructed from the wrong fields) leaves both
    // configs at the identical default height.
    auto rest_height = [](float hz) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicleConfig cfg;
        cfg.suspension_frequency_hz = hz;
        JoltVehicle car(world, cfg, Vec3{0, 2, 0});
        step(world, 240); // settle past the initial bounce
        return car.chassis_state().position.y;
    };
    const float soft = rest_height(0.8f);
    const float firm = rest_height(4.0f);
    NF_CHECK(soft < firm);
    NF_CHECK(soft > 0.2f); // the soft spring still holds the chassis off the floor
    NF_CHECK(firm < 2.0f); // and the firm one is not rigid
}

NF_TEST(jolt_vehicle_tire_grip_tune_changes_cornering) {
    // Guards the friction-curve wiring the same way the suspension test guards
    // the spring: if the config never reached the tires, both runs come out
    // bit-identical. Direction of the effect: scaling every tire's lateral
    // grip down scales the *front* axle's ability to pull the chassis into the
    // bend, so the slick car understeers — it corners less for the same input,
    // not more. (Measured: peak 1.3 turns the heading 113 deg, peak 0.3 only
    // 74 deg over the same 4 s of half-lock.)
    auto turn_radians = [](float peak_grip) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicleConfig cfg;
        cfg.tire_peak_friction = peak_grip;
        cfg.tire_limit_friction = peak_grip * 0.8f;
        JoltVehicle car(world, cfg, Vec3{0, 2, 0});
        step(world, 120);
        for (int i = 0; i < 240; ++i) {
            car.drive(1.0f, 0.5f);
            world.step(1.0f / 60.0f);
        }
        const Vec3 fwd = car.chassis_state().rotation.rotate(Vec3{0, 0, 1});
        // Rotation magnitude off +Z, in [0, pi]: both runs yaw the same way,
        // so this is monotone in how far the corner carried them.
        return std::acos(std::clamp(fwd.z, -1.0f, 1.0f));
    };
    const float grippy_turn = turn_radians(1.3f);
    const float slick_turn = turn_radians(0.3f);
    NF_CHECK(grippy_turn > 0.5f); // the grippy car actually corners
    NF_CHECK(slick_turn < grippy_turn); // the slick one understeers
}

NF_TEST(jolt_vehicle_demo_tune_drives_straight_and_corner_upright) {
    // The exact tune Samples/VehicleDemo ships: wider track, reduced steering
    // lock, firmer damped suspension, tires just past 1.0 grip. This is the
    // documented proof that the demo's car stays driveable — the demo itself
    // needs a human at the keyboard, so the stability claim lives here.
    JoltVehicleConfig cfg;
    cfg.track_half_width = 1.0f;
    cfg.max_steer_deg = 22.0f;
    cfg.suspension_frequency_hz = 2.0f;
    cfg.suspension_damping = 0.85f;
    cfg.tire_peak_friction = 1.3f;
    cfg.tire_limit_friction = 1.05f;

    // Full throttle, no steer: straight and upright.
    {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, cfg, Vec3{0, 2, 0});
        step(world, 120);
        const JoltBodyState st = cruise_straight(car, world, 900); // 15 s
        NF_CHECK(st.position.z > 20.0f);
        NF_CHECK(std::fabs(st.position.x) < 0.05f * st.position.z);
        NF_CHECK(chassis_up(st).y > 0.90f);
    }

    // Full lock at full throttle: it turns and slides, but does not roll.
    {
        JoltWorld world;
        world.add_body(static_plane());
        JoltVehicle car(world, cfg, Vec3{0, 2, 0});
        step(world, 120);
        for (int i = 0; i < 900; ++i) {
            car.drive(1.0f, 1.0f);
            world.step(1.0f / 60.0f);
        }
        const JoltBodyState st = car.chassis_state();
        const Vec3 fwd = st.rotation.rotate(Vec3{0, 0, 1});
        NF_CHECK(fwd.z < 0.90f); // the lock actually turned it
        NF_CHECK(chassis_up(st).y > 0.55f); // rubber-side down through the slide
    }
}

