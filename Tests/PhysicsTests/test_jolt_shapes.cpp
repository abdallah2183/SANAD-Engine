// PhysicsTests — complex colliders on the Jolt backend (design §38): capsule,
// convex hull and static triangle mesh.
//
// These three shapes arrive through additive entry points (JoltWorld's
// add_capsule_body / add_convex_hull_body / add_mesh_body) that keep the
// closed first-party Shape vocabulary untouched. What each case verifies is
// therefore two things at once: the collider is the shape that was asked for
// (a capsule that silently becomes a sphere is a WRONG collider, not a
// missing one) and it is an ordinary body to the rest of the wrapper —
// queries, CCD, life cycle.
//
// Every case is a tiny headless world with a fixed dt: no rendering, no
// threads, deterministic.

#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Test/TestFramework.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kPi = 3.14159265358979323846f;

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

BodyDesc dynamic_sphere(Vec3 position, float radius) {
    BodyDesc d;
    d.type = BodyType::Dynamic;
    d.shape = Shape::make_sphere(radius);
    d.position = position;
    return d;
}

BodyDesc dynamic_box(Vec3 position, Vec3 half_extents) {
    BodyDesc d;
    d.type = BodyType::Dynamic;
    d.shape = Shape::make_box(half_extents);
    d.position = position;
    return d;
}

void step(JoltWorld& world, int n, float dt = kDt) {
    for (int i = 0; i < n; ++i) world.step(dt);
}

/// Zero-gravity world: shape tests place bodies by hand and must not have them
/// fall between setup and assertion.
JoltWorld shape_world() {
    PhysicsSettings settings;
    settings.gravity = Vec3{0, 0, 0};
    return JoltWorld(settings);
}

/// The 8 corners of an axis-aligned box — the classic convex-hull input, and
/// the one case where the hull has an exact reference to compare against.
std::vector<Vec3> box_corners(Vec3 center, Vec3 half_extents) {
    std::vector<Vec3> out;
    out.reserve(8);
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                out.push_back(Vec3{center.x + static_cast<float>(sx) * half_extents.x,
                                   center.y + static_cast<float>(sy) * half_extents.y,
                                   center.z + static_cast<float>(sz) * half_extents.z});
            }
        }
    }
    return out;
}

/// A two-triangle quad in the y = 0 plane, wound CCW seen from above so the
/// face normal is +Y: mesh triangles are single-sided for simulation.
void floor_patch(std::vector<Vec3>& vertices, std::vector<u32>& indices) {
    vertices = {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{2, 0, 2}, Vec3{-2, 0, 2}};
    indices = {0, 2, 1, 0, 3, 2};
}

bool contains(const std::vector<JoltBody>& bodies, JoltBody body) {
    return std::find(bodies.begin(), bodies.end(), body) != bodies.end();
}

} // namespace

// --- Capsule -------------------------------------------------------------

NF_TEST(jolt_capsule_falls_and_rests_on_floor) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());

    constexpr float kRadius = 0.3f;
    constexpr float kHalfHeight = 0.9f;
    BodyDesc base;
    base.position = Vec3{0, 1.5f, 0};
    const JoltBody capsule = world.add_capsule_body(base, kRadius, kHalfHeight);
    NF_CHECK(capsule.valid());
    NF_CHECK(world.is_alive(capsule));
    NF_CHECK(world.body_count() == 2); // the plane + the capsule, nothing else

    step(world, 180);
    const JoltBodyState st = world.state(capsule);
    NF_CHECK(world.is_alive(capsule));
    // Standing upright on its lower cap: the centre sits half_height + radius
    // above the floor (1.2), not at the radius alone.
    NF_CHECK_NEAR(st.position.y, kHalfHeight + kRadius, 0.05f);
    NF_CHECK(std::abs(st.linear_velocity.y) < 0.1f);
    NF_CHECK_NEAR(st.position.x, 0.0f, 0.05f);
    NF_CHECK_NEAR(st.position.z, 0.0f, 0.05f);
}

