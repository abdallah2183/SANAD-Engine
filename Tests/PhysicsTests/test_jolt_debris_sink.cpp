// PhysicsTests — the Jolt-backed IDebrisSink (design doc Section 41: debris).
//
// NFDestruction's own suite verifies the budget, the stress model and the shard
// kinematics against a NullDebrisSink, which is arithmetic. This file answers
// the question that suite cannot: does the sink that ships actually turn a
// shard into a body, give it the velocities the fracture math computed, and
// take the body away again when the budget retires it?
//
// Every case is a tiny headless Jolt world with a fixed dt — no rendering, no
// threads — and the numbers compared are closed form or exact identity, the
// same standard as the rest of the Jolt suites.

#include <NF/Core/Math.hpp>
#include <NF/Destruction/DebrisSink.hpp>
#include <NF/Destruction/DestructionWorld.hpp>
#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Destruction/FractureMath.hpp>
#include <NF/Physics/JoltDebrisSink.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::destruction;
using namespace nf::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;

/// A cube of side 2 centred at the origin: volume 8, six faces of area 4.
FracturePiece unit_cube() {
    FracturePiece p;
    p.vertices = {
        Vec3{-1.0f, -1.0f, -1.0f}, Vec3{ 1.0f, -1.0f, -1.0f},
        Vec3{ 1.0f,  1.0f, -1.0f}, Vec3{-1.0f,  1.0f, -1.0f},
        Vec3{-1.0f, -1.0f,  1.0f}, Vec3{ 1.0f, -1.0f,  1.0f},
        Vec3{ 1.0f,  1.0f,  1.0f}, Vec3{-1.0f,  1.0f,  1.0f},
    };
    p.indices = {
        0u, 2u, 1u,  0u, 3u, 2u,   1u, 6u, 5u,  1u, 2u, 6u,
        5u, 7u, 4u,  5u, 6u, 7u,   4u, 3u, 0u,  4u, 7u, 3u,
        4u, 0u, 1u,  4u, 1u, 5u,   3u, 6u, 2u,  3u, 7u, 6u,
    };
    return p;
}

/// A cube of side 1 centred at `c` — one shard's worth of convex solid.
FracturePiece small_cube(Vec3 c) {
    FracturePiece p;
    p.vertices = {
        Vec3{c.x - 0.5f, c.y - 0.5f, c.z - 0.5f},
        Vec3{c.x + 0.5f, c.y - 0.5f, c.z - 0.5f},
        Vec3{c.x + 0.5f, c.y + 0.5f, c.z - 0.5f},
        Vec3{c.x - 0.5f, c.y + 0.5f, c.z - 0.5f},
        Vec3{c.x - 0.5f, c.y - 0.5f, c.z + 0.5f},
        Vec3{c.x + 0.5f, c.y - 0.5f, c.z + 0.5f},
        Vec3{c.x + 0.5f, c.y + 0.5f, c.z + 0.5f},
        Vec3{c.x - 0.5f, c.y + 0.5f, c.z + 0.5f},
    };
    p.indices = {
        0u, 2u, 1u,  0u, 3u, 2u,   1u, 6u, 5u,  1u, 2u, 6u,
        5u, 7u, 4u,  5u, 6u, 7u,   4u, 3u, 0u,  4u, 7u, 3u,
        4u, 0u, 1u,  4u, 1u, 5u,   3u, 6u, 2u,  3u, 7u, 6u,
    };
    return p;
}

/// A real fracture asset: convex leaves, bonds weak enough that one strong
/// blast at the centre takes them in a single call.
FractureAsset four_piece_asset() {
    FractureParams params;
    params.seed = 0x5EEDBEEFu;
    params.target_chunks = 4u;
    params.strength_per_area = 1.0f;
    FractureAsset asset;
    build_fracture_asset(unit_cube(), params, asset);
    return asset;
}

