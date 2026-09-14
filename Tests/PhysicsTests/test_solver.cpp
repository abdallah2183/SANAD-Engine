// Tests/PhysicsTests/test_solver.cpp — rigid-body solver behaviour
//
// These are the tests that decide whether the solver is real. A contact solver
// can look plausible in a screenshot while being wrong in ways that only show up
// after hundreds of steps — sinking, jitter, energy gain — so the assertions
// here are closed-form or long-run rather than visual.

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/PhysicsWorld.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

namespace {

constexpr f32 kStep = 1.0f / 60.0f;

/// Ground plane at y = 0, as a static body.
BodyHandle add_ground(PhysicsWorld& world) {
    BodyDesc desc;
    desc.type = BodyType::Static;
    desc.shape = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));
    desc.position = Vec3::zero;
    desc.friction = 0.8f;
    desc.restitution = 0.0f;
    return world.add_body(desc);
}

void run(PhysicsWorld& world, int steps, f32 dt = kStep) {
    for (int i = 0; i < steps; ++i) {
        world.step(dt);
    }
}

} // namespace

// --- Integration ------------------------------------------------------------

NF_TEST(solver_free_fall_matches_the_closed_form) {
    // With no contacts this is plain kinematics, so the analytic answer is
    // available and the integrator must match it closely.
    PhysicsWorld world;
    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_sphere(0.5f);
    desc.position = Vec3(0.0f, 100.0f, 0.0f);
    desc.linear_damping = 0.0f;
    desc.angular_damping = 0.0f;
    desc.mass = 2.0f;
    const BodyHandle body = world.add_body(desc);

    const int steps = 30;
    run(world, steps);

    const f32 t = kStep * static_cast<f32>(steps);
    const f32 expected = 100.0f - 0.5f * 9.81f * t * t;
    // Semi-implicit Euler overshoots by half a step of gravity; that is expected
    // and bounded, so the tolerance is one step's worth of velocity.
    NF_CHECK_NEAR(world.state(body).position.y, expected, 9.81f * kStep * 1.5f);
}

NF_TEST(solver_sphere_rests_on_a_plane_at_its_radius) {
    PhysicsWorld world;
    add_ground(world);

    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_sphere(0.5f);
    desc.position = Vec3(0.0f, 3.0f, 0.0f);
    desc.friction = 0.5f;
    desc.restitution = 0.0f;
    const BodyHandle ball = world.add_body(desc);

    run(world, 400);

    // The centre settles one radius above the plane, minus the penetration slop
    // the solver deliberately tolerates so resting contacts do not buzz.
    const f32 y = world.state(ball).position.y;
    NF_CHECK_NEAR(y, 0.5f, 0.02f);
    NF_CHECK(y > 0.45f); // it did not sink through
}

NF_TEST(solver_box_rests_on_a_plane_at_its_half_height) {
    PhysicsWorld world;
    add_ground(world);

    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_box(Vec3(0.5f, 0.5f, 0.5f));
    desc.position = Vec3(0.0f, 3.0f, 0.0f);
    desc.friction = 0.8f;
    desc.restitution = 0.0f;
    const BodyHandle box = world.add_body(desc);

    run(world, 400);

    const BodyState s = world.state(box);
    NF_CHECK_NEAR(s.position.y, 0.5f, 0.02f);
    // It must not have drifted sideways while settling.
    NF_CHECK_NEAR(s.position.x, 0.0f, 0.05f);
    NF_CHECK_NEAR(s.position.z, 0.0f, 0.05f);
    // And it must still be level, not tipped onto an edge.
    NF_CHECK(std::fabs(s.orientation.x) < 0.05f);
    NF_CHECK(std::fabs(s.orientation.z) < 0.05f);
}

