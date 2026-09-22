// PhysicsTests — Jolt-backed character controller (design §39): step offset,
// slopes, moving platforms, crouch, climb hook, lifetime.
//
// Every case here is a tiny headless world driven with a fixed dt: no
// rendering, no threads, deterministic — same spirit as the vehicle suite.
// The character is a CharacterVirtual (a kinematic-style volume), so each
// case ticks it through JoltCharacter/JoltWorld::character_move and lets the
// world step for everything else.

#include <NF/Physics/JoltCharacter.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Test/TestFramework.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace nf;
using namespace nf::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;

BodyDesc static_plane() {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_plane(Vec3{0, 1, 0});
    return d;
}

BodyDesc static_box(Vec3 position, Vec3 half_extents) {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_box(half_extents);
    d.position = position;
    return d;
}

/// A 30-degree ramp whose top surface starts at the origin and climbs with +z.
/// The centre is derived, not eyeballed: an L-long, h-thick slab rotated about
/// +X by -angle is placed so its lower top edge lands exactly on y = 0 at
/// z = 0. The angle is negative because from_axis_angle is right-handed about
/// +X (at +90 degrees it maps +Y to +Z), so a positive angle tips the far end
/// *down*.
BodyDesc static_ramp(float angle_deg, float length = 8.0f, float thickness = 0.1f) {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_box(Vec3{4.0f, thickness, length});
    const float a = to_radians(angle_deg);
    d.position = Vec3{0.0f, length * std::sin(a) - thickness * std::cos(a),
                      thickness * std::sin(a) + length * std::cos(a)};
    d.orientation = Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, -a);
    return d;
}

/// The highest point a jump starting from `rest_y` reaches, so the jump tests
/// can compare arcs instead of trusting one absolute number. The window has to
/// cover the whole flight or the measurement clips the apex instead of the
/// physics setting it: 240 ticks is 4 s, i.e. an apex reachable only by a
/// 19.6 m/s launch, well past anything these tests configure. Holding the
/// request past landing does not re-trigger a jump (verified: a 1-tick hold and
/// a 400-tick hold reach the same peak), so the window cannot inflate itself.
float jump_peak(JoltWorld& world, JoltCharacter& ch, float rest_y) {
    float best = rest_y;
    for (int i = 0; i < 240; ++i) { // hold the request while leaving the ground
        ch.move(Vec3{0, 0, 0}, true, kDt);
        world.step(kDt);
        best = std::max(best, ch.position().y);
    }
    return best;
}

void step(JoltWorld& world, int n, float dt = kDt) {
    for (int i = 0; i < n; ++i) world.step(dt);
}

/// One gameplay tick, exactly as a game would run it: move the character, then
/// step the world for everything else.
void drive(JoltWorld& world, JoltCharacter& character, Vec3 wish, bool jump, int n) {
    for (int i = 0; i < n; ++i) {
        character.move(wish, jump, kDt);
        world.step(kDt);
    }
}

} // namespace

NF_TEST(jolt_character_spawns_and_settles_on_ground) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 1.5f, 0});
    NF_CHECK(character.valid());
    NF_CHECK(world.body_count() == 1); // kinematic-style: not a body

    drive(world, character, Vec3{0, 0, 0}, false, 120);

    NF_CHECK(character.is_grounded());
    // position() is the character's FEET, so resting on a floor at y = 0 is
    // "near the floor", not "one capsule height above it".
    NF_CHECK_NEAR(character.position().y, 0.0f, 0.1f);
    // Gravity must not accumulate while supported (it would read as an
    // ever-growing falling speed; one tick of gravity is the settled value).
    NF_CHECK(std::abs(character.velocity().y) < 0.25f);
    NF_CHECK_NEAR(character.position().x, 0.0f, 0.05f);
    NF_CHECK_NEAR(character.position().z, 0.0f, 0.05f);
}

