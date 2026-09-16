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
