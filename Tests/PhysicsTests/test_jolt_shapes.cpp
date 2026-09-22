// PhysicsTests — complex colliders on the Jolt backend (design §38): capsule,
// cylinder, convex hull, static triangle mesh, height field, compound.
//
// These shapes arrive through additive entry points (JoltWorld's
// add_capsule_body / add_cylinder_body / add_convex_hull_body / add_mesh_body /
// add_heightfield_body / add_compound_body) that keep the closed first-party
// Shape vocabulary untouched. What each case verifies is therefore two things at
// once: the collider is the shape that was asked for (a capsule that silently
// becomes a sphere is a WRONG collider, not a missing one) and it is an ordinary
// body to the rest of the wrapper — queries, CCD, life cycle.
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

// --- Height field ---------------------------------------------------------

namespace {

/// The field used by every heightfield case: an 8x8 grid laid over [-4, 4]^2
/// in XZ, flat at y = 0 except a raised central square. The engine's terrain
/// convention (grid in XZ, +Y up) is Jolt's too, so `heights[y * n + x]` is
/// both the sample a renderer would read and the one the collider gets.
constexpr u32 kHfCount = 8;
constexpr Vec3 kHfOffset{-4.0f, 0.0f, -4.0f};
constexpr Vec3 kHfScale{8.0f / 7.0f, 1.0f, 8.0f / 7.0f};
constexpr float kPlateauHeight = 2.0f;

std::vector<float> plateau_field() {
    std::vector<float> h(kHfCount * kHfCount, 0.0f);
    // Samples x,y in {3, 4} are the raised square. Sample index x maps to world
    // X = -4 + (8/7)x, so 3 and 4 are -0.571 and +0.571: the plateau is the
    // square [-0.571, 0.571]^2 at y = 2, centred on the origin.
    for (u32 y = 0; y < kHfCount; ++y) {
        for (u32 x = 0; x < kHfCount; ++x) {
            if ((x == 3 || x == 4) && (y == 3 || y == 4)) {
                h[y * kHfCount + x] = kPlateauHeight;
            }
        }
    }
    return h;
}

} // namespace

NF_TEST(jolt_heightfield_ball_rests_on_the_ridge_not_the_floor) {
    JoltWorld world;
    const JoltBody field =
        world.add_heightfield_body(BodyDesc{}, plateau_field(), kHfCount, kHfOffset, kHfScale);
    NF_CHECK(field.valid());
    NF_CHECK(world.is_alive(field));

    // Dropped over the raised square: it must come to rest 2 + 0.25 up, not on
    // the flat base. This is the whole point of the collider — a height field
    // that silently collapsed to a box or a plane parks the ball at 0.25.
    const JoltBody on_ridge = world.add_body(dynamic_sphere(Vec3{0, 5, 0}, 0.25f));
    // Control: same ball, same height, over the flat part of the same field.
    const JoltBody on_flat = world.add_body(dynamic_sphere(Vec3{3.5f, 5, 3.5f}, 0.25f));
    NF_CHECK(on_ridge.valid());
    NF_CHECK(on_flat.valid());
    NF_CHECK(world.body_count() == 3);

    step(world, 240);
    NF_CHECK_NEAR(world.state(on_flat).position.y, 0.25f, 0.05f);
    NF_CHECK_NEAR(world.state(on_ridge).position.y, kPlateauHeight + 0.25f, 0.05f);
    NF_CHECK(world.state(on_ridge).position.y > world.state(on_flat).position.y + 1.0f);
    NF_CHECK(std::abs(world.state(on_ridge).linear_velocity.y) < 0.1f);

    step(world, 120); // it stays parked, it does not slide off or sink
    NF_CHECK_NEAR(world.state(on_ridge).position.y, kPlateauHeight + 0.25f, 0.05f);
    NF_CHECK_NEAR(world.state(on_ridge).position.x, 0.0f, 0.15f);
}

NF_TEST(jolt_heightfield_body_is_forced_static) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    const usize base_count = world.body_count();

    // Asked for Dynamic, documented to get Static: a Jolt height field may not
    // move, exactly like the mesh collider.
    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.position = Vec3{0, 3, 0};
    const JoltBody field =
        world.add_heightfield_body(desc, plateau_field(), kHfCount, kHfOffset, kHfScale);
    NF_CHECK(field.valid());
    NF_CHECK(world.is_alive(field));

    // Control: gravity is on, so a sphere let go at the same height falls.
    // (Placed over the flat part, where the field is at y = 0, so it lands
    // rather than resting on the plateau.)
    const JoltBody ball = world.add_body(dynamic_sphere(Vec3{3.5f, 3, 3.5f}, 0.25f));
    NF_CHECK(ball.valid());
    step(world, 180);

    NF_CHECK_NEAR(world.state(field).position.y, 3.0f, 1e-4f);
    NF_CHECK(world.state(field).linear_velocity.y == 0.0f);
    NF_CHECK(world.state(ball).position.y < 1.0f);
    NF_CHECK(world.body_count() == base_count + 2);
}