NF_TEST(jolt_character_walks_forward) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});

    drive(world, character, Vec3{0, 0, 1}, false, 180);

    const Vec3 p = character.position();
    NF_CHECK(p.z > 2.0f);      // it actually walked (3s at the 6 m/s cruise)
    NF_CHECK(p.z < 20.0f);     // ...and not further than the speed allows
    NF_CHECK(character.is_grounded());
    NF_CHECK(p.y > -0.1f);     // did not sink into the floor
    NF_CHECK(p.y < 0.2f);      // and did not fly
    NF_CHECK_NEAR(p.x, 0.0f, 0.1f); // straight line, no sideways drift
}

NF_TEST(jolt_character_blocked_by_wall) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    // Wall face at z = 2.75, far taller than the 0.4m step offset.
    NF_CHECK(world.add_body(static_box(Vec3{0, 1.5f, 3.0f}, Vec3{4.0f, 1.5f, 0.25f})).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});

    drive(world, character, Vec3{0, 0, 1}, false, 180);

    const Vec3 p = character.position();
    NF_CHECK(p.z > 1.0f); // it walked up to the wall
    NF_CHECK(p.z < 2.9f); // and stopped at it instead of passing through
    NF_CHECK(p.y < 0.2f); // never climbed it either
    NF_CHECK(character.is_grounded());
}

NF_TEST(jolt_character_steps_over_low_obstacle) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    // 0.3m tall slab spanning z in [2.5, 3.5]: below the default 0.4m step
    // offset, so WalkStairs must carry the character over it.
    NF_CHECK(world.add_body(static_box(Vec3{0, 0.15f, 3.0f}, Vec3{4.0f, 0.15f, 0.5f})).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});

    float max_y = 0.0f;
    for (int i = 0; i < 150; ++i) {
        character.move(Vec3{0, 0, 1}, false, kDt);
        world.step(kDt);
        max_y = std::max(max_y, character.position().y);
    }

    NF_CHECK(max_y > 0.2f);              // it stood on top of the obstacle
    NF_CHECK(character.position().z > 4.5f); // and continued past it
    NF_CHECK(std::abs(character.position().y) < 0.15f); // back at floor level
    NF_CHECK(character.is_grounded());
}

NF_TEST(jolt_character_cannot_climb_steep_slope) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    // 1.5m tall, vertical face: far above the step offset and far steeper than
    // the 50 degree slope limit.
    NF_CHECK(world.add_body(static_box(Vec3{0, 0.75f, 3.0f}, Vec3{4.0f, 0.75f, 0.5f})).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});

    drive(world, character, Vec3{0, 0, 1}, false, 180);

    const Vec3 p = character.position();
    NF_CHECK(p.y < 0.3f);  // pressing into it never raised the character
    NF_CHECK(p.z < 2.9f);  // held at the face (front face at z = 2.5)
    NF_CHECK(character.is_grounded());
}

NF_TEST(jolt_character_jumps_and_lands) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
    drive(world, character, Vec3{0, 0, 0}, false, 60); // settle
    NF_CHECK(character.is_grounded());
    const float rest_y = character.position().y;

    float peak = rest_y;
    for (int i = 0; i < 20; ++i) { // jump is a request: hold it while leaving the ground
        character.move(Vec3{0, 0, 0}, true, kDt);
        world.step(kDt);
        peak = std::max(peak, character.position().y);
    }
    NF_CHECK(peak - rest_y > 0.3f); // left the ground by a sane amount
    NF_CHECK(peak - rest_y < 5.0f); // ...without being launched to orbit

    drive(world, character, Vec3{0, 0, 0}, false, 150); // gravity brings it back
    NF_CHECK(character.is_grounded());
    NF_CHECK_NEAR(character.position().y, rest_y, 0.1f);
}