/// A hand-built asset whose break order is fixed by the bond strengths, not by
/// the builder's tree shape: the inner bond (weak) goes first and releases one
/// shard, the outer bond (strong) goes second and releases another. That is the
/// only shape in which the budget's retirement path can be asserted exactly —
/// with a generated tree, which bonds break in which call depends on the split.
///
/// chunk 0 = internal (children 1 and 4); chunk 1 = internal (children 2 and 3);
/// chunks 2, 3, 4 = leaves. bond 0 = chunk 0's split, bond 1 = chunk 1's split.
FractureAsset two_shard_asset() {
    FractureAsset asset;
    asset.chunks.resize(5u);
    asset.bonds.resize(2u);

    asset.chunks[0u] = FractureChunk{FracturePiece{}, Vec3{0.0f, 0.0f, 0.0f}, 3.0f,
                                    kInvalidChunk, {1u, 4u}, 0u};
    asset.chunks[1u] = FractureChunk{FracturePiece{}, Vec3{-1.0f, 0.0f, 0.0f}, 2.0f,
                                    0u, {2u, 3u}, 1u};
    asset.chunks[2u] = FractureChunk{small_cube(Vec3{-1.5f, 0.0f, 0.0f}),
                                    Vec3{-1.5f, 0.0f, 0.0f}, 1.0f, 1u,
                                    {kInvalidChunk, kInvalidChunk}, 2u};
    asset.chunks[3u] = FractureChunk{small_cube(Vec3{-0.5f, 0.0f, 0.0f}),
                                    Vec3{-0.5f, 0.0f, 0.0f}, 1.0f, 1u,
                                    {kInvalidChunk, kInvalidChunk}, 2u};
    asset.chunks[4u] = FractureChunk{small_cube(Vec3{1.0f, 0.0f, 0.0f}),
                                    Vec3{1.0f, 0.0f, 0.0f}, 1.0f, 0u,
                                    {kInvalidChunk, kInvalidChunk}, 1u};

    // The outer bond is strong, the inner bond weak: a blast that breaks only
    // the inner one, followed by a blast that breaks the outer, releases the
    // two shards one at a time — which is the sequence the budget sees.
    asset.bonds[0u] = FractureBond{0u, 1u, 4u, Vec3{1.0f, 0.0f, 0.0f}, -1.0f, 1.0f,
                                  Vec3{0.0f, 0.0f, 0.0f}, 100.0f};
    asset.bonds[1u] = FractureBond{1u, 2u, 3u, Vec3{1.0f, 0.0f, 0.0f}, -0.5f, 1.0f,
                                  Vec3{-1.0f, 0.0f, 0.0f}, 1.0f};
    return asset;
}

DamageEvent blast_at(Vec3 point, float radius, float impulse) {
    DamageEvent event;
    event.world_point = point;
    event.radius = radius;
    event.impulse = impulse;
    return event;
}

/// The convex hull points of one leaf — the shape a shard becomes.
std::vector<Vec3> leaf_hull(const FractureAsset& asset, u32 chunk_id) {
    return asset.chunks[chunk_id].piece.vertices;
}

/// Angle between two quaternions, in radians (0 = identical orientations).
float quat_angle(const Quat& a, const Quat& b) {
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float c = dot < 0.0f ? -dot : dot;   // double cover
    return 2.0f * acosf(c < 1.0f ? c : 1.0f);
}

} // namespace

// =========================================================================
// One shard: shape, pose, mass, velocities
// =========================================================================