NF_TEST(jolt_heightfield_is_queryable) {
    JoltWorld world = shape_world();
    const JoltBody field =
        world.add_heightfield_body(BodyDesc{}, plateau_field(), kHfCount, kHfOffset, kHfScale);
    NF_CHECK(field.valid());

    // Straight down through the plateau's centre: the surface is at y = 2, so a
    // ray from y = 10 travels 8. A collider that ignored the samples (a box at
    // the field's bounds, or a plane) would not give this number.
    const JoltWorld::QueryHit down = world.ray_cast(Vec3{0, 10, 0}, Vec3{0, -1, 0}, 20.0f);
    NF_CHECK(down.hit);
    NF_CHECK(down.body == field);
    NF_CHECK_NEAR(down.distance, 8.0f, 0.05f);
    NF_CHECK_NEAR(down.normal.y, 1.0f, 1e-2f);

    // Over the flat part the same ray reaches the base instead.
    const JoltWorld::QueryHit flat = world.ray_cast(Vec3{3.5f, 10, 3.5f}, Vec3{0, -1, 0}, 20.0f);
    NF_CHECK(flat.hit);
    NF_CHECK(flat.body == field);
    NF_CHECK_NEAR(flat.distance, 10.0f, 0.05f);

    const std::vector<JoltBody> overlaps = world.overlap_sphere(Vec3{0, 2.4f, 0}, 0.5f);
    NF_CHECK(contains(overlaps, field));

    // A 0.25 sphere swept down at the plateau stops one radius above the ridge.
    const JoltWorld::QueryHit sweep =
        world.sphere_cast(Vec3{0, 10, 0}, 0.25f, Vec3{0, -1, 0}, 20.0f);
    NF_CHECK(sweep.hit);
    NF_CHECK(sweep.body == field);
    NF_CHECK_NEAR(sweep.distance, 8.0f - 0.25f, 0.05f);
}

NF_TEST(jolt_heightfield_invalid_inputs_are_safe) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    const usize base_count = world.body_count();
    BodyDesc desc;
    const std::vector<float> ok = plateau_field();

    // Sample count below Jolt's floor of 4 (sample_count / block_size >= 2 with
    // the default block size of 2).
    NF_CHECK(!world.add_heightfield_body(desc, {0.0f, 0.0f, 0.0f, 0.0f}, 2, kHfOffset, kHfScale)
                  .valid());
    // Buffer does not match the declared grid: Jolt's constructor copies
    // sample_count^2 values sight unseen, so this is caught here rather than
    // read out of bounds.
    NF_CHECK(!world.add_heightfield_body(desc, ok, 16, kHfOffset, kHfScale).valid());
    NF_CHECK(!world.add_heightfield_body(desc, std::vector<float>(63, 0.0f), 8, kHfOffset,
                                         kHfScale)
                  .valid());
    // Non-finite samples: the builder would otherwise quantize a NaN.
    std::vector<float> nan_field = ok;
    nan_field[3 * kHfCount + 3] = std::nanf("");
    NF_CHECK(!world.add_heightfield_body(desc, nan_field, kHfCount, kHfOffset, kHfScale).valid());
    // A zero X or Z scale collapses the grid to a line: no field to hit.
    NF_CHECK(!world.add_heightfield_body(desc, ok, kHfCount, kHfOffset,
                                         Vec3{0.0f, 1.0f, 1.0f})
                  .valid());
    NF_CHECK(!world.add_heightfield_body(desc, ok, kHfCount, kHfOffset,
                                         Vec3{1.0f, 1.0f, 0.0f})
                  .valid());

    NF_CHECK(world.body_count() == base_count); // none of them added a body

    // A zero Y scale is a flat field, which is a legal plane and stays allowed.
    const JoltBody flat =
        world.add_heightfield_body(desc, std::vector<float>(16, 1.5f), 4, Vec3{0, 0, 0},
                                   Vec3{1.0f, 0.0f, 1.0f});
    NF_CHECK(flat.valid());
    NF_CHECK(world.body_count() == base_count + 1);

    // And it is an ordinary body: counted, alive, removable.
    world.remove_body(flat);
    NF_CHECK(!world.is_alive(flat));
    NF_CHECK(world.body_count() == base_count);
}

// --- Compound -------------------------------------------------------------