NF_TEST(jolt_character_crouch_changes_height) {
    // Crouch state is readable directly; its effect on the capsule is what the
    // doorway below measures: the standing capsule is 2*(0.55+0.35) = 1.8m
    // tall, the crouched one 2*(0.30+0.35) = 1.30m, and the gap under the slab
    // is 1.5m — so only the crouched character fits through.
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
    NF_CHECK(!character.is_crouched());
    character.set_crouch(true);
    NF_CHECK(character.is_crouched());
    character.set_crouch(false);
    NF_CHECK(!character.is_crouched());

    auto run = [](bool crouched) {
        JoltWorld w;
        NF_CHECK(w.add_body(static_plane()).valid());
        // Slab with its underside at y = 1.5, spanning z in [3.5, 4.5].
        NF_CHECK(w.add_body(static_box(Vec3{0, 1.75f, 4.0f}, Vec3{4.0f, 0.25f, 0.5f})).valid());
        JoltCharacter ch(w, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
        ch.set_crouch(crouched);
        drive(w, ch, Vec3{0, 0, 1}, false, 180);
        return ch.position();
    };
    const Vec3 standing = run(false);
    const Vec3 crouching = run(true);
    NF_CHECK(standing.z < 4.0f);  // blocked by the low ceiling
    NF_CHECK(crouching.z > 5.0f); // ducked under it and kept walking
}

NF_TEST(jolt_character_rides_moving_platform) {
    // The wish is zero for the whole run: any horizontal advance is the
    // platform carrying the character (§39's moving-platform requirement).
    struct Result {
        Vec3 character;
        Vec3 platform;
    };
    auto run = [](bool moving) -> Result {
        JoltWorld world;
        NF_CHECK(world.add_body(static_plane()).valid());
        // Dynamic box driven by set_linear_velocity: JoltWorld maps
        // BodyType::Kinematic onto a static Jolt body, so a velocity-driven
        // dynamic platform is how a moving platform is expressed here.
        BodyDesc platform;
        platform.type = BodyType::Dynamic;
        platform.shape = Shape::make_box(Vec3{2.0f, 0.2f, 2.0f});
        platform.position = Vec3{0, 0.2f, 0};
        platform.friction = 0.1f;
        platform.allow_sleep = false; // a sleeping platform would stop dead
        // Heavy on purpose: the character pushes bodies with a bounded force,
        // and a light platform would be punted out from under it (losing the
        // ground contact that carries the character).
        platform.mass = 500.0f;
        const JoltBody plat = world.add_body(platform);
        JoltCharacter ch(world, JoltCharacterConfig{}, Vec3{0, 0.5f, 0});
        drive(world, ch, Vec3{0, 0, 0}, false, 30); // settle on the platform
        for (int i = 0; i < 120; ++i) { // 2s of platform travel
            if (moving) world.set_linear_velocity(plat, Vec3{0, 0, 2.0f});
            ch.move(Vec3{0, 0, 0}, false, kDt);
            world.step(kDt);
        }
        return Result{ch.position(), world.state(plat).position};
    };
    const Result still = run(false);
    const Result moved = run(true);
    NF_CHECK(std::abs(still.character.z) < 0.2f); // no platform motion, no motion
    NF_CHECK(moved.platform.z > 3.0f);            // the platform really moved
    NF_CHECK(moved.character.z > 3.0f);           // ...and the character rode it
    NF_CHECK_NEAR(moved.character.z, moved.platform.z, 0.35f);
}

NF_TEST(jolt_character_climb_hook_moves_vertically) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
    drive(world, character, Vec3{0, 0, 0}, false, 60);
    const float rest_y = character.position().y;

    // Ladder: the game enables the hook and asks for "up" through wish_dir.y.
    character.set_climbing(true);
    drive(world, character, Vec3{0, 1, 0}, false, 60); // 1s at the cruise speed
    NF_CHECK(character.position().y > rest_y + 1.0f);
    // Zero wish while climbing holds position (no gravity while the hook is on).
    const float held_y = character.position().y;
    drive(world, character, Vec3{0, 0, 0}, false, 30);
    NF_CHECK_NEAR(character.position().y, held_y, 0.05f);

    // Letting go drops it back to the floor: gravity is back on.
    character.set_climbing(false);
    drive(world, character, Vec3{0, 0, 0}, false, 180);
    NF_CHECK(character.is_grounded());
    NF_CHECK_NEAR(character.position().y, rest_y, 0.1f);
}