NF_TEST(jolt_capsule_collides_like_a_capsule_not_a_sphere) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());

    constexpr float kRadius = 0.3f;
    constexpr float kHalfHeight = 2.0f;

    BodyDesc capsule_desc;
    capsule_desc.position = Vec3{-2, 2.5f, 0};
    const JoltBody capsule = world.add_capsule_body(capsule_desc, kRadius, kHalfHeight);
    NF_CHECK(capsule.valid());

    // Control: a real sphere of the same radius, released from the same height.
    // (The old bounding-sphere fallback was radius 0.5, so it would have
    // parked the capsule around y = 0.5 instead of 2.3.)
    const JoltBody sphere = world.add_body(dynamic_sphere(Vec3{2, 2.5f, 0}, kRadius));
    NF_CHECK(sphere.valid());

    step(world, 240);
    const float capsule_y = world.state(capsule).position.y;
    const float sphere_y = world.state(sphere).position.y;

    NF_CHECK_NEAR(sphere_y, kRadius, 0.05f);                // the control reaches the floor
    NF_CHECK_NEAR(capsule_y, kHalfHeight + kRadius, 0.05f); // the capsule rests on its cap
    // The discriminator: the two colliders are not interchangeable at all.
    NF_CHECK(capsule_y > sphere_y + 1.5f);
    NF_CHECK(capsule_y > 2.0f);
}

NF_TEST(jolt_capsule_slides_or_rolls_downhill) {
    constexpr float kTilt = 25.0f * kPi / 180.0f;
    constexpr float kRadius = 0.25f;
    // A 25-degree ramp about +Z: its top face normal tilts toward -X, so
    // "downhill" is -X. A capsule whose axis runs along Z is a log on that
    // ramp and rolls in the XY plane.
    const Vec3 tilted_normal = Vec3{-std::sin(kTilt), std::cos(kTilt), 0.0f};
    const Vec3 tilted_face = Vec3{-0.25f * std::sin(kTilt), 0.25f * std::cos(kTilt), 0.0f};

    auto run = [&](bool tilted) {
        JoltWorld world;
        BodyDesc slope = static_box(Vec3{0, 0, 0}, Vec3{4.0f, 0.25f, 4.0f});
        if (tilted) slope.orientation = Quat::from_axis_angle(Vec3{0, 0, 1}, kTilt);
        NF_CHECK(world.add_body(slope).valid());

        // Placed just above the top face, along its normal.
        const Vec3 normal = tilted ? tilted_normal : Vec3{0, 1, 0};
        const Vec3 face = tilted ? tilted_face : Vec3{0, 0.25f, 0};
        BodyDesc base;
        base.orientation = Quat::from_axis_angle(Vec3{1, 0, 0}, 0.5f * kPi); // axis Y -> Z
        base.position = face + normal * (kRadius + 0.05f);
        const JoltBody capsule = world.add_capsule_body(base, kRadius, 0.5f);
        NF_CHECK(capsule.valid());

        const Vec3 start = world.state(capsule).position;
        step(world, 60); // one second, no input of any kind
        return world.state(capsule).position - start;
    };

    const Vec3 moved = run(true);
    NF_CHECK(moved.x < -0.5f);  // it went downhill...
    NF_CHECK(moved.y < -0.05f); // ...and dropped with it
    // Control: the same capsule on flat ground stays where it was put, so the
    // displacement above is the slope and not a phantom force.
    const Vec3 pinned = run(false);
    NF_CHECK(std::abs(pinned.x) < 0.05f);
    NF_CHECK(std::abs(pinned.z) < 0.05f);
}