NF_TEST(jolt_compound_is_concave_where_a_hull_cannot_be) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());

    // A U-bracket: a base bar with two arms, open at the top. A convex hull of
    // its corners is the bounding BOX of the U, groove filled in — so a body
    // dropped into the U's mouth lands on the base, while the same body dropped
    // onto the hull lands an arm's height higher. That difference is the whole
    // reason the compound exists: the hull cannot have a groove.
    const Vec3 arm{0.25f, 1.0f, 0.5f};
    const Vec3 base{1.5f, 0.25f, 0.5f};
    std::vector<JoltWorld::CompoundPart> parts;
    JoltWorld::CompoundPart base_part;
    base_part.shape = JoltWorld::CompoundShape::make_box(base);
    // The base is centred on y = 0.25 so it spans 0..0.5: the compound's LOWEST
    // local point is then 0, same as the hull whose corners start at y = 0. Both
    // origins sit at their shape's own bottom, which is what makes the two
    // rest-height assertions below comparable. Centring the base on 0 instead
    // puts its bottom half a part BELOW the origin, so the bracket rests 0.25
    // above the floor while the hull rests at 0 — the two are no longer
    // measuring the same thing and the ball-in-mouth height shifts with it.
    base_part.position = Vec3{0, 0.25f, 0}; // base spans y = 0 .. 0.5
    JoltWorld::CompoundPart left;
    left.shape = JoltWorld::CompoundShape::make_box(arm);
    left.position = Vec3{-1.0f, 1.25f, 0}; // arms run y = 0.25 .. 2.25
    JoltWorld::CompoundPart right;
    right.shape = JoltWorld::CompoundShape::make_box(arm);
    right.position = Vec3{1.0f, 1.25f, 0};
    parts.push_back(base_part);
    parts.push_back(left);
    parts.push_back(right);

    BodyDesc u_desc;
    u_desc.type = BodyType::Dynamic;
    u_desc.position = Vec3{-3, 4, 0};
    const JoltBody bracket = world.add_compound_body(u_desc, parts);
    NF_CHECK(bracket.valid());
    NF_CHECK(world.is_alive(bracket));

    // Control: the convex hull of the U's own 12 corners — the bounding box,
    // 3 wide and 2.25 tall, whose top is flat where the U has a mouth. The
    // corners are exactly the U's extent in y (0 .. 2.25), so the hull bounds
    // the same volume as the compound and both shapes put their lowest point at
    // local 0 — which is what makes the two rest-height assertions below assert
    // the same number rather than two numbers that happen to look alike.
    std::vector<Vec3> corners;
    for (const float sx : {-1.5f, 1.5f}) {
        for (const float sy : {0.0f, 2.25f}) {
            for (const float sz : {-0.5f, 0.5f}) {
                corners.push_back(Vec3{sx, sy, sz});
            }
        }
    }
    BodyDesc hull_desc;
    hull_desc.type = BodyType::Dynamic;
    hull_desc.position = Vec3{3, 4, 0};
    const JoltBody hull = world.add_convex_hull_body(hull_desc, corners);
    NF_CHECK(hull.valid());
    NF_CHECK(world.body_count() == 3); // the plane + the U + the hull

    step(world, 400);
    // Both rest on the floor and both have the SAME bounding box (the U's parts
    // span y = -0.25 .. 2.25, and the hull was built from exactly those
    // corners), so both report the same part-space origin height: the shapes
    // agree about where they sit. The disagreement is only about the groove.
    NF_CHECK_NEAR(world.state(bracket).position.y, 0.0f, 0.05f);
    NF_CHECK_NEAR(world.state(hull).position.y, 0.0f, 0.05f);
    NF_CHECK(std::abs(world.state(bracket).linear_velocity.y) < 0.05f);

    // The discriminator: a ball dropped into the U's mouth falls to the base
    // (top at 0.5) and parks at 0.5 + radius. The same ball dropped onto the
    // hull lands on the flat top (2.25) and parks at 2.25 + radius. An arm's
    // worth of difference, and only the concave collider can produce it.
    const float r = 0.2f;
    const JoltBody in_u = world.add_body(dynamic_sphere(Vec3{-3, 4, 0}, r));
    const JoltBody on_hull = world.add_body(dynamic_sphere(Vec3{3, 4, 0}, r));
    NF_CHECK(in_u.valid());
    NF_CHECK(on_hull.valid());
    step(world, 300);
    NF_CHECK_NEAR(world.state(in_u).position.y, 0.5f + r, 0.05f);
    NF_CHECK_NEAR(world.state(on_hull).position.y, 2.25f + r, 0.05f);
    NF_CHECK(world.state(on_hull).position.y > world.state(in_u).position.y + 1.0f);
}

