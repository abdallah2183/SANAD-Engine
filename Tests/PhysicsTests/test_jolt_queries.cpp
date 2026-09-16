// PhysicsTests — Jolt scene queries (ray/sweep/overlap), sensors (triggers)
// and continuous collision detection (CCD).
//
// Every case here is pure query work on a tiny world: no rendering, no
// threads, fixed dt — deterministic and well under the suite budget.

#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Test/TestFramework.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

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

BodyDesc static_box(Vec3 position, Vec3 half_extents) {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_box(half_extents);
    d.position = position;
    return d;
}

BodyDesc static_sphere(Vec3 position, float radius) {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape = Shape::make_sphere(radius);
    d.position = position;
    return d;
}

/// Zero-gravity world: query tests place bodies by hand and must not have them
/// fall between setup and assertion.
JoltWorld query_world() {
    PhysicsSettings settings;
    settings.gravity = Vec3{0, 0, 0};
    return JoltWorld(settings);
}

bool contains(const std::vector<JoltBody>& bodies, JoltBody body) {
    return std::find(bodies.begin(), bodies.end(), body) != bodies.end();
}

} // namespace

// --- Ray casts -----------------------------------------------------------

NF_TEST(jolt_query_ray_cast_hits_box) {
    JoltWorld world;
    // Box half-extent 0.5 at z = -5: its +z face sits at z = -4.5.
    const JoltBody target = world.add_body(static_box(Vec3{0, 0, -5}, Vec3{0.5f, 0.5f, 0.5f}));
    NF_CHECK(target.valid());

    const JoltWorld::QueryHit hit = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 5.0f);
    NF_CHECK(hit.hit);
    NF_CHECK(hit.body == target);
    NF_CHECK_NEAR(hit.distance, 4.5f, 0.02f);
    NF_CHECK_NEAR(hit.position.z, -4.5f, 0.02f);
    NF_CHECK(hit.fraction > 0.0f && hit.fraction <= 1.0f);
    NF_CHECK_NEAR(hit.fraction, 0.9f, 0.01f); // 4.5 / 5
    // The facing surface points back at the ray: +z, not the box's far face.
    NF_CHECK_NEAR(hit.normal.z, 1.0f, 1e-3f);
    NF_CHECK_NEAR(hit.normal.x, 0.0f, 1e-3f);
    NF_CHECK_NEAR(hit.normal.y, 0.0f, 1e-3f);

    // An unnormalized direction must give the same hit: max_distance is a
    // length, not a multiple of the direction's magnitude.
    const JoltWorld::QueryHit scaled = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -3}, 5.0f);
    NF_CHECK(scaled.hit);
    NF_CHECK(scaled.body == target);
    NF_CHECK_NEAR(scaled.distance, 4.5f, 0.02f);
}

NF_TEST(jolt_query_ray_cast_misses) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_box(Vec3{0, 0, -5}, Vec3{0.5f, 0.5f, 0.5f})).valid());

    // Pointing away from the box.
    const JoltWorld::QueryHit away = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 20.0f);
    NF_CHECK(!away.hit);
    NF_CHECK(!away.body.valid());

    // Right direction, too short: the face is at 4.5, the ray stops at 4.
    NF_CHECK(!world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 4.0f).hit);

    // Parallel but off-axis (2 above the box's top face at y = +0.5).
    NF_CHECK(!world.ray_cast(Vec3{0, 2, 0}, Vec3{0, 0, -1}, 20.0f).hit);
}

NF_TEST(jolt_query_ray_cast_ignores_body) {
    JoltWorld world;
    const JoltBody near_body = world.add_body(static_box(Vec3{0, 0, -5}, Vec3{0.5f, 0.5f, 0.5f}));
    const JoltBody far_body = world.add_body(static_box(Vec3{0, 0, -10}, Vec3{0.5f, 0.5f, 0.5f}));

    // Without an ignore the nearest body wins.
    const JoltWorld::QueryHit first = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 20.0f);
    NF_CHECK(first.hit);
    NF_CHECK(first.body == near_body);

    // Ignoring it exposes the body behind it: a skip, not a blanket miss.
    const JoltWorld::QueryHit second = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 20.0f, near_body);
    NF_CHECK(second.hit);
    NF_CHECK(second.body == far_body);
    NF_CHECK_NEAR(second.distance, 9.5f, 0.02f);

    // A dead handle is a no-op: the near body is hit again.
    world.remove_body(far_body);
    const JoltWorld::QueryHit third = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 20.0f, far_body);
    NF_CHECK(third.hit);
    NF_CHECK(third.body == near_body);
}