NF_TEST(jolt_capsule_queries_work) {
    JoltWorld world = shape_world();
    BodyDesc base;
    base.type = BodyType::Static;
    base.position = Vec3{0, 2, 0};
    const JoltBody capsule = world.add_capsule_body(base, 0.4f, 1.0f);
    NF_CHECK(capsule.valid());

    // Side hit: the surface is exactly `radius` from the axis.
    const JoltWorld::QueryHit side = world.ray_cast(Vec3{5, 2, 0}, Vec3{-1, 0, 0}, 10.0f);
    NF_CHECK(side.hit);
    NF_CHECK(side.body == capsule);
    NF_CHECK_NEAR(side.distance, 5.0f - 0.4f, 0.02f);
    NF_CHECK_NEAR(side.normal.x, 1.0f, 1e-3f);

    // Axial hit: the top cap reaches half_height + radius above the centre
    // (3.4 here), which a sphere collider cannot reproduce.
    const JoltWorld::QueryHit top = world.ray_cast(Vec3{0, 6, 0}, Vec3{0, -1, 0}, 10.0f);
    NF_CHECK(top.hit);
    NF_CHECK(top.body == capsule);
    NF_CHECK_NEAR(top.distance, 6.0f - (2.0f + 1.0f + 0.4f), 0.02f);

    // And it is an ordinary body to the overlap query.
    const std::vector<JoltBody> overlaps = world.overlap_sphere(Vec3{0, 2, 0}, 0.5f);
    NF_CHECK(contains(overlaps, capsule));
}

// --- Convex hull ---------------------------------------------------------

NF_TEST(jolt_convex_hull_box_corners_match_a_box) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());

    // The hull of a box's own corners must behave like the box: this is the
    // reference shape the hull builder is compared against.
    BodyDesc hull_desc;
    hull_desc.position = Vec3{-2, 2, 0};
    const JoltBody hull =
        world.add_convex_hull_body(hull_desc, box_corners(Vec3{0, 0, 0}, Vec3{0.5f, 0.5f, 0.5f}));
    NF_CHECK(hull.valid());
    const JoltBody box = world.add_body(dynamic_box(Vec3{2, 2, 0}, Vec3{0.5f, 0.5f, 0.5f}));
    NF_CHECK(box.valid());
    NF_CHECK(world.body_count() == 3);

    step(world, 240);
    const float hull_y = world.state(hull).position.y;
    const float box_y = world.state(box).position.y;
    NF_CHECK_NEAR(box_y, 0.5f, 0.05f);   // the control really is resting on the floor
    NF_CHECK_NEAR(hull_y, box_y, 0.05f); // and the hull lands at the same height
    NF_CHECK(std::abs(world.state(hull).linear_velocity.y) < 0.05f); // settled, not jittering

    step(world, 120); // two more seconds: it must stay put, not sink or drift
    NF_CHECK_NEAR(world.state(hull).position.y, hull_y, 0.01f);
}

NF_TEST(jolt_convex_hull_is_queryable) {
    JoltWorld world = shape_world();
    BodyDesc base;
    base.type = BodyType::Static;
    base.position = Vec3{0, 0, -5};
    const JoltBody hull =
        world.add_convex_hull_body(base, box_corners(Vec3{0, 0, 0}, Vec3{0.5f, 0.5f, 0.5f}));
    NF_CHECK(hull.valid());

    // The +Z face of a half-extent 0.5 hull at z = -5 sits at z = -4.5.
    const JoltWorld::QueryHit ray = world.ray_cast(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 10.0f);
    NF_CHECK(ray.hit);
    NF_CHECK(ray.body == hull);
    NF_CHECK_NEAR(ray.distance, 4.5f, 0.05f);
    NF_CHECK_NEAR(ray.normal.z, 1.0f, 1e-2f);

    const std::vector<JoltBody> overlaps = world.overlap_sphere(Vec3{0, 0, -5}, 0.3f);
    NF_CHECK(contains(overlaps, hull));

    // A 0.25 sphere swept at the hull stops one radius in front of the face.
    const JoltWorld::QueryHit sweep = world.sphere_cast(Vec3{0, 0, 0}, 0.25f, Vec3{0, 0, -1}, 10.0f);
    NF_CHECK(sweep.hit);
    NF_CHECK(sweep.body == hull);
    NF_CHECK_NEAR(sweep.distance, 4.25f, 0.05f);
}