NF_TEST(jolt_character_is_deterministic_for_identical_inputs) {
    // §39's network-prediction hook, made assertable: replaying the same input
    // sequence from the same spawn must produce the same trajectory, so a
    // client's prediction and the server's authoritative replay agree.
    auto run = [] {
        JoltWorld world;
        world.add_body(static_plane());
        world.add_body(static_box(Vec3{0, 0.15f, 3.0f}, Vec3{4.0f, 0.15f, 0.5f}));
        JoltCharacter ch(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
        for (int i = 0; i < 240; ++i) {
            const float t = static_cast<float>(i);
            // Varying input: a curve across the obstacle, with a jump every 90th tick.
            ch.move(Vec3{std::sin(t * 0.05f), 0.0f, 1.0f}, (i % 90) == 0, kDt);
            world.step(kDt);
        }
        return ch.position();
    };
    const Vec3 a = run();
    const Vec3 b = run();
    NF_CHECK_NEAR(a.x, b.x, 1e-4f);
    NF_CHECK_NEAR(a.y, b.y, 1e-4f);
    NF_CHECK_NEAR(a.z, b.z, 1e-4f);
}

NF_TEST(jolt_character_invalid_handles_are_safe) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());

    const JoltWorld::CharacterHandle dead;
    NF_CHECK(!dead.valid());
    world.character_move(dead, Vec3{0, 0, 1}, true, kDt); // no-ops, never crash
    world.character_set_climbing(dead, true);
    world.character_set_crouch(dead, true);
    world.character_destroy(dead);
    NF_CHECK(!world.character_is_crouched(dead));
    NF_CHECK(!world.character_is_grounded(dead));
    NF_CHECK(world.character_position(dead).length() == 0.0f);
    NF_CHECK(world.character_velocity(dead).length() == 0.0f);

    // A destroyed handle behaves exactly like an invalid one.
    const JoltWorld::CharacterHandle handle =
        world.character_create(JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
    NF_CHECK(handle.valid());
    world.character_destroy(handle);
    NF_CHECK(!world.character_is_grounded(handle));
    NF_CHECK(world.character_position(handle).length() == 0.0f);
    world.character_move(handle, Vec3{0, 0, 1}, false, kDt);
    world.character_destroy(handle); // double destroy is a no-op too
    step(world, 5);
    NF_CHECK(true);
}

NF_TEST(jolt_character_destroy_mid_simulation) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    {
        JoltCharacter character(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
        NF_CHECK(character.valid());
        NF_CHECK(world.body_count() == 1); // never a body, alive or dead
        drive(world, character, Vec3{0, 0, 1}, false, 60);
        NF_CHECK(character.position().z > 1.0f);
    } // destructor tears the character down mid-simulation

    step(world, 120); // the world keeps stepping without it
    BodyDesc ball;
    ball.shape = Shape::make_sphere(0.5f);
    ball.position = Vec3{0, 5, 0};
    const JoltBody b = world.add_body(ball);
    step(world, 180);
    NF_CHECK(world.state(b).position.y > 0.1f);
    NF_CHECK(world.state(b).position.y < 1.5f);
    NF_CHECK(world.body_count() == 2); // plane + ball; the character was no body

    // A fresh character still works in the same world (no stale state left).
    JoltCharacter again(world, JoltCharacterConfig{}, Vec3{3, 0.5f, 0});
    NF_CHECK(again.valid());
    drive(world, again, Vec3{0, 0, 0}, false, 120);
    NF_CHECK(again.is_grounded());
    NF_CHECK(world.body_count() == 2);
}