NF_TEST(jolt_query_invalid_inputs_are_safe) {
    // NOTE: an "invalid world" is not reachable through the public API (a
    // constructed JoltWorld is always valid), so the guards for it are
    // exercised by inspection only; every invalid *input* is covered here.
    JoltWorld world;
    const JoltBody box = world.add_body(static_box(Vec3{0, 0, -5}, Vec3{0.5f, 0.5f, 0.5f}));
    const JoltBody dead; // default-constructed = invalid

    // Degenerate cast inputs: all misses, no NaNs, no crash.
    NF_CHECK(!world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, 0}, 10.0f).hit);      // zero direction
    NF_CHECK(!world.ray_cast(Vec3{0, 0, 0}, Vec3{1e-9f, 0, 0}, 10.0f).hit);  // ~zero direction
    NF_CHECK(!world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 0.0f).hit);      // zero distance
    NF_CHECK(!world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, -5.0f).hit);     // negative distance
    NF_CHECK(!world.sphere_cast(Vec3{0, 0, 0}, 0.0f, Vec3{0, 0, -1}, 10.0f).hit);
    NF_CHECK(!world.sphere_cast(Vec3{0, 0, 0}, -1.0f, Vec3{0, 0, -1}, 10.0f).hit);
    NF_CHECK(!world.sphere_cast(Vec3{0, 0, 0}, 0.5f, Vec3{0, 0, 0}, 10.0f).hit);
    NF_CHECK(!world.sphere_cast(Vec3{0, 0, 0}, 0.5f, Vec3{0, 0, -1}, -1.0f).hit);

    // Degenerate overlaps return empty rather than a zero-extent Jolt shape.
    NF_CHECK(world.overlap_sphere(Vec3{0, 0, 0}, 0.0f).empty());
    NF_CHECK(world.overlap_sphere(Vec3{0, 0, 0}, -2.0f).empty());
    NF_CHECK(world.overlap_box(Vec3{0, 0, 0}, Vec3{0, 0, 0}).empty());
    NF_CHECK(world.overlap_box(Vec3{0, 0, 0}, Vec3{1, 0, 1}).empty());
    NF_CHECK(world.overlap_box(Vec3{0, 0, 0}, Vec3{-1, -1, -1}).empty());

    // Dead handles: empty results / no-ops, world still usable afterwards.
    NF_CHECK(world.trigger_overlaps(dead).empty());
    NF_CHECK(world.trigger_overlaps(box).empty()); // live, but alone in the world
    world.set_continuous_collision(dead, true);
    world.set_continuous_collision(dead, false);
    NF_CHECK(world.is_alive(box));

    // A dead handle as `ignore` must not swallow the hit.
    const JoltWorld::QueryHit hit = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 20.0f, dead);
    NF_CHECK(hit.hit);
    NF_CHECK(hit.body == box);
}

// --- Sphere sweeps -------------------------------------------------------

NF_TEST(jolt_query_sphere_cast_accounts_for_radius) {
    JoltWorld world;
    // Wall with its near face at z = -4.5 (half-extent 0.5 in z).
    const JoltBody wall = world.add_body(static_box(Vec3{0, 0, -5}, Vec3{2.0f, 2.0f, 0.5f}));
    NF_CHECK(wall.valid());

    const JoltWorld::QueryHit ray = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 10.0f);
    NF_CHECK(ray.hit);
    NF_CHECK_NEAR(ray.distance, 4.5f, 0.02f);

    // A radius-0.5 sphere stops half a metre earlier: its surface touches the
    // wall exactly where the ray does, its centre does not.
    const JoltWorld::QueryHit ball =
        world.sphere_cast(Vec3{0, 0, 0}, 0.5f, Vec3{0, 0, -1}, 10.0f);
    NF_CHECK(ball.hit);
    NF_CHECK(ball.body == wall);
    NF_CHECK_NEAR(ball.distance, 4.0f, 0.02f);
    NF_CHECK(ball.distance < ray.distance);
    NF_CHECK_NEAR(ball.position.z, -4.5f, 0.05f); // contact point on the wall face
    NF_CHECK_NEAR(ball.normal.z, 1.0f, 1e-2f);
    NF_CHECK(ball.fraction > 0.0f && ball.fraction <= 1.0f);
    NF_CHECK_NEAR(ball.fraction, 0.4f, 0.01f); // 4.0 / 10

    // The sweep obeys `ignore` the same way a ray does.
    const JoltWorld::QueryHit ignored =
        world.sphere_cast(Vec3{0, 0, 0}, 0.5f, Vec3{0, 0, -1}, 10.0f, wall);
    NF_CHECK(!ignored.hit);
}