NF_TEST(jolt_compound_body_is_dynamic_and_moves_as_one_rigid_body) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());

    // A table: one top and four legs, all one body. Dropped legs-down, it falls
    // until the legs' feet reach the floor. Built as four loose boxes instead,
    // the top would just drop to the ground beside them — nothing holds it up.
    std::vector<JoltWorld::CompoundPart> parts;
    JoltWorld::CompoundPart top;
    top.shape = JoltWorld::CompoundShape::make_box(Vec3{1.0f, 0.1f, 1.0f});
    top.position = Vec3{0, 1.0f, 0};
    parts.push_back(top);
    for (const float sx : {-0.9f, 0.9f}) {
        for (const float sz : {-0.9f, 0.9f}) {
            JoltWorld::CompoundPart leg;
            leg.shape = JoltWorld::CompoundShape::make_box(Vec3{0.1f, 0.9f, 0.1f});
            leg.position = Vec3{sx, 0.0f, sz};
            parts.push_back(leg);
        }
    }

    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.position = Vec3{0, 5, 0};
    const JoltBody table = world.add_compound_body(desc, parts);
    NF_CHECK(table.valid());
    NF_CHECK(world.is_alive(table));

    // Control: the top as a LOOSE body at the same world height. Nothing
    // supports it, so it falls all the way to the floor on its own. It is offset
    // in x on purpose: dropped at the table's own x it lands ON the tabletop
    // (which is what "the legs hold it up" means) and parks at ~1.9 instead of
    // 0.1, so the comparison would measure the tabletop, not the floor.
    BodyDesc loose_top;
    loose_top.type = BodyType::Dynamic;
    loose_top.position = desc.position + top.position + Vec3{4.0f, 0.0f, 0.0f};
    loose_top.shape = Shape::make_box(Vec3{1.0f, 0.1f, 1.0f});
    const JoltBody free_top = world.add_body(loose_top);
    NF_CHECK(free_top.valid());
    NF_CHECK(world.body_count() == 3); // plane + table + loose top

    step(world, 300);
    const float table_y = world.state(table).position.y;
    // The feet sit 0.9 below the origin, so the table rests at 0.9. It fell
    // under gravity (from 5) and it stopped, rather than hovering or sinking.
    NF_CHECK_NEAR(table_y, 0.9f, 0.05f);
    NF_CHECK(std::abs(world.state(table).linear_velocity.y) < 0.05f);
    // It did not topple: the top is still directly above the feet, not swung
    // off to one side.
    NF_CHECK_NEAR(world.state(table).position.x, 0.0f, 0.05f);

    // The compound's top is still 1.0 above its origin — held up by the legs —
    // while the loose top fell to 0.1. That gap is what "one rigid body" buys:
    // the legs carried the top down with them instead of leaving it behind.
    NF_CHECK_NEAR(world.state(table).position.y + 1.0f, 1.9f, 0.05f);
    NF_CHECK_NEAR(world.state(free_top).position.y, 0.1f, 0.05f);
    NF_CHECK(world.state(table).position.y > world.state(free_top).position.y + 0.5f);
}

NF_TEST(jolt_compound_is_queryable) {
    JoltWorld world = shape_world();
    std::vector<JoltWorld::CompoundPart> parts;
    JoltWorld::CompoundPart low;
    low.shape = JoltWorld::CompoundShape::make_box(Vec3{0.5f, 0.5f, 0.5f});
    low.position = Vec3{0, 0, 0};
    JoltWorld::CompoundPart high;
    high.shape = JoltWorld::CompoundShape::make_box(Vec3{0.5f, 0.5f, 0.5f});
    high.position = Vec3{0, 3, 0}; // a second storey, 2 m above the first
    parts.push_back(low);
    parts.push_back(high);

    BodyDesc desc;
    desc.type = BodyType::Static;
    desc.position = Vec3{0, 0, -5};
    const JoltBody compound = world.add_compound_body(desc, parts);
    NF_CHECK(compound.valid());

    // The upper box's near face is at z = -4.5, same as a single box would be —
    // a ray through the upper storey must hit THAT box and not the lower one.
    const JoltWorld::QueryHit ray = world.ray_cast(Vec3{0, 3, 0}, Vec3{0, 0, -1}, 10.0f);
    NF_CHECK(ray.hit);
    NF_CHECK(ray.body == compound);
    NF_CHECK_NEAR(ray.distance, 4.5f, 0.05f);
    NF_CHECK_NEAR(ray.normal.z, 1.0f, 1e-2f);

    // An overlap at the upper storey finds the compound; one between the boxes
    // (where there is nothing) does not.
    NF_CHECK(contains(world.overlap_sphere(Vec3{0, 3, -5}, 0.3f), compound));
    NF_CHECK(!contains(world.overlap_sphere(Vec3{0, 1.5f, -5}, 0.3f), compound));

    // Sweeping a sphere at the upper storey stops one radius short of the face.
    const JoltWorld::QueryHit sweep =
        world.sphere_cast(Vec3{0, 3, 0}, 0.25f, Vec3{0, 0, -1}, 10.0f);
    NF_CHECK(sweep.hit);
    NF_CHECK(sweep.body == compound);
    NF_CHECK_NEAR(sweep.distance, 4.25f, 0.05f);
}