NF_TEST(jolt_convex_hull_degenerate_points_are_safe) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    const usize base_count = world.body_count();
    BodyDesc base;
    base.position = Vec3{0, 2, 0};

    // Nothing to build a hull from.
    NF_CHECK(!world.add_convex_hull_body(base, {}).valid());
    // Three points are a triangle, not a hull (the documented floor is 4).
    NF_CHECK(!world.add_convex_hull_body(base, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}).valid());
    // Four copies of one point: no volume either.
    NF_CHECK(!world.add_convex_hull_body(base, std::vector<Vec3>(4, Vec3{0, 0, 0})).valid());
    // A coplanar cloud (hexagon in the y = 0 plane). Jolt v5.6 does NOT report
    // this one as an error: its builder accepts the points and returns a hull
    // of volume 0 (which the convex radius then puffs into a thin slab), so
    // the wrapper rejects it on the volume instead.
    const std::vector<Vec3> coplanar = {Vec3{-1, 0, -1}, Vec3{1, 0, -1}, Vec3{2, 0, 0},
                                        Vec3{1, 0, 1},   Vec3{-1, 0, 1}, Vec3{-2, 0, 0}};
    NF_CHECK(!world.add_convex_hull_body(base, coplanar).valid());
    // Collinear points go down Jolt's reported-error path.
    NF_CHECK(!world.add_convex_hull_body(base, {Vec3{-2, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, 0},
                                               Vec3{1, 0, 0}})
                  .valid());

    NF_CHECK(world.body_count() == base_count); // none of them added a body
}

// --- Static triangle mesh ------------------------------------------------

NF_TEST(jolt_mesh_static_wall_blocks_ball) {
    JoltWorld world;
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
    floor_patch(vertices, indices);
    BodyDesc mesh_desc; // static, vertices are already in body-local world coords
    const JoltBody mesh = world.add_mesh_body(mesh_desc, vertices, indices);
    NF_CHECK(mesh.valid());
    NF_CHECK(world.is_alive(mesh));

    // Off the quad's diagonal, so the ball lands in the interior of a triangle:
    // an internal edge is not what this case is about.
    const JoltBody ball = world.add_body(dynamic_sphere(Vec3{0, 2, 1}, 0.25f));
    NF_CHECK(ball.valid());

    step(world, 180);
    const JoltBodyState st = world.state(ball);
    NF_CHECK(world.is_alive(ball));
    NF_CHECK(st.position.y > 0.1f);                  // it did not fall through...
    NF_CHECK_NEAR(st.position.y, 0.25f, 0.05f);      // ...it is resting ON the mesh patch
    step(world, 120);                                // and it stays there
    NF_CHECK_NEAR(world.state(ball).position.y, 0.25f, 0.05f);
}

NF_TEST(jolt_mesh_body_is_forced_static) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
    floor_patch(vertices, indices);

    // Asked for Dynamic, documented to get Static: a Jolt mesh collider may
    // not move.
    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.position = Vec3{0, 3, 0};
    const JoltBody mesh = world.add_mesh_body(desc, vertices, indices);
    NF_CHECK(mesh.valid());
    NF_CHECK(world.is_alive(mesh));

    // Control body: gravity is on in this world, so a sphere let go at the
    // same height falls. (It is placed off the +-2 patch so it never meets the
    // mesh.) That is what makes "the mesh did not move" meaningful.
    const JoltBody ball = world.add_body(dynamic_sphere(Vec3{4, 3, 0}, 0.25f));
    NF_CHECK(ball.valid());
    step(world, 180);

    NF_CHECK_NEAR(world.state(mesh).position.y, 3.0f, 1e-4f);
    NF_CHECK(world.state(mesh).linear_velocity.y == 0.0f);
    NF_CHECK(world.state(ball).position.y < 1.0f);
}