// --- Overlap queries -----------------------------------------------------

NF_TEST(jolt_query_overlap_sphere_selects_region) {
    JoltWorld world = query_world();
    BodyDesc ball;
    ball.shape = Shape::make_sphere(0.5f);
    ball.position = Vec3{0, 0, -10};
    const JoltBody a = world.add_body(ball);
    ball.position = Vec3{10, 0, -10};
    const JoltBody b = world.add_body(ball);
    // Static scenery is queryable too: it must be reported where it overlaps
    // and nowhere else.
    const JoltBody c = world.add_body(static_box(Vec3{0, 10, -10}, Vec3{1, 1, 1}));

    const std::vector<JoltBody> hits = world.overlap_sphere(Vec3{0, 0, -10}, 1.0f);
    NF_CHECK(hits.size() == 1);
    NF_CHECK(hits[0] == a);

    // Empty space between the bodies.
    NF_CHECK(world.overlap_sphere(Vec3{0, 5, -10}, 1.0f).empty());

    // Static body still gets found (queries are not dynamic-only).
    const std::vector<JoltBody> box_hits = world.overlap_sphere(Vec3{0, 10, -10}, 0.5f);
    NF_CHECK(box_hits.size() == 1);
    NF_CHECK(box_hits[0] == c);

    // And the third body is not swept up by a query around the first two.
    const std::vector<JoltBody> far = world.overlap_sphere(Vec3{10, 0, -10}, 0.6f);
    NF_CHECK(far.size() == 1);
    NF_CHECK(far[0] == b);
}

NF_TEST(jolt_query_overlap_box_selects_region) {
    JoltWorld world = query_world();
    BodyDesc ball;
    ball.shape = Shape::make_sphere(0.5f);
    ball.position = Vec3{0, 0, -10};
    const JoltBody a = world.add_body(ball);
    ball.position = Vec3{10, 0, -10};
    const JoltBody b = world.add_body(ball);
    const JoltBody c = world.add_body(static_box(Vec3{0, 10, -10}, Vec3{1, 1, 1}));

    // Tight box around one body only.
    const std::vector<JoltBody> one = world.overlap_box(Vec3{10, 0, -10}, Vec3{1, 1, 1});
    NF_CHECK(one.size() == 1);
    NF_CHECK(one[0] == b);

    // Wide box spanning the two z = -10 bodies but stopping short of x = 10.
    const std::vector<JoltBody> two = world.overlap_box(Vec3{0, 5, -10}, Vec3{6, 6, 1});
    NF_CHECK(two.size() == 2);
    NF_CHECK(contains(two, a));
    NF_CHECK(contains(two, c));
    NF_CHECK(!contains(two, b));

    // Box nowhere near anything.
    NF_CHECK(world.overlap_box(Vec3{0, 0, 500}, Vec3{1, 1, 1}).empty());
}

// --- Sensors (triggers) --------------------------------------------------

NF_TEST(jolt_query_trigger_detects_but_does_not_block) {
    struct Run {
        float final_y = 0.0f;
        float min_y = 0.0f;
        bool saw_ball = false;
        bool saw_other = false;
    };
    // Same world twice: plain, and with a trigger volume the ball falls
    // through. The only difference between the runs is the sensor.
    auto run = [](bool with_trigger) {
        JoltWorld world;
        world.add_body(static_plane()); // floor at y = 0
        // Decoy scenery, far away: it must never show up in a trigger report.
        const JoltBody decoy = world.add_body(static_box(Vec3{50, 0, 0}, Vec3{1, 1, 1}));
        JoltBody trigger;
        if (with_trigger) {
            trigger = world.add_trigger(static_sphere(Vec3{0, 2, 0}, 1.0f));
            NF_CHECK(trigger.valid());
        }
        BodyDesc ball;
        ball.shape = Shape::make_sphere(0.25f);
        ball.position = Vec3{0, 5, 0};
        ball.allow_sleep = false;
        const JoltBody body = world.add_body(ball);

        Run out;
        out.min_y = 1e9f;
        for (int i = 0; i < 90; ++i) {
            world.step(1.0f / 60.0f);
            const float y = world.state(body).position.y;
            if (y < out.min_y) out.min_y = y;
            if (with_trigger) {
                for (const JoltBody& inside : world.trigger_overlaps(trigger)) {
                    if (inside == body) out.saw_ball = true;
                    if (inside == decoy) out.saw_other = true;
                }
            }
        }
        out.final_y = world.state(body).position.y;
        return out;
    };

    const Run with = run(true);
    const Run without = run(false);

    // The trigger saw the ball while it was inside the volume...
    NF_CHECK(with.saw_ball);
    NF_CHECK(!with.saw_other); // ...and nothing that was not inside it.
    // The ball went through: it ends up below the trigger (bottom at y = 1).
    NF_CHECK(with.final_y < 1.0f);
    NF_CHECK(with.min_y < 0.5f);
    // And it did not slow down: the sensor run is the control run.
    NF_CHECK_NEAR(with.final_y, without.final_y, 0.05f);
    NF_CHECK_NEAR(with.min_y, without.min_y, 0.05f);
}