NF_TEST(jolt_compound_invalid_inputs_are_safe) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    const usize base_count = world.body_count();
    BodyDesc desc;
    const std::vector<JoltWorld::CompoundPart> pair = {
        JoltWorld::CompoundPart{JoltWorld::CompoundShape::make_box(Vec3{0.5f, 0.5f, 0.5f}),
                                Vec3{0, 0, 0}, Quat::identity()},
        JoltWorld::CompoundPart{JoltWorld::CompoundShape::make_box(Vec3{0.5f, 0.5f, 0.5f}),
                                Vec3{1, 0, 0}, Quat::identity()},
    };

    // Fewer than two parts: not a compound, and Jolt's builder would error.
    NF_CHECK(!world.add_compound_body(desc, {}).valid());
    NF_CHECK(!world.add_compound_body(desc, {pair[0]}).valid());

    // One degenerate part refuses the WHOLE body. Part geometry is not clamped
    // the way the first-party Shape factories clamp theirs: a part reaches Jolt
    // alone, and Jolt's cylinder/capsule builders pass a negative radius through
    // unnormalised, so refusing is the only safe answer. `!(x > 0)` catches NaN
    // along with zero and the negatives.
    std::vector<JoltWorld::CompoundPart> bad = pair;
    const auto try_part = [&](const JoltWorld::CompoundShape& shape) {
        bad[0].shape = shape;
        return world.add_compound_body(desc, bad);
    };
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_sphere(0.0f)).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_sphere(-1.0f)).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_sphere(NAN)).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_box(Vec3{0.5f, 0.0f, 0.5f})).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_box(Vec3{-1.0f, 0.5f, 0.5f})).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_cylinder(0.5f, 0.0f)).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_cylinder(0.5f, -1.0f)).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_cylinder(NAN, 1.0f)).valid());
    NF_CHECK(!try_part(JoltWorld::CompoundShape::make_capsule(0.0f, 1.0f)).valid());
    NF_CHECK(world.body_count() == base_count); // none of them added a body

    // The one degenerate case that IS legal: a zero-height capsule is a sphere
    // to Jolt, so the part is accepted where the cylinder's zero-height disc is
    // not.
    const JoltBody degenerate_capsule =
        try_part(JoltWorld::CompoundShape::make_capsule(0.5f, 0.0f));
    NF_CHECK(degenerate_capsule.valid());
    NF_CHECK(world.body_count() == base_count + 1); // the only one that did
    world.remove_body(degenerate_capsule);
    NF_CHECK(world.body_count() == base_count);

    // A valid compound is an ordinary body: counted, alive, removable.
    const JoltBody ok = world.add_compound_body(desc, pair);
    NF_CHECK(ok.valid());
    NF_CHECK(world.is_alive(ok));
    NF_CHECK(world.body_count() == base_count + 1);
    world.remove_body(ok);
    NF_CHECK(!world.is_alive(ok));
    NF_CHECK(world.body_count() == base_count);
}