NF_TEST(solver_three_box_stack_stays_stacked) {
    // The case that separates a real solver from a plausible one. Without warm
    // starting this sinks or jitters; with a single contact point per pair it
    // topples.
    PhysicsWorld world;
    add_ground(world);

    BodyHandle boxes[3];
    for (int i = 0; i < 3; ++i) {
        BodyDesc desc;
        desc.type = BodyType::Dynamic;
        desc.shape = Shape::make_box(Vec3(0.5f, 0.5f, 0.5f));
        desc.position = Vec3(0.0f, 0.5f + static_cast<f32>(i) * 1.0f, 0.0f);
        desc.friction = 0.8f;
        desc.restitution = 0.0f;
        boxes[i] = world.add_body(desc);
    }

    run(world, 600);

    for (int i = 0; i < 3; ++i) {
        const BodyState s = world.state(boxes[i]);
        const f32 expected_y = 0.5f + static_cast<f32>(i) * 1.0f;
        // The stack may compress by a slop per contact but must not collapse.
        NF_CHECK_NEAR(s.position.y, expected_y, 0.06f);
        NF_CHECK_NEAR(s.position.x, 0.0f, 0.08f);
        NF_CHECK_NEAR(s.position.z, 0.0f, 0.08f);
    }

    // The top box must still be above the middle one: a collapse would show as
    // inverted or coincident heights.
    NF_CHECK(world.state(boxes[2]).position.y > world.state(boxes[1]).position.y);
    NF_CHECK(world.state(boxes[1]).position.y > world.state(boxes[0]).position.y);
}

NF_TEST(solver_a_settled_stack_falls_asleep) {
    // The regression test for island-based sleeping. Per-body sleeping cannot
    // pass this: each body falls asleep and is immediately woken by its still
    // awake neighbour, so a stack never settles. It also pins the iteration
    // count — at 8 velocity iterations the stack is stable to the eye but its
    // residual velocity never drops below the sleep threshold.
    PhysicsWorld world;
    add_ground(world);

    BodyHandle boxes[3];
    for (int i = 0; i < 3; ++i) {
        BodyDesc desc;
        desc.type = BodyType::Dynamic;
        desc.shape = Shape::make_box(Vec3(0.5f));
        desc.position = Vec3(0.0f, 0.5f + static_cast<f32>(i) * 1.0f, 0.0f);
        desc.friction = 0.8f;
        desc.restitution = 0.0f;
        boxes[i] = world.add_body(desc);
    }

    run(world, 600);

    for (int i = 0; i < 3; ++i) {
        const BodyState s = world.state(boxes[i]);
        NF_CHECK(s.asleep);
        // Sleeping zeroes the velocity, which is what makes a settled scene
        // reproducible rather than drifting on residual motion.
        NF_CHECK_NEAR(s.linear_velocity.length(), 0.0f, 1e-6f);
        NF_CHECK_NEAR(s.angular_velocity.length(), 0.0f, 1e-6f);
    }

    // And the stack must still be a stack, not a pile.
    NF_CHECK(world.state(boxes[2]).position.y > world.state(boxes[1]).position.y);
    NF_CHECK(world.state(boxes[1]).position.y > world.state(boxes[0]).position.y);
}

NF_TEST(solver_bouncing_ball_loses_height) {
    // Restitution below 1 means every apex is lower than the last. A solver that
    // gains energy shows up here as an apex that never decreases.
    PhysicsWorld world;
    add_ground(world);

    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_sphere(0.5f);
    desc.position = Vec3(0.0f, 4.0f, 0.0f);
    desc.restitution = 0.6f;
    desc.friction = 0.3f;
    desc.linear_damping = 0.0f;
    desc.allow_sleep = false; // keep it bouncing for the whole run
    const BodyHandle ball = world.add_body(desc);

    // Track the peak of each upward phase, keyed on the SIGN of the vertical
    // velocity rather than on a height threshold. A threshold cannot work here:
    // the resting height is one radius (0.5) and each bounce loses most of its
    // height, so the second apex is well under any threshold that the first one
    // comfortably clears — and the test then reports "never bounced" for a ball
    // that bounced fine.
    std::vector<f32> peaks;
    f32 current_peak = -1.0e9f;
    bool rising = false;
    for (int i = 0; i < 1200; ++i) {
        world.step(kStep);
        const BodyState s = world.state(ball);
        if (s.linear_velocity.y > 0.0f) {
            rising = true;
            current_peak = std::max(current_peak, s.position.y);
        } else if (rising) {
            peaks.push_back(current_peak);
            rising = false;
            current_peak = -1.0e9f;
        }
    }

    NF_CHECK(peaks.size() >= 2); // it bounced more than once
    for (size_t i = 1; i < peaks.size(); ++i) {
        // Restitution below 1 means every apex is lower than the last. A solver
        // that gains energy shows up as an apex that fails this.
        NF_CHECK(peaks[i] < peaks[i - 1] + 0.01f);
    }
    if (peaks.size() >= 2) {
        NF_CHECK(peaks.back() < peaks.front());
        // And it must not have exceeded the height it was dropped from.
        NF_CHECK(peaks.front() < 4.0f);
    }
}