NF_TEST(jolt_query_trigger_lifecycle_and_counts) {
    JoltWorld world;
    const usize base = world.body_count();

    const JoltBody trigger = world.add_trigger(static_sphere(Vec3{0, 2, 0}, 1.0f));
    NF_CHECK(trigger.valid());
    NF_CHECK(world.is_alive(trigger));
    NF_CHECK(world.body_count() == base + 1); // counted like any other body

    // A sensor is invisible to every cast and overlap query — even when it is
    // the only thing on the ray.
    NF_CHECK(!world.ray_cast(Vec3{0, 2, 10}, Vec3{0, 0, -1}, 20.0f).hit);
    NF_CHECK(!world.sphere_cast(Vec3{0, 2, 10}, 0.1f, Vec3{0, 0, -1}, 20.0f).hit);
    NF_CHECK(world.overlap_sphere(Vec3{0, 2, 0}, 0.5f).empty());
    NF_CHECK(world.overlap_box(Vec3{0, 2, 0}, Vec3{0.5f, 0.5f, 0.5f}).empty());

    // Control: the same volume as a NORMAL body is reported by all of them, so
    // the checks above are filtering sensors rather than failing to work.
    const JoltBody solid = world.add_body(static_sphere(Vec3{0, 2, 0}, 1.0f));
    const JoltWorld::QueryHit ray = world.ray_cast(Vec3{0, 2, 10}, Vec3{0, 0, -1}, 20.0f);
    NF_CHECK(ray.hit);
    NF_CHECK(ray.body == solid);
    const std::vector<JoltBody> overlaps = world.overlap_sphere(Vec3{0, 2, 0}, 0.5f);
    NF_CHECK(overlaps.size() == 1);
    NF_CHECK(overlaps[0] == solid);

    // The trigger reports what is inside it — the solid body, not itself.
    const std::vector<JoltBody> inside = world.trigger_overlaps(trigger);
    NF_CHECK(inside.size() == 1);
    NF_CHECK(inside[0] == solid);

    // Removal follows the normal body bookkeeping.
    world.remove_body(trigger);
    NF_CHECK(!world.is_alive(trigger));
    NF_CHECK(world.trigger_overlaps(trigger).empty());
    NF_CHECK(world.body_count() == base + 1); // the solid body is still here
    world.remove_body(solid);
    NF_CHECK(world.body_count() == base);
}

// --- Continuous collision detection (CCD) --------------------------------

NF_TEST(jolt_query_ccd_stops_fast_body) {
    // Flight path: 120 m/s * (1/60) s = 2 m per step, aimed at a 0.1 m thick
    // wall 5 m away. Discrete integration steps from -5 to -3, -1, +1 ... and
    // never samples a position where the ball overlaps the wall, so it
    // tunnels; a linear-cast body is caught on the near face.
    auto run = [](bool ccd) {
        PhysicsSettings settings;
        settings.gravity = Vec3{0, 0, 0}; // straight line: the wall is the only variable
        JoltWorld world(settings);
        const JoltBody wall = world.add_body(static_box(Vec3{0, 0, 0}, Vec3{4, 4, 0.05f}));
        NF_CHECK(wall.valid());

        BodyDesc ball;
        ball.type = BodyType::Dynamic;
        ball.shape = Shape::make_sphere(0.25f);
        ball.position = Vec3{0, 0, -5};
        ball.restitution = 0.0f;
        ball.allow_sleep = false;
        const JoltBody body = world.add_body(ball);
        NF_CHECK(body.valid());

        world.set_continuous_collision(body, ccd);
        world.set_linear_velocity(body, Vec3{0, 0, 120});
        step(world, 6);
        return world.state(body).position.z;
    };

    const float ccd_z = run(true);
    // Caught one radius in front of the near face (z = -0.05), i.e. -0.3:
    // never on the far side.
    NF_CHECK(ccd_z < -0.1f);
    NF_CHECK(ccd_z > -1.0f);

    // Control: the identical launch without CCD ends up past the wall.
    const float discrete_z = run(false);
    NF_CHECK(discrete_z > 0.5f);
}
