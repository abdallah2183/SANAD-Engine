// PhysicsTests — character controller: landing, walking, jumping, walls.

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/CharacterController.hpp>
#include <NF/Physics/PhysicsWorld.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

namespace {

PhysicsWorld make_ground_world() {
    PhysicsWorld world;
    BodyDesc ground;
    ground.type = BodyType::Static;
    ground.shape = Shape::make_plane(Vec3{0.0f, 1.0f, 0.0f});
    world.add_body(ground);
    return world;
}

void step_world(PhysicsWorld& world, CharacterController& hero, Vec3 wish, bool jump, float dt,
                int steps) {
    for (int i = 0; i < steps; ++i) {
        hero.move(wish, jump && i == 0, dt);
        world.step(dt);
        hero.post_step();
    }
}

} // namespace

NF_TEST(character_lands_and_grounds_on_plane) {
    PhysicsWorld world = make_ground_world();
    CharacterConfig cfg;
    CharacterController hero(world, cfg, Vec3{0.0f, 5.0f, 0.0f});
    NF_CHECK(!hero.grounded());
    step_world(world, hero, Vec3{}, false, 1.0f / 60.0f, 180);
    NF_CHECK(hero.grounded());
    // Resting center sits one radius above the plane.
    NF_CHECK_NEAR(hero.position().y, cfg.radius, 0.05f);
    NF_CHECK_NEAR(hero.velocity().y, 0.0f, 0.3f);
}

NF_TEST(character_walks_at_cruise_speed) {
    PhysicsWorld world = make_ground_world();
    CharacterConfig cfg;
    CharacterController hero(world, cfg, Vec3{0.0f, 1.0f, 0.0f});
    step_world(world, hero, Vec3{}, false, 1.0f / 60.0f, 60); // settle
    NF_CHECK(hero.grounded());
    const float x0 = hero.position().x;
    step_world(world, hero, Vec3{1.0f, 0.0f, 0.0f}, false, 1.0f / 60.0f, 120);
    const float dx = hero.position().x - x0;
    // 2 seconds at ~6 u/s (ramps up fast): well past halfway, never past max.
    NF_CHECK(dx > cfg.max_speed * 1.0f);
    NF_CHECK(dx <= cfg.max_speed * 2.0f + 0.5f);
    NF_CHECK(hero.grounded()); // stays planted while walking
}

NF_TEST(character_jump_arcs_and_lands) {
    PhysicsWorld world = make_ground_world();
    CharacterConfig cfg;
    CharacterController hero(world, cfg, Vec3{0.0f, 1.0f, 0.0f});
    step_world(world, hero, Vec3{}, false, 1.0f / 60.0f, 60);
    NF_CHECK(hero.grounded());
    float peak = hero.position().y;
    bool left_ground = false;
    for (int i = 0; i < 240; ++i) {
        hero.move(Vec3{}, i == 0, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
        hero.post_step();
        peak = peak > hero.position().y ? peak : hero.position().y;
        if (!hero.grounded()) left_ground = true;
        if (left_ground && hero.grounded()) break; // landed again
    }
    NF_CHECK(left_ground);
    NF_CHECK(hero.grounded());
    // v=7 jump against g=9.81 rises ~2.5 units.
    NF_CHECK(peak > cfg.radius + 1.0f);
    NF_CHECK(peak < cfg.radius + 4.0f);
}

NF_TEST(character_wall_stops_progress) {
    PhysicsWorld world = make_ground_world();
    BodyDesc wall;
    wall.type = BodyType::Static;
    wall.shape = Shape::make_box(Vec3{0.5f, 2.0f, 5.0f});
    wall.position = Vec3{5.0f, 2.0f, 0.0f};
    world.add_body(wall);

    CharacterConfig cfg;
    CharacterController hero(world, cfg, Vec3{0.0f, 1.0f, 0.0f});
    step_world(world, hero, Vec3{}, false, 1.0f / 60.0f, 60);
    step_world(world, hero, Vec3{1.0f, 0.0f, 0.0f}, false, 1.0f / 60.0f, 600);
    // Wall face at x=4.5, sphere radius 0.4: rests at ~4.1, never through.
    NF_CHECK(hero.position().x < 4.5f);
    NF_CHECK(hero.position().x > 3.0f);
}