NF_TEST(jolt_mesh_invalid_inputs_are_safe) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    const usize base_count = world.body_count();
    BodyDesc desc;

    const std::vector<Vec3> quad = {Vec3{-1, 0, -1}, Vec3{1, 0, -1}, Vec3{1, 0, 1}, Vec3{-1, 0, 1}};

    NF_CHECK(!world.add_mesh_body(desc, {}, {}).valid());                 // no geometry at all
    NF_CHECK(!world.add_mesh_body(desc, quad, {}).valid());               // vertices, no triangles
    NF_CHECK(!world.add_mesh_body(desc, {Vec3{0, 0, 0}, Vec3{1, 0, 0}}, {0, 1, 0}).valid());
    NF_CHECK(!world.add_mesh_body(desc, quad, {0, 1}).valid());           // ragged index list
    // Out-of-range index: Jolt's MeshShapeSettings::Sanitize() indexes the
    // vertex list with the raw triangle indices, so this one is an
    // out-of-bounds read over there — caught up front here instead.
    NF_CHECK(!world.add_mesh_body(desc, quad, {0, 1, 7}).valid());
    NF_CHECK(!world.add_mesh_body(desc, quad, {0, 1, 4}).valid());        // 4 == size(): one past the end

    NF_CHECK(world.body_count() == base_count);
}

// --- Everything together: CCD and life cycle ------------------------------

NF_TEST(jolt_complex_shapes_ccd_and_lifecycle) {
    // Zero gravity, one fast small ball per run: 120 m/s * (1/60) s = 2 m per
    // step, launched 5 m away from the wall, so discrete integration samples
    // -5, -3, -1, +1 ... and never sees the contact. The only variable between
    // the runs is the wall (hull or capsule) and whether CCD is enabled.
    auto run = [](bool hull_wall, bool ccd) {
        PhysicsSettings settings;
        settings.gravity = Vec3{0, 0, 0};
        JoltWorld world(settings);
        BodyDesc wall;
        wall.type = BodyType::Static;
        if (hull_wall) {
            // 0.1 m thick convex-hull wall, 8 m tall.
            NF_CHECK(world
                         .add_convex_hull_body(wall, box_corners(Vec3{0, 0, 0}, Vec3{4, 4, 0.05f}))
                         .valid());
        } else {
            // Capsule pillar, axis along Y.
            NF_CHECK(world.add_capsule_body(wall, 0.5f, 4.0f).valid());
        }

        const JoltBody ball = world.add_body(dynamic_sphere(Vec3{0, 0, -5}, 0.25f));
        NF_CHECK(ball.valid());
        world.set_continuous_collision(ball, ccd);
        world.set_linear_velocity(ball, Vec3{0, 0, 120});
        step(world, 6);
        return world.state(ball).position.z;
    };

    const float hull_stopped = run(true, true);
    NF_CHECK(hull_stopped < -0.1f); // caught in front of the near face (z = -0.3)...
    NF_CHECK(hull_stopped > -1.0f); // ...not somewhere down the path
    NF_CHECK(run(true, false) > 0.5f); // control: the same launch without CCD tunnels

    const float capsule_stopped = run(false, true);
    NF_CHECK(capsule_stopped < -0.3f); // the capsule surface (0.5) plus the ball (0.25)
    NF_CHECK(capsule_stopped > -1.5f);

    // Life cycle: a complex collider is an ordinary body — counted, alive,
    // removable.
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    BodyDesc desc;
    desc.position = Vec3{0, 1, 0};
    const JoltBody capsule = world.add_capsule_body(desc, 0.3f, 0.6f);
    const JoltBody hull =
        world.add_convex_hull_body(desc, box_corners(Vec3{0, 0, 0}, Vec3{0.3f, 0.3f, 0.3f}));
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
    floor_patch(vertices, indices);
    BodyDesc mesh_desc;
    const JoltBody mesh = world.add_mesh_body(mesh_desc, vertices, indices);
    NF_CHECK(capsule.valid());
    NF_CHECK(hull.valid());
    NF_CHECK(mesh.valid());
    NF_CHECK(world.body_count() == 4); // the plane + the three complex bodies

    for (const JoltBody body : {capsule, hull, mesh}) {
        NF_CHECK(world.is_alive(body));
        world.remove_body(body);
        NF_CHECK(!world.is_alive(body));
    }
    NF_CHECK(world.body_count() == 1);
}