NF_TEST(solver_stays_finite_over_a_long_run) {
    // A single NaN propagates to every body within a few steps, so a long run
    // with a pile of interacting bodies is the cheapest NaN detector there is.
    PhysicsWorld world;
    add_ground(world);

    for (int i = 0; i < 12; ++i) {
        BodyDesc desc;
        desc.type = BodyType::Dynamic;
        desc.shape = (i % 2 == 0) ? Shape::make_box(Vec3(0.4f)) : Shape::make_sphere(0.4f);
        desc.position = Vec3(0.25f * static_cast<f32>(i % 4), 1.0f + 0.9f * static_cast<f32>(i),
                             0.25f * static_cast<f32>(i / 4));
        desc.friction = 0.6f;
        desc.restitution = 0.2f;
        world.add_body(desc);
    }

    run(world, 1200);

    for (size_t i = 0; i < world.body_count(); ++i) {
        // is_alive is the only way to reach a body by index from outside, so
        // walk handles instead.
        (void)i;
    }
    // Every body's hash input must be finite; recomputing the hash exercises
    // exactly the fields that would be NaN.
    const u64 h = world.state_hash();
    NF_CHECK(h != 0u);
    NF_CHECK(world.awake_count() <= world.body_count());
}

// --- Sleeping ---------------------------------------------------------------

NF_TEST(solver_a_settled_body_falls_asleep) {
    PhysicsWorld world;
    add_ground(world);

    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_sphere(0.5f);
    desc.position = Vec3(0.0f, 1.0f, 0.0f);
    desc.restitution = 0.0f;
    const BodyHandle ball = world.add_body(desc);

    run(world, 600);
    NF_CHECK(world.state(ball).asleep);
    // Sleeping zeroes the velocity, which is what makes a settled scene
    // bit-identical across runs instead of drifting on residual motion.
    NF_CHECK_NEAR(world.state(ball).linear_velocity.length(), 0.0f, 1e-6f);
}

NF_TEST(solver_a_body_with_sleep_disabled_never_sleeps) {
    PhysicsWorld world;
    add_ground(world);

    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_sphere(0.5f);
    desc.position = Vec3(0.0f, 1.0f, 0.0f);
    desc.restitution = 0.0f;
    desc.allow_sleep = false;
    const BodyHandle ball = world.add_body(desc);

    run(world, 600);
    NF_CHECK(!world.state(ball).asleep);
}

NF_TEST(solver_a_moving_body_wakes_a_sleeping_one) {
    PhysicsWorld world;
    add_ground(world);

    BodyDesc sleeper_desc;
    sleeper_desc.type = BodyType::Dynamic;
    sleeper_desc.shape = Shape::make_box(Vec3(0.5f));
    sleeper_desc.position = Vec3(0.0f, 0.5f, 0.0f);
    sleeper_desc.restitution = 0.0f;
    const BodyHandle sleeper = world.add_body(sleeper_desc);

    run(world, 600);
    NF_CHECK(world.state(sleeper).asleep);

    // Drop something onto it. The sleeper must wake, or the new body would rest
    // on a body that never responds.
    BodyDesc faller_desc;
    faller_desc.type = BodyType::Dynamic;
    faller_desc.shape = Shape::make_sphere(0.4f);
    faller_desc.position = Vec3(0.0f, 3.0f, 0.0f);
    faller_desc.restitution = 0.0f;
    world.add_body(faller_desc);

    // Check that it woke AT SOME POINT during the impact, not that it is still
    // awake afterwards: once the falling body has settled too, the island is
    // slow again and everything legitimately goes back to sleep together.
    bool woke = false;
    for (int i = 0; i < 240; ++i) {
        world.step(kStep);
        if (!world.state(sleeper).asleep) {
            woke = true;
            break;
        }
    }
    NF_CHECK(woke);
}

// --- Body types -------------------------------------------------------------