// ---------------------------------------------------------------------------
// Config wiring. Every knob in JoltCharacterConfig is plumbed through to Jolt
// in character_create / character_move, and these cases are what breaks if any
// one of them is dropped or clamped to a constant. The pattern is the one the
// vehicle suite uses: change the value, assert the *behaviour* changes with it,
// so a severed wire cannot hide behind a default that happens to work.
// ---------------------------------------------------------------------------

NF_TEST(jolt_character_step_offset_decides_what_is_walkable) {
    // Same 0.3m slab as steps_over_low_obstacle. A step offset above it clears
    // the slab; one far below treats it as a wall and stops at it.
    auto run = [](float step_offset) {
        JoltWorld world;
        world.add_body(static_plane());
        world.add_body(static_box(Vec3{0, 0.15f, 3.0f}, Vec3{4.0f, 0.15f, 0.5f}));
        JoltCharacterConfig cfg;
        cfg.step_offset = step_offset;
        JoltCharacter ch(world, cfg, Vec3{0, 0.1f, 0});
        float max_y = 0.0f;
        for (int i = 0; i < 150; ++i) {
            ch.move(Vec3{0, 0, 1}, false, kDt);
            world.step(kDt);
            max_y = std::max(max_y, ch.position().y);
        }
        return std::pair{ch.position().z, max_y};
    };
    const auto clears = run(0.4f); // the default
    const auto blocked = run(0.05f);
    NF_CHECK(clears.first > 4.5f);          // over the slab and away
    NF_CHECK(clears.second > 0.2f);         // stood on top of it
    NF_CHECK(blocked.first < 2.9f);         // held at the front face
    NF_CHECK(blocked.second < 0.1f);        // and never climbed it
    NF_CHECK(clears.first > blocked.first + 2.0f);
}

NF_TEST(jolt_character_slope_limit_decides_what_is_ground) {
    // A 30-degree ramp: walkable when the limit is above 30, a wall when it is
    // below. The distinction is exactly the OnGround / OnSteepGround split that
    // the jump and ground-velocity branches depend on.
    auto run = [](float max_slope_deg) {
        JoltWorld world;
        world.add_body(static_plane());
        world.add_body(static_ramp(30.0f));
        JoltCharacterConfig cfg;
        cfg.max_slope_deg = max_slope_deg;
        JoltCharacter ch(world, cfg, Vec3{0, 0.1f, -3.0f});
        float max_y = 0.0f;
        for (int i = 0; i < 240; ++i) {
            ch.move(Vec3{0, 0, 1}, false, kDt);
            world.step(kDt);
            max_y = std::max(max_y, ch.position().y);
        }
        return std::pair{ch.position().z, max_y};
    };
    const auto climbs = run(60.0f);
    const auto stops = run(10.0f);
    NF_CHECK(climbs.first > 4.0f);   // went up the ramp
    NF_CHECK(climbs.second > 2.0f);  // ...and gained real height
    NF_CHECK(stops.first < 1.5f);    // the ramp was a wall to it
    NF_CHECK(stops.second < 0.6f);
    NF_CHECK(climbs.second > stops.second + 1.0f);
}

NF_TEST(jolt_character_jump_speed_sets_the_peak) {
    // v^2 / (2g) is the ballistic ceiling, so a 3x jump speed is a 9x height —
    // the bound is derived, not a magic number picked from a run.
    auto height = [](float jump_speed) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltCharacterConfig cfg;
        cfg.jump_speed = jump_speed;
        JoltCharacter ch(world, cfg, Vec3{0, 0.1f, 0});
        drive(world, ch, Vec3{0, 0, 0}, false, 60);
        const float rest_y = ch.position().y;
        return jump_peak(world, ch, rest_y) - rest_y;
    };
    const float small = height(5.0f);
    const float big = height(15.0f);
    NF_CHECK(small > 0.2f); // both actually left the ground
    NF_CHECK(big > small + 1.0f);
    // The ceiling is analytic; a 60 Hz discrete step overshoots it by up to
    // one frame of launch velocity (v*dt), so the bound carries that slack.
    // A re-boosting jump (rocket) would clear it by metres, not centimetres.
    NF_CHECK(small < 5.0f * 5.0f / (2.0f * 9.81f) + 5.0f * kDt);
    NF_CHECK(big < 15.0f * 15.0f / (2.0f * 9.81f) + 15.0f * kDt);
    // The signature of a speed-proportional launch: height goes as v^2, so a
    // 3x speed is a 9x height. A clamped or constant jump_speed collapses this
    // ratio towards 1 no matter what the absolute heights happen to be.
    NF_CHECK_NEAR(big / small, 9.0f, 0.5f);
}