// A part keeps its own shape inside the compound. Both cases below give the
// compound and its box control IDENTICAL bounding boxes — a cylinder of radius
// r and half-height h, and a capsule of the same dimensions, each fit exactly
// inside a box of half-extents (r, h + r, r) — so nothing that reads extents can
// tell them apart. The difference is the curved part: both faces are circles
// inscribed in the control's square, and a ray through the square's corner
// passes the compound and hits the box. These are queries rather than drops
// because a body settling on a curved face is exactly the case a drop test
// cannot pin; that the compound still simulates as one body is the table test.
NF_TEST(jolt_compound_cylinder_part_is_round_not_a_box) {
    JoltWorld world = shape_world();

    // The cylinder part, plus a second part (a compound needs two) placed clear
    // of every ray path so only the cylinder is measured.
    std::vector<JoltWorld::CompoundPart> parts;
    JoltWorld::CompoundPart wheel;
    wheel.shape = JoltWorld::CompoundShape::make_cylinder(1.0f, 1.0f);
    wheel.position = Vec3{0, 0, 0};
    JoltWorld::CompoundPart axle;
    axle.shape = JoltWorld::CompoundShape::make_box(Vec3{1.0f, 1.0f, 1.0f});
    axle.position = Vec3{6, 0, 0};
    parts.push_back(wheel);
    parts.push_back(axle);
    BodyDesc desc;
    desc.type = BodyType::Static;
    const JoltBody compound = world.add_compound_body(desc, parts);
    NF_CHECK(compound.valid());

    // The control: the same two parts as boxes, offset in z out of the way.
    std::vector<JoltWorld::CompoundPart> box_parts;
    JoltWorld::CompoundPart box_a;
    box_a.shape = JoltWorld::CompoundShape::make_box(Vec3{1.0f, 1.0f, 1.0f});
    box_a.position = Vec3{0, 0, 0};
    JoltWorld::CompoundPart box_b;
    box_b.shape = JoltWorld::CompoundShape::make_box(Vec3{1.0f, 1.0f, 1.0f});
    box_b.position = Vec3{6, 0, 0};
    box_parts.push_back(box_a);
    box_parts.push_back(box_b);
    BodyDesc box_desc;
    box_desc.type = BodyType::Static;
    box_desc.position = Vec3{0, 0, -10};
    const JoltBody boxes = world.add_compound_body(box_desc, box_parts);
    NF_CHECK(boxes.valid());

    const Vec3 down{0, -1, 0};
    // (0.9, 0.9) is 1.272 from the axis: outside the unit circle the cylinder's
    // face is, inside the [-1, 1] square the box's face is.
    const float in_square_past_circle = 0.9f;

    // Down the axis both answer the same — the face is at the same height either
    // way, which is also what makes the two bounding boxes identical.
    const JoltWorld::QueryHit cyl_centre = world.ray_cast(Vec3{0, 10, 0}, down, 100.0f);
    const JoltWorld::QueryHit box_centre = world.ray_cast(Vec3{0, 10, -10}, down, 100.0f);
    NF_CHECK(cyl_centre.hit);
    NF_CHECK(box_centre.hit);
    NF_CHECK_NEAR(cyl_centre.distance, box_centre.distance, 0.01f);
    NF_CHECK_NEAR(cyl_centre.distance, 9.0f, 0.01f);

    // Through the corner: the box control's square face is there, the cylinder
    // part's circular face is not.
    const JoltWorld::QueryHit cyl_corner =
        world.ray_cast(Vec3{in_square_past_circle, 10.0f, in_square_past_circle}, down, 100.0f);
    const JoltWorld::QueryHit box_corner =
        world.ray_cast(Vec3{in_square_past_circle, 10.0f, -10.0f + in_square_past_circle},
                       down, 100.0f);
    NF_CHECK(!cyl_corner.hit);
    NF_CHECK(box_corner.hit);
    NF_CHECK_NEAR(box_corner.distance, 9.0f, 0.01f);
}

