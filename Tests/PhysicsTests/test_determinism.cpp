// Tests/PhysicsTests/test_determinism.cpp — the fixed clock and reproducibility
//
// This is the test the whole phase rests on. A physics engine that looks right
// but produces a different result depending on frame rate cannot be replayed,
// cannot be networked, and cannot be debugged from a bug report. The property
// under test is not "the simulation is stable" — it is "the same inputs produce
// the same bits, no matter how the elapsed time was chopped up".

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/FixedTimestep.hpp>
#include <NF/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace nf;
using namespace nf::physics;

namespace {

/// 1/64 is exact in binary. Using 1/60 would accumulate rounding in the
/// accumulator and the "different frame patterns" test would fail for a reason
/// that has nothing to do with the engine.
constexpr f32 kStep = 1.0f / 64.0f;

/// A scene with a stack, a falling sphere and a bounce: enough interacting
/// bodies that any ordering or timing dependence shows up.
void build_scene(PhysicsWorld& world) {
    BodyDesc ground;
    ground.type = BodyType::Static;
    ground.shape = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));
    ground.friction = 0.8f;
    ground.restitution = 0.0f;
    world.add_body(ground);

    for (int i = 0; i < 3; ++i) {
        BodyDesc box;
        box.type = BodyType::Dynamic;
        box.shape = Shape::make_box(Vec3(0.5f));
        box.position = Vec3(0.0f, 0.5f + static_cast<f32>(i), 0.0f);
        box.friction = 0.8f;
        box.restitution = 0.0f;
        world.add_body(box);
    }

    BodyDesc ball;
    ball.type = BodyType::Dynamic;
    ball.shape = Shape::make_sphere(0.35f);
    ball.position = Vec3(1.7f, 5.0f, 0.4f);
    ball.restitution = 0.5f;
    ball.friction = 0.3f;
    world.add_body(ball);

    BodyDesc slide;
    slide.type = BodyType::Dynamic;
    slide.shape = Shape::make_box(Vec3(0.3f));
    slide.position = Vec3(-2.0f, 0.31f, 0.0f);
    slide.linear_velocity = Vec3(3.0f, 0.0f, 0.0f);
    slide.friction = 0.4f;
    world.add_body(slide);
}

/// Run `world` for one second of simulated time, driven by the given frame
/// durations. Returns the number of fixed steps that were actually taken.
u32 run_with_frame_pattern(PhysicsWorld& world, const std::vector<f32>& frames) {
    FixedTimestep clock(kStep, 64);
    u32 steps = 0;
    for (f32 frame : frames) {
        const u32 n = clock.advance(frame);
        for (u32 i = 0; i < n; ++i) {
            world.step(kStep);
            ++steps;
        }
    }
    return steps;
}

u64 hash_after_one_second(const std::vector<f32>& frames, u32* out_steps = nullptr) {
    PhysicsWorld world;
    build_scene(world);
    const u32 steps = run_with_frame_pattern(world, frames);
    if (out_steps != nullptr) {
        *out_steps = steps;
    }
    return world.state_hash();
}

} // namespace

// --- FixedTimestep ----------------------------------------------------------

NF_TEST(fixed_timestep_accumulates_whole_steps) {
    FixedTimestep clock(0.1f, 16);
    // 0.25 s at a 0.1 s step is two steps with 0.05 carried.
    NF_CHECK_EQ(clock.advance(0.25f), 2u);
    NF_CHECK_NEAR(clock.alpha(), 0.5f, 1e-4f);
    // The carried remainder completes the next step.
    NF_CHECK_EQ(clock.advance(0.05f), 1u);
    NF_CHECK_NEAR(clock.alpha(), 0.0f, 1e-4f);
}

NF_TEST(fixed_timestep_carries_the_remainder_across_frames) {
    FixedTimestep clock(0.1f, 16);
    u32 total = 0;
    for (int i = 0; i < 10; ++i) {
        total += clock.advance(0.05f); // 10 * 0.05 = 0.5 s
    }
    NF_CHECK_EQ(total, 5u);
}

NF_TEST(fixed_timestep_caps_substeps_and_counts_the_dropped_time) {
    // A 5-second stall (a breakpoint, a window drag) must not queue up 300
    // steps: the simulation would visibly fast-forward, and on a real scene it
    // would stall for seconds trying to catch up.
    FixedTimestep clock(0.01f, 8);
    const u32 steps = clock.advance(5.0f);
    NF_CHECK_EQ(steps, 8u);
    NF_CHECK(clock.dropped_steps() > 0u);
    // And the surplus is gone, not still pending.
    NF_CHECK_EQ(clock.advance(0.0f), 0u);
}

NF_TEST(fixed_timestep_rejects_nonsense_deltas) {
    FixedTimestep clock(0.01f, 8);
    NF_CHECK_EQ(clock.advance(0.0f), 0u);
    NF_CHECK_EQ(clock.advance(-1.0f), 0u);
    // A NaN delta would otherwise poison the accumulator and every step after it.
    NF_CHECK_EQ(clock.advance(std::numeric_limits<f32>::quiet_NaN()), 0u);
    NF_CHECK_NEAR(clock.alpha(), 0.0f, 1e-6f);
}

NF_TEST(fixed_timestep_reset_clears_the_accumulator) {
    FixedTimestep clock(0.1f, 16);
    clock.advance(0.25f);
    clock.reset();
    NF_CHECK_NEAR(clock.alpha(), 0.0f, 1e-6f);
    NF_CHECK_EQ(clock.dropped_steps(), 0u);
}

// --- Determinism ------------------------------------------------------------