NF_TEST(jolt_character_gravity_scale_shapes_the_arc) {
    // Quarter gravity is a four-times-higher arc for the same jump: the
    // multiplier is what makes this a wiring test rather than a second jump
    // test, and the long tick count is because the slower arc takes ~4x longer
    // to peak.
    auto height = [](float gravity_scale) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltCharacterConfig cfg;
        cfg.gravity_scale = gravity_scale;
        JoltCharacter ch(world, cfg, Vec3{0, 0.1f, 0});
        drive(world, ch, Vec3{0, 0, 0}, false, 60);
        const float rest_y = ch.position().y;
        float best = rest_y;
        for (int i = 0; i < 600; ++i) {
            ch.move(Vec3{0, 0, 0}, (i % 200) == 0, kDt);
            world.step(kDt);
            best = std::max(best, ch.position().y);
        }
        return best - rest_y;
    };
    const float earth = height(1.0f);
    const float moon = height(0.25f);
    NF_CHECK(earth > 0.2f);
    NF_CHECK(moon > earth + 1.0f);
    NF_CHECK_NEAR(moon, earth * 4.0f, 0.5f);
}

NF_TEST(jolt_character_air_control_steers_a_fall) {
    // Spawned airborne with the wish held: the whole fall is the airborne
    // phase, so air control is the only thing that can move it downrange. The
    // sample is taken on the last airborne tick, because once it lands the
    // grounded accelerator takes over and measures something else entirely.
    auto drift = [](float air_control) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltCharacterConfig cfg;
        cfg.air_control = air_control;
        JoltCharacter ch(world, cfg, Vec3{0, 5.0f, 0});
        float last_airborne_z = 0.0f;
        for (int i = 0; i < 300; ++i) {
            ch.move(Vec3{0, 0, 1}, false, kDt);
            world.step(kDt);
            if (ch.is_grounded()) break;
            last_airborne_z = ch.position().z;
        }
        return last_airborne_z;
    };
    const float none = drift(0.0f);
    const float full = drift(1.0f);
    NF_CHECK(none < 0.2f);      // no air control: a dead drop, no downrange
    NF_CHECK(full > 2.0f);      // full air control: it steered while falling
    NF_CHECK(full > none + 1.0f);
}

NF_TEST(jolt_character_crouch_height_decides_what_fits_under) {
    // The slab's underside is at y = 1.5. A capsule of 2*(half + 0.35) fits
    // under it only while that total stays below 1.5, so the knob is what draws
    // the line — including the degenerate case where crouching changes nothing.
    auto pass = [](float crouch_half_height) {
        JoltWorld world;
        world.add_body(static_plane());
        world.add_body(static_box(Vec3{0, 1.75f, 4.0f}, Vec3{4.0f, 0.25f, 0.5f}));
        JoltCharacterConfig cfg;
        cfg.crouch_half_height = crouch_half_height;
        JoltCharacter ch(world, cfg, Vec3{0, 0.1f, 0});
        ch.set_crouch(true);
        NF_CHECK(ch.is_crouched());
        drive(world, ch, Vec3{0, 0, 1}, false, 180);
        return ch.position().z;
    };
    const float low = pass(0.05f);  // 0.8m capsule: ducked well under
    const float tall = pass(0.55f); // 1.8m capsule: crouching == standing
    NF_CHECK(low > 5.0f);
    NF_CHECK(tall < 4.0f);
    NF_CHECK(low > tall + 1.0f);
}