NF_TEST(jolt_debris_sink_spawn_makes_one_dynamic_body) {
    const FractureAsset asset = four_piece_asset();
    NF_CHECK(asset.leaf_count() >= 2u);

    JoltWorld world;
    NF_CHECK(world.valid());
    const usize before = world.body_count();

    JoltDebrisSink sink(world);

    DebrisSpawn spawn;
    spawn.chunk_id = 1u;
    spawn.position = Vec3{0.0f, 10.0f, 0.0f};
    spawn.mass = 2.0f;
    spawn.hull_points = leaf_hull(asset, 1u);

    const u32 id = sink.spawn(spawn);
    NF_CHECK(id != kInvalidDebris);

    NF_CHECK(sink.active_count() == 1u);
    NF_CHECK(world.body_count() == before + 1u);

    const JoltBody body = sink.body_of(id);
    NF_CHECK(body.valid());
    NF_CHECK(world.is_alive(body));

    // The pose the spawn asked for, and no velocity yet. A generated fracture
    // chunk's hull is 14 points whose local centre of mass is well off the
    // origin, so asserting on all three axes is also the regression guard for
    // the Jolt trap behind state(): reporting the COM position rather than the
    // shape-local origin would send this back offset by that COM.
    const JoltBodyState state = world.state(body);
    NF_CHECK_NEAR(state.position.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(state.position.y, 10.0f, 1e-4f);
    NF_CHECK_NEAR(state.position.z, 0.0f, 1e-4f);
    NF_CHECK_NEAR(state.linear_velocity.length_sq(), 0.0f, 1e-6f);
}

NF_TEST(jolt_debris_sink_applies_the_linear_velocity) {
    // Zero gravity and zero damping, so the shard drifts in a straight line at
    // a constant speed and the closed form is position = v * t exactly.
    PhysicsSettings settings;
    settings.gravity = Vec3{0.0f, 0.0f, 0.0f};
    JoltWorld drift(settings);
    NF_CHECK(drift.valid());
    JoltDebrisSink sink(drift, DebrisSinkConfig{0.6f, 0.05f, 0.0f, 0.0f, true});

    DebrisSpawn spawn;
    spawn.position = Vec3{0.0f, 0.0f, 0.0f};
    spawn.linear_velocity = Vec3{2.0f, 0.0f, 0.0f};
    spawn.mass = 1.0f;
    spawn.hull_points = leaf_hull(four_piece_asset(), 1u);

    const u32 id = sink.spawn(spawn);
    NF_CHECK(id != kInvalidDebris);

    for (u32 i = 0u; i < 30u; ++i) drift.step(kDt);

    // 30 steps of 1/60 s at 2 m/s = 1 m along +x.
    const JoltBodyState state = drift.state(sink.body_of(id));
    NF_CHECK_NEAR(state.position.x, 1.0f, 1e-3f);
    NF_CHECK_NEAR(state.linear_velocity.x, 2.0f, 1e-3f);
}

NF_TEST(jolt_debris_sink_applies_the_angular_velocity) {
    // set_angular_velocity has no read-back in JoltBodyState by design (a pose
    // is one atomic fact; angular velocity is not part of it), so this case
    // verifies the setter by its effect. Two identical shards, one spinning,
    // one not: after stepping, only the spinning one has turned. Gravity acts
    // through a uniform hull's centre of mass, so the still shard keeps its
    // orientation as it falls.
    JoltWorld world;
    JoltDebrisSink sink(world, DebrisSinkConfig{0.6f, 0.05f, 0.0f, 0.0f, true});

    const std::vector<Vec3> hull = leaf_hull(four_piece_asset(), 1u);

    DebrisSpawn spinning;
    spinning.position = Vec3{-3.0f, 5.0f, 0.0f};
    spinning.angular_velocity = Vec3{0.0f, 4.0f, 0.0f};
    spinning.mass = 1.0f;
    spinning.hull_points = hull;

    DebrisSpawn still;
    still.position = Vec3{3.0f, 5.0f, 0.0f};
    still.mass = 1.0f;
    still.hull_points = hull;

    const u32 spin_id = sink.spawn(spinning);
    const u32 still_id = sink.spawn(still);
    NF_CHECK(spin_id != kInvalidDebris);
    NF_CHECK(still_id != kInvalidDebris);

    for (u32 i = 0u; i < 60u; ++i) world.step(kDt);   // 1 s

    const Quat spin_rot = world.state(sink.body_of(spin_id)).rotation;
    const Quat still_rot = world.state(sink.body_of(still_id)).rotation;

    // 4 rad/s for 1 s is well over a full turn; comparing the two orientations
    // is the claim, and it does not depend on which way the spin happened to
    // land. A rotation purely about y leaves x and z at zero, so asserting on
    // those components alone would pass for a spinning shard too.
    NF_CHECK(quat_angle(spin_rot, still_rot) > 0.5f);
    NF_CHECK(quat_angle(still_rot, Quat::identity()) < 1e-2f);
}

NF_TEST(jolt_debris_sink_drops_shards_that_are_not_hulls) {
    JoltWorld world;
    const usize before = world.body_count();
    JoltDebrisSink sink(world);

    DebrisSpawn spawn;
    spawn.mass = 1.0f;
    spawn.hull_points = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}; // a triangle

    NF_CHECK(sink.spawn(spawn) == kInvalidDebris);
    NF_CHECK(sink.active_count() == 0u);
    // No body was created and left orphaned in the world.
    NF_CHECK(world.body_count() == before);
}