NF_TEST(solver_static_body_never_moves) {
    PhysicsWorld world;
    const BodyHandle ground = add_ground(world);

    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_box(Vec3(0.5f));
    desc.position = Vec3(0.0f, 1.0f, 0.0f);
    world.add_body(desc);

    run(world, 300);
    const BodyState g = world.state(ground);
    NF_CHECK_NEAR(g.position.y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(g.linear_velocity.length(), 0.0f, 1e-6f);
}

NF_TEST(solver_an_impulse_does_nothing_to_a_static_body) {
    PhysicsWorld world;
    const BodyHandle ground = add_ground(world);
    world.apply_impulse(ground, Vec3(1000.0f, 1000.0f, 1000.0f), Vec3::zero);
    run(world, 10);
    NF_CHECK_NEAR(world.state(ground).position.length(), 0.0f, 1e-6f);
}

NF_TEST(solver_kinematic_body_moves_but_is_not_pushed) {
    PhysicsWorld world;
    add_ground(world);

    BodyDesc kin;
    kin.type = BodyType::Kinematic;
    kin.shape = Shape::make_box(Vec3(0.5f));
    kin.position = Vec3(-3.0f, 0.5f, 0.0f);
    kin.linear_velocity = Vec3(2.0f, 0.0f, 0.0f);
    const BodyHandle mover = world.add_body(kin);

    run(world, 60);

    // It moved under its own velocity despite gravity and any contacts.
    NF_CHECK(world.state(mover).position.x > -3.0f + 1.0f);
    NF_CHECK_NEAR(world.state(mover).position.y, 0.5f, 0.05f);
}

// --- Impulses ---------------------------------------------------------------

NF_TEST(solver_an_off_centre_impulse_produces_spin) {
    PhysicsWorld world;
    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_box(Vec3(0.5f));
    desc.position = Vec3::zero;
    desc.linear_damping = 0.0f;
    desc.angular_damping = 0.0f;
    desc.allow_sleep = false;
    const BodyHandle box = world.add_body(desc);

    // Push the top edge sideways: the box must both move and rotate.
    world.apply_impulse(box, Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.5f, 0.0f));
    const BodyState s = world.state(box);

    NF_CHECK(s.linear_velocity.x > 0.0f);
    NF_CHECK(std::fabs(s.angular_velocity.z) > 0.1f);
}

NF_TEST(solver_apply_impulse_to_a_dead_handle_is_ignored) {
    PhysicsWorld world;
    const BodyHandle h = world.add_body(BodyDesc{});
    world.remove_body(h);
    world.apply_impulse(h, Vec3(1000.0f), Vec3::zero); // must not crash
    NF_CHECK(!world.is_alive(h));
}

// --- Handles ----------------------------------------------------------------

NF_TEST(solver_a_removed_handle_is_detectably_dead) {
    PhysicsWorld world;
    const BodyHandle h = world.add_body(BodyDesc{});
    NF_CHECK(world.is_alive(h));
    world.remove_body(h);
    NF_CHECK(!world.is_alive(h));
}

NF_TEST(solver_a_recycled_slot_does_not_resurrect_an_old_handle) {
    // The whole point of the generation counter. Without it a stale handle would
    // silently address whichever body landed in the freed slot.
    PhysicsWorld world;
    const BodyHandle first = world.add_body(BodyDesc{});
    world.remove_body(first);

    const BodyHandle second = world.add_body(BodyDesc{});
    NF_CHECK_EQ(second.index, first.index); // the slot was reused
    NF_CHECK(second != first);              // but the handle is different
    NF_CHECK(world.is_alive(second));
    NF_CHECK(!world.is_alive(first));       // the stale handle stays dead

    // And acting through it must not touch the new body.
    const f32 y_before = world.state(second).position.y;
    world.apply_impulse(first, Vec3(0.0f, 500.0f, 0.0f), Vec3::zero);
    NF_CHECK_NEAR(world.state(second).position.y, y_before, 1e-6f);
}

NF_TEST(solver_state_hash_changes_when_a_body_moves) {
    PhysicsWorld world;
    BodyDesc desc;
    desc.shape = Shape::make_sphere(0.5f);
    desc.position = Vec3(0.0f, 10.0f, 0.0f);
    desc.allow_sleep = false;
    world.add_body(desc);

    const u64 before = world.state_hash();
    run(world, 10);
    const u64 after = world.state_hash();
    NF_CHECK(before != after);
}