NF_TEST(determinism_the_same_pattern_reproduces_the_same_hash) {
    const std::vector<f32> frames(64, kStep);
    NF_CHECK_EQ(hash_after_one_second(frames), hash_after_one_second(frames));
}

NF_TEST(determinism_one_second_in_64_frames_equals_one_second_in_8) {
    // The property the whole phase is for: chopping the same elapsed time into
    // different frame sizes must not change the simulation.
    const std::vector<f32> fine(64, kStep);
    const std::vector<f32> coarse(8, kStep * 8.0f);

    u32 fine_steps = 0;
    u32 coarse_steps = 0;
    const u64 fine_hash = hash_after_one_second(fine, &fine_steps);
    const u64 coarse_hash = hash_after_one_second(coarse, &coarse_steps);

    NF_CHECK_EQ(fine_steps, 64u);
    NF_CHECK_EQ(coarse_steps, 64u);
    NF_CHECK_EQ(fine_hash, coarse_hash);
}

NF_TEST(determinism_an_irregular_frame_pattern_matches_a_regular_one) {
    // Real frame times are never uniform. This is the pattern a stuttery frame
    // produces: several fast frames, one slow one, repeated.
    const std::vector<f32> regular(64, kStep);

    // Built by accumulating DURATIONS until exactly one second has elapsed.
    // Counting frames and multiplying by the step is wrong — the frames do not
    // all have the same length — and that mistake makes the two runs take a
    // different number of steps for a reason that has nothing to do with the
    // engine.
    //
    // Every duration here is a multiple of kStep/2, which is a power of two, so
    // the accumulator arithmetic is exact and the total lands on 1.0 exactly.
    std::vector<f32> irregular;
    f32 elapsed = 0.0f;
    int index = 0;
    while (elapsed < 1.0f) {
        f32 next = (index % 3 == 0) ? kStep * 3.0f : kStep * 0.5f;
        if (elapsed + next > 1.0f) {
            next = 1.0f - elapsed; // trim the last frame to land exactly on 1.0
        }
        irregular.push_back(next);
        elapsed += next;
        ++index;
    }
    NF_CHECK_NEAR(elapsed, 1.0f, 1e-6f);

    u32 regular_steps = 0;
    u32 irregular_steps = 0;
    const u64 regular_hash = hash_after_one_second(regular, &regular_steps);
    const u64 irregular_hash = hash_after_one_second(irregular, &irregular_steps);

    // The step count must match; the hash must too. Equal step counts with
    // different hashes would mean the step itself reads something time-related.
    NF_CHECK_EQ(regular_steps, irregular_steps);
    NF_CHECK_EQ(regular_hash, irregular_hash);
}

NF_TEST(determinism_two_worlds_built_identically_agree) {
    PhysicsWorld a;
    PhysicsWorld b;
    build_scene(a);
    build_scene(b);
    for (int i = 0; i < 300; ++i) {
        a.step(kStep);
        b.step(kStep);
    }
    NF_CHECK_EQ(a.state_hash(), b.state_hash());
}

NF_TEST(determinism_the_hash_is_sensitive_to_a_single_step) {
    // A hash that does not change proves nothing. This is the control: one extra
    // step must change it.
    PhysicsWorld world;
    build_scene(world);
    for (int i = 0; i < 100; ++i) world.step(kStep);
    const u64 before = world.state_hash();
    world.step(kStep);
    NF_CHECK(before != world.state_hash());
}

NF_TEST(determinism_the_hash_is_sensitive_to_the_gravity_setting) {
    PhysicsWorld a;
    PhysicsWorld b;
    build_scene(a);
    build_scene(b);
    b.settings().gravity = Vec3(0.0f, -9.8f, 0.0f); // a hair different
    for (int i = 0; i < 200; ++i) {
        a.step(kStep);
        b.step(kStep);
    }
    NF_CHECK(a.state_hash() != b.state_hash());
}

NF_TEST(determinism_a_removed_body_changes_the_hash) {
    PhysicsWorld a;
    build_scene(a);
    PhysicsWorld b;
    build_scene(b);

    // Remove the sliding box from b. The remaining bodies must diverge, which
    // also proves removal actually takes the body out of the simulation rather
    // than only marking it dead.
    b.remove_body(BodyHandle{4, 1}); // the fifth body added by build_scene

    for (int i = 0; i < 200; ++i) {
        a.step(kStep);
        b.step(kStep);
    }
    NF_CHECK(a.state_hash() != b.state_hash());
}

NF_TEST(determinism_holds_across_a_long_run) {
    // Divergence from a non-deterministic detail is usually slow: it shows as
    // two runs drifting apart rather than as an immediate difference. A short
    // run would not catch it.
    const std::vector<f32> frames(64 * 10, kStep);
    NF_CHECK_EQ(hash_after_one_second(frames), hash_after_one_second(frames));
}

NF_TEST(determinism_the_simulation_produces_finite_values) {
    // A NaN would make state_hash meaningless: every comparison against NaN is
    // false, so two runs full of NaN would compare unequal for the wrong reason.
    PhysicsWorld world;
    build_scene(world);
    for (int i = 0; i < 1200; ++i) {
        world.step(kStep);
    }
    // Re-hashing twice must agree, which it cannot if any field is NaN.
    NF_CHECK_EQ(world.state_hash(), world.state_hash());

    for (size_t i = 0; i < world.body_count(); ++i) {
        const BodyState s = world.state(BodyHandle{static_cast<u32>(i), 1});
        NF_CHECK(std::isfinite(s.position.x) && std::isfinite(s.position.y) &&
                 std::isfinite(s.position.z));
        NF_CHECK(std::isfinite(s.linear_velocity.x) && std::isfinite(s.linear_velocity.y) &&
                 std::isfinite(s.linear_velocity.z));
    }
}