NF_TEST(jolt_debris_sink_drops_shards_with_non_positive_mass) {
    JoltWorld world;
    const usize before = world.body_count();
    JoltDebrisSink sink(world);

    DebrisSpawn spawn;
    spawn.mass = 0.0f;
    spawn.hull_points = leaf_hull(four_piece_asset(), 1u);

    NF_CHECK(sink.spawn(spawn) == kInvalidDebris);
    NF_CHECK(world.body_count() == before);
}

NF_TEST(jolt_debris_sink_shards_rest_on_the_ground) {
    // The point of a real backend: the shard does not fall through the floor.
    JoltWorld world;
    JoltDebrisSink sink(world);

    BodyDesc ground;
    ground.type = BodyType::Static;
    ground.shape = Shape::make_plane(Vec3{0.0f, 1.0f, 0.0f});
    ground.position = Vec3{0.0f, -2.0f, 0.0f};
    NF_CHECK(world.add_body(ground).valid());

    DebrisSpawn spawn;
    spawn.position = Vec3{0.0f, 10.0f, 0.0f};
    spawn.mass = 1.0f;
    spawn.hull_points = leaf_hull(four_piece_asset(), 1u);
    const u32 id = sink.spawn(spawn);
    NF_CHECK(id != kInvalidDebris);

    for (u32 i = 0u; i < 120u; ++i) world.step(kDt);

    // Fell, and stopped above the plane instead of passing through it.
    const JoltBodyState state = world.state(sink.body_of(id));
    NF_CHECK(state.position.y < 10.0f);
    NF_CHECK(state.position.y > -2.0f);
}

// =========================================================================
// Retirement: the budget must actually free the physics body
// =========================================================================

NF_TEST(jolt_debris_sink_destroy_removes_the_body) {
    JoltWorld world;
    const usize before = world.body_count();
    JoltDebrisSink sink(world);

    DebrisSpawn spawn;
    spawn.mass = 1.0f;
    spawn.hull_points = leaf_hull(four_piece_asset(), 1u);
    const u32 id = sink.spawn(spawn);
    const JoltBody body = sink.body_of(id);
    NF_CHECK(world.is_alive(body));

    sink.destroy(id);
    NF_CHECK(sink.active_count() == 0u);
    NF_CHECK(!world.is_alive(body));
    NF_CHECK(world.body_count() == before);

    // Idempotent: retiring an unknown or already-retired id touches nothing.
    sink.destroy(id);
    sink.destroy(999u);
    NF_CHECK(world.body_count() == before);
}

NF_TEST(jolt_debris_sink_clear_empties_the_world) {
    JoltWorld world;
    const usize before = world.body_count();
    JoltDebrisSink sink(world);

    const std::vector<Vec3> hull = leaf_hull(four_piece_asset(), 1u);
    for (u32 i = 0u; i < 4u; ++i) {
        DebrisSpawn spawn;
        spawn.position = Vec3{static_cast<f32>(i) * 4.0f, 5.0f, 0.0f};
        spawn.mass = 1.0f;
        spawn.hull_points = hull;
        NF_CHECK(sink.spawn(spawn) != kInvalidDebris);
    }
    NF_CHECK(sink.active_count() == 4u);

    sink.clear();
    NF_CHECK(sink.active_count() == 0u);
    NF_CHECK(world.body_count() == before);
}