NF_TEST(jolt_compound_capsule_part_is_round_not_a_box) {
    JoltWorld world = shape_world();

    // A capsule of radius 1 and half-height 1 spans y = -2 .. 2, so its bounding
    // box is exactly a box of half-extents (1, 2, 1) — the control beside it.
    std::vector<JoltWorld::CompoundPart> parts;
    JoltWorld::CompoundPart capsule;
    capsule.shape = JoltWorld::CompoundShape::make_capsule(1.0f, 1.0f);
    capsule.position = Vec3{0, 0, 0};
    JoltWorld::CompoundPart other;
    other.shape = JoltWorld::CompoundShape::make_box(Vec3{1.0f, 1.0f, 1.0f});
    other.position = Vec3{6, 0, 0};
    parts.push_back(capsule);
    parts.push_back(other);
    BodyDesc desc;
    desc.type = BodyType::Static;
    const JoltBody compound = world.add_compound_body(desc, parts);
    NF_CHECK(compound.valid());

    std::vector<JoltWorld::CompoundPart> box_parts;
    JoltWorld::CompoundPart tall;
    tall.shape = JoltWorld::CompoundShape::make_box(Vec3{1.0f, 2.0f, 1.0f});
    tall.position = Vec3{0, 0, 0};
    JoltWorld::CompoundPart wide;
    wide.shape = JoltWorld::CompoundShape::make_box(Vec3{1.0f, 1.0f, 1.0f});
    wide.position = Vec3{6, 0, 0};
    box_parts.push_back(tall);
    box_parts.push_back(wide);
    BodyDesc box_desc;
    box_desc.type = BodyType::Static;
    box_desc.position = Vec3{0, 0, -10};
    const JoltBody boxes = world.add_compound_body(box_desc, box_parts);
    NF_CHECK(boxes.valid());

    const Vec3 down{0, -1, 0};
    const float in_square_past_circle = 0.9f;

    // Down the axis: the capsule's dome peak and the box's flat top are both at
    // y = 2 — the same bounding box, so the same distance.
    const JoltWorld::QueryHit cap_top = world.ray_cast(Vec3{0, 10, 0}, down, 100.0f);
    const JoltWorld::QueryHit box_top = world.ray_cast(Vec3{0, 10, -10}, down, 100.0f);
    NF_CHECK(cap_top.hit);
    NF_CHECK(box_top.hit);
    NF_CHECK_NEAR(cap_top.distance, box_top.distance, 0.01f);
    NF_CHECK_NEAR(cap_top.distance, 8.0f, 0.01f);

    // Through the corner of the square: the box's flat face is there; the
    // capsule's dome is a circle of radius 1, and 1.272 from the axis is past it.
    NF_CHECK(!world.ray_cast(Vec3{in_square_past_circle, 10.0f, in_square_past_circle},
                             down, 100.0f).hit);
    NF_CHECK(world.ray_cast(Vec3{in_square_past_circle, 10.0f, -10.0f + in_square_past_circle},
                            down, 100.0f).hit);
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

// --- Cylinder ---------------------------------------------------------------

/// A static cylinder whose flat face is up, and a static box with the same
/// half-extents beside it. The two shapes' axis-aligned bounding boxes are
/// identical, so nothing that looks at extents alone can tell them apart — the
/// difference is that the cylinder's face is a CIRCLE inscribed in the box's
/// square. Anything dropped past the circle but inside the square falls through
/// one and lands on the other, which is what the roundness tests below assert.
namespace {

constexpr float kCylRadius = 1.0f;
constexpr float kCylHalfHeight = 1.0f;
constexpr float kCylBallRadius = 0.2f;
constexpr float kCylBoxX = 8.0f;

/// (0.9, 0.9) is 1.272 from the axis: outside the unit circle the cylinder's
/// face is, inside the [-1, 1] square the box's face is.
constexpr float kInSquarePastCircle = 0.9f;

void add_cylinder_and_box(JoltWorld& world) {
    BodyDesc cylinder_desc;
    cylinder_desc.type = BodyType::Static;
    cylinder_desc.position = Vec3{0, 0, 0};
    NF_CHECK(world.add_cylinder_body(cylinder_desc, kCylRadius, kCylHalfHeight).valid());
    world.add_body(static_box(Vec3{kCylBoxX, 0, 0},
                             Vec3{kCylRadius, kCylHalfHeight, kCylRadius}));
}

} // namespace

NF_TEST(jolt_cylinder_body_rests_on_its_flat_end) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());

    // A dynamic cylinder and a dynamic box with the same half-extents, dropped
    // side by side. The cylinder's lowest local point is -half_height, exactly
    // where the box's is, so both origins sit the same height off the floor —
    // the two assertions below are the same number on purpose. A collider that
    // silently rounded the wrong way (a sphere of this radius would rest at
    // kCylRadius, a capsule at half_height + radius) would not.
    BodyDesc cylinder_desc;
    cylinder_desc.type = BodyType::Dynamic;
    cylinder_desc.position = Vec3{-4, 5, 0};
    const JoltBody cylinder =
        world.add_cylinder_body(cylinder_desc, kCylRadius, kCylHalfHeight);
    const JoltBody box = world.add_body(dynamic_box(Vec3{4, 5, 0},
                                                   Vec3{kCylRadius, kCylHalfHeight, kCylRadius}));
    NF_CHECK(cylinder.valid());
    NF_CHECK(box.valid());
    NF_CHECK(world.body_count() == 3); // the plane + the cylinder + the box

    step(world, 400);
    NF_CHECK_NEAR(world.state(cylinder).position.y, kCylHalfHeight, 0.05f);
    NF_CHECK_NEAR(world.state(box).position.y, kCylHalfHeight, 0.05f);
    NF_CHECK(std::abs(world.state(cylinder).linear_velocity.y) < 0.05f);
}