NF_TEST(jolt_character_max_speed_sets_the_cruise) {
    // 2 seconds of walking. Neither run may exceed its own speed-time product;
    // the gap between them is what proves the knob was read.
    auto distance = [](float max_speed) {
        JoltWorld world;
        world.add_body(static_plane());
        JoltCharacterConfig cfg;
        cfg.max_speed = max_speed;
        JoltCharacter ch(world, cfg, Vec3{0, 0.1f, 0});
        drive(world, ch, Vec3{0, 0, 1}, false, 120);
        return ch.position().z;
    };
    const float walk = distance(3.0f);
    const float sprint = distance(12.0f);
    NF_CHECK(walk > 4.0f);                    // it got somewhere
    NF_CHECK(walk < 3.0f * 2.0f + 0.1f);      // and not past its own ceiling
    NF_CHECK(sprint > walk + 8.0f);
    NF_CHECK(sprint < 12.0f * 2.0f + 0.1f);
}

// ---------------------------------------------------------------------------
// Bit-identical replay (§39, DoD). The case above checks the endpoint with a
// 1e-4 tolerance, which is what a replay needs to *look* smooth; the network
// hook needs more. A client predicts and the server replays; the correction the
// client applies on a mismatch is itself a discontinuity, so agreement has to be
// exact per tick, not approximately correct at the end. Exact float equality is
// the assertion here rather than a bug: identical inputs into identical worlds
// must produce identical bits.
// ---------------------------------------------------------------------------

std::vector<Vec3> replay_trajectory() {
    JoltWorld world;
    world.add_body(static_plane());
    world.add_body(static_box(Vec3{0, 0.15f, 3.0f}, Vec3{4.0f, 0.15f, 0.5f}));
    // A rideable moving platform: the ground-relative velocity math in
    // character_move is the branch most likely to diverge on replay, so the
    // sequence puts the character on a moving surface, not just past one.
    BodyDesc platform;
    platform.type = BodyType::Dynamic;
    platform.shape = Shape::make_box(Vec3{2.0f, 0.05f, 2.0f});
    platform.position = Vec3{0, 0.05f, 20.0f};
    platform.friction = 0.1f;
    platform.mass = 500.0f;     // heavy: not punted out from under the rider
    platform.allow_sleep = false;
    const JoltBody plat = world.add_body(platform);

    JoltCharacter ch(world, JoltCharacterConfig{}, Vec3{0, 0.1f, 0});
    std::vector<Vec3> samples;
    samples.reserve(480);
    for (int i = 0; i < 480; ++i) { // 8 seconds
        const float t = static_cast<float>(i);
        ch.set_crouch((i / 60) % 2 == 0); // stance toggled every second
        ch.move(Vec3{std::sin(t * 0.05f), 0.0f, 1.0f}, (i % 90) == 0, kDt);
        world.step(kDt);
        samples.push_back(ch.position());
        // Started only once the character is aboard, so the trigger is a
        // function of state the replay reaches identically, not of wall-clock.
        if (ch.position().z > 18.5f) world.set_linear_velocity(plat, Vec3{0, 0, 2.0f});
    }
    return samples;
}

NF_TEST(jolt_character_replay_is_bit_identical_over_every_tick) {
    const std::vector<Vec3> a = replay_trajectory();
    const std::vector<Vec3> b = replay_trajectory();
    NF_CHECK(!a.empty());
    NF_CHECK_EQ(a.size(), b.size());

    std::size_t first_diff = a.size();
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) {
            first_diff = i;
            break;
        }
    }
    NF_CHECK_EQ(first_diff, a.size()); // a.size() == "no tick differed"
}