// =========================================================================
// Integration with DestructionWorld — the seam, end to end
// =========================================================================

NF_TEST(jolt_debris_sink_world_and_sink_agree_on_the_live_count) {
    const FractureAsset asset = four_piece_asset();

    JoltWorld world;
    JoltDebrisSink sink(world);
    DestructionWorld destruction(sink);

    DestructibleComponent cmp;
    cmp.density = 1.0f;

    const u32 broken = destruction.apply_damage(asset, cmp, Mat4::identity(),
                                                blast_at(Vec3::zero, 10.0f, 1e6f));
    NF_CHECK(broken > 0u);

    // Every shard the world believes it emitted is a real Jolt body, and vice
    // versa — these are the same count, measured three independent ways.
    NF_CHECK(destruction.active_debris() == sink.active_count());
    NF_CHECK(sink.active_count() == cmp.emitted_count());
    NF_CHECK(world.body_count() == sink.active_count());
}

NF_TEST(jolt_debris_sink_shards_fall_under_gravity) {
    // Rule 0: a green unit suite says nothing about the running system. The
    // sink's bookkeeping can be perfect while every shard sits at the blast
    // point forever, so this steps the world and asserts the shards moved.
    const FractureAsset asset = four_piece_asset();

    JoltWorld world;
    JoltDebrisSink sink(world);
    DestructionWorld destruction(sink);

    DestructibleComponent cmp;
    destruction.apply_damage(asset, cmp, Mat4::identity(),
                             blast_at(Vec3{0.0f, 20.0f, 0.0f}, 40.0f, 1e6f));
    NF_CHECK(sink.active_count() > 0u);

    const std::vector<u32> ids = sink.live_ids();
    std::vector<Vec3> origins;
    for (const u32 id : ids) {
        origins.push_back(world.state(sink.body_of(id)).position);
    }

    for (u32 i = 0u; i < 60u; ++i) world.step(kDt);

    for (std::size_t i = 0u; i < ids.size(); ++i) {
        // Gravity is -9.81 along y; in one second nothing here falls back up.
        NF_CHECK(world.state(sink.body_of(ids[i])).position.y < origins[i].y + 1e-3f);
    }
}

NF_TEST(jolt_debris_sink_budget_retirement_frees_the_physics_body) {
    // This is the case a NullDebrisSink cannot catch: a budget that retires a
    // shard in its own bookkeeping but leaves the Jolt body behind leaks one
    // collider per shard forever, and the frame cost is invisible to the
    // destruction world's own counters. The hand-built asset fixes the break
    // order so the sequence is known exactly rather than inferred.
    const FractureAsset asset = two_shard_asset();

    JoltWorld world;
    const usize before = world.body_count();
    JoltDebrisSink sink(world);

    DestructionBudget budget;
    budget.max_active_debris = 1u;
    budget.max_breaks_per_frame = 16u;
    DestructionWorld destruction(sink, budget);

    DestructibleComponent cmp;

    // Blast one: only the weak inner bond goes (impulse 50 < the outer bond's
    // 100, and far above the inner bond's 1). One shard, under the cap.
    destruction.apply_damage(asset, cmp, Mat4::identity(),
                             blast_at(Vec3{-1.0f, 0.0f, 0.0f}, 10.0f, 50.0f));
    NF_CHECK(sink.active_count() == 1u);
    const std::vector<u32> first_ids = sink.live_ids();
    NF_CHECK(first_ids.size() == 1u);
    const JoltBody first_body = sink.body_of(first_ids[0u]);
    NF_CHECK(world.is_alive(first_body));

    // Blast two: the strong outer bond goes. The new shard would exceed the cap,
    // so the first shard is retired — its Jolt body must actually be gone.
    destruction.tick(kDt);   // frees the frame's break allowance
    destruction.apply_damage(asset, cmp, Mat4::identity(),
                             blast_at(Vec3{-1.0f, 0.0f, 0.0f}, 10.0f, 200.0f));
    NF_CHECK(sink.active_count() == 1u);   // held at the cap, not above it
    NF_CHECK(!world.is_alive(first_body)); // the physics body was freed too
    NF_CHECK(world.body_count() == before + 1u);
    NF_CHECK(cmp.emitted_count() == 2u);   // both shards were spawned
}