NF_TEST(jolt_cylinder_is_round_not_a_box) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    add_cylinder_and_box(world);

    // Dead centre of the flat face: the face is there for both shapes, so the
    // ball lands on top of either one.
    const JoltBody centre_on_cylinder =
        world.add_body(dynamic_sphere(Vec3{0, 3, 0}, kCylBallRadius));
    // The square's corner: the box has material there, the cylinder does not.
    const JoltBody corner_on_cylinder =
        world.add_body(dynamic_sphere(Vec3{kInSquarePastCircle, 3, kInSquarePastCircle},
                                      kCylBallRadius));
    const JoltBody corner_on_box =
        world.add_body(dynamic_sphere(Vec3{kCylBoxX + kInSquarePastCircle, 3,
                                           kInSquarePastCircle},
                                      kCylBallRadius));
    step(world, 500);

    // The ball on the face parks at face height + radius for both shapes; the
    // ball through the corner falls to the floor.
    NF_CHECK_NEAR(world.state(centre_on_cylinder).position.y,
                  kCylHalfHeight + kCylBallRadius, 0.05f);
    NF_CHECK_NEAR(world.state(corner_on_box).position.y,
                  kCylHalfHeight + kCylBallRadius, 0.05f);
    NF_CHECK_NEAR(world.state(corner_on_cylinder).position.y, kCylBallRadius, 0.05f);
    // The one ball sits a full half-height above the other: the cylinder's face
    // is missing where the box's face is, and that is the whole difference
    // between the two colliders.
    NF_CHECK_NEAR(world.state(corner_on_box).position.y -
                      world.state(corner_on_cylinder).position.y,
                  kCylHalfHeight, 0.1f);
}

NF_TEST(jolt_cylinder_is_queryable) {
    JoltWorld world;
    add_cylinder_and_box(world);

    const Vec3 down{0, -1, 0};

    // Down the axis: both faces are at the same height, so both rays travel the
    // same distance to reach them.
    const JoltWorld::QueryHit cyl_centre =
        world.ray_cast(Vec3{0, 10, 0}, down, 100.0f);
    const JoltWorld::QueryHit box_centre =
        world.ray_cast(Vec3{kCylBoxX, 10, 0}, down, 100.0f);
    NF_CHECK(cyl_centre.hit);
    NF_CHECK(box_centre.hit);
    NF_CHECK_NEAR(cyl_centre.distance, box_centre.distance, 0.01f);
    NF_CHECK_NEAR(cyl_centre.distance, 10.0f - kCylHalfHeight, 0.01f);
    NF_CHECK(cyl_centre.body.valid());

    // The same ray aimed through the square's corner hits the box and passes
    // the cylinder clean through the gap its circle leaves.
    const JoltWorld::QueryHit cyl_corner =
        world.ray_cast(Vec3{kInSquarePastCircle, 10, kInSquarePastCircle}, down, 100.0f);
    const JoltWorld::QueryHit box_corner =
        world.ray_cast(Vec3{kCylBoxX + kInSquarePastCircle, 10, kInSquarePastCircle},
                       down, 100.0f);
    NF_CHECK(!cyl_corner.hit);
    NF_CHECK(box_corner.hit);
    NF_CHECK_NEAR(box_corner.distance, 10.0f - kCylHalfHeight, 0.01f);

    // A swept sphere stops its centre one radius short of the face — a cast
    // that reaches the cylinder down its axis and one aimed at the corner.
    const JoltWorld::QueryHit cast_centre =
        world.sphere_cast(Vec3{0, 10, 0}, kCylBallRadius, down, 100.0f);
    NF_CHECK(cast_centre.hit);
    NF_CHECK_NEAR(cast_centre.distance,
                  10.0f - kCylHalfHeight - kCylBallRadius, 0.01f);
    NF_CHECK(!world.sphere_cast(Vec3{kInSquarePastCircle, 10, kInSquarePastCircle},
                                kCylBallRadius, down, 100.0f).hit);
}

NF_TEST(jolt_cylinder_invalid_inputs_are_safe) {
    JoltWorld world;
    NF_CHECK(world.add_body(static_plane()).valid());
    const usize base_count = world.body_count();

    BodyDesc desc;
    desc.type = BodyType::Static;
    // `!(x > 0)` catches NaN as well as zero and the negatives: a NaN radius
    // would otherwise reach Jolt's assert-free constructor as-is.
    NF_CHECK(!world.add_cylinder_body(desc, 0.0f, kCylHalfHeight).valid());
    NF_CHECK(!world.add_cylinder_body(desc, -kCylRadius, kCylHalfHeight).valid());
    NF_CHECK(!world.add_cylinder_body(desc, NAN, kCylHalfHeight).valid());
    NF_CHECK(!world.add_cylinder_body(desc, kCylRadius, 0.0f).valid());
    NF_CHECK(!world.add_cylinder_body(desc, kCylRadius, -kCylHalfHeight).valid());
    NF_CHECK(!world.add_cylinder_body(desc, kCylRadius, NAN).valid());

    // Nothing was added while rejecting all of the above.
    NF_CHECK(world.body_count() == base_count);
}