NF_TEST(jolt_debris_sink_lifetime_retirement_frees_the_physics_body) {
    // The other retirement path: age, not the cap. tick() must reach into the
    // sink and free the body, not just drop the record.
    const FractureAsset asset = two_shard_asset();

    JoltWorld world;
    const usize before = world.body_count();
    JoltDebrisSink sink(world);

    DestructionBudget budget;
    budget.max_active_debris = 8u;          // not under cap pressure
    budget.max_breaks_per_frame = 16u;
    budget.max_debris_lifetime = 1.0f;
    DestructionWorld destruction(sink, budget);

    DestructibleComponent cmp;
    destruction.apply_damage(asset, cmp, Mat4::identity(),
                             blast_at(Vec3{-1.0f, 0.0f, 0.0f}, 10.0f, 50.0f));
    const std::vector<u32> ids = sink.live_ids();
    NF_CHECK(ids.size() == 1u);
    const JoltBody body = sink.body_of(ids[0u]);
    NF_CHECK(world.is_alive(body));

    destruction.tick(2.0f);   // past the lifetime in one tick

    NF_CHECK(sink.active_count() == 0u);
    NF_CHECK(!world.is_alive(body));
    NF_CHECK(world.body_count() == before);
}

NF_TEST(jolt_debris_sink_same_spawns_give_the_same_trajectory) {
    // Determinism (Section 372): two independent sinks given the same spawns in
    // the same order, stepped with the same fixed dt, land in the same place.
    const std::vector<Vec3> hull = leaf_hull(four_piece_asset(), 1u);

    PhysicsSettings settings;
    settings.gravity = Vec3{0.0f, 0.0f, 0.0f};

    JoltWorld a(settings);
    JoltWorld b(settings);
    JoltDebrisSink sink_a(a);
    JoltDebrisSink sink_b(b);

    std::vector<u32> ids_a;
    std::vector<u32> ids_b;
    for (u32 i = 0u; i < 4u; ++i) {
        DebrisSpawn spawn;
        spawn.position = Vec3{static_cast<f32>(i) * 3.0f, 0.0f, 0.0f};
        spawn.linear_velocity = Vec3{0.0f, 1.5f, 0.2f};
        spawn.angular_velocity = Vec3{0.0f, 0.7f, 0.0f};
        spawn.mass = 1.5f;
        spawn.hull_points = hull;

        const u32 id_a = sink_a.spawn(spawn);
        const u32 id_b = sink_b.spawn(spawn);
        NF_CHECK(id_a != kInvalidDebris);
        NF_CHECK(id_a == id_b);   // ids are handed out in the same order
        ids_a.push_back(id_a);
        ids_b.push_back(id_b);
    }

    for (u32 step = 0u; step < 30u; ++step) {
        a.step(kDt);
        b.step(kDt);
    }

    for (std::size_t i = 0u; i < ids_a.size(); ++i) {
        const JoltBodyState sa = a.state(sink_a.body_of(ids_a[i]));
        const JoltBodyState sb = b.state(sink_b.body_of(ids_b[i]));
        NF_CHECK_NEAR(sa.position.x, sb.position.x, 1e-4f);
        NF_CHECK_NEAR(sa.position.y, sb.position.y, 1e-4f);
        NF_CHECK_NEAR(sa.position.z, sb.position.z, 1e-4f);
    }
}
