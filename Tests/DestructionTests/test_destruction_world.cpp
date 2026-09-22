// DestructionTests — damage, bonds, debris budgets, shard kinematics.
// (design doc Section 41: "Breakable meshes / Fracture assets / Runtime
// destruction" — the runtime half; test_fracture.cpp covers the geometry half.)
//
// The world is arithmetic behind an IDebrisSink seam, so every number here is
// closed form and no physics backend is involved: a point-bank blast delivers
// the full impulse, a shard's mass is its chunk volume times the component
// density, and the budget is a cap you can hit with two crates and a for loop.
// What these tests buy is the two invariants the runtime is organised around —
// a shattered region is emitted exactly once, and the frame's break allowance
// is shared across the whole world rather than per object.

#include <NF/Core/Math.hpp>
#include <NF/Destruction/DebrisSink.hpp>
#include <NF/Destruction/DestructibleComponent.hpp>
#include <NF/Destruction/DestructionWorld.hpp>
#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Destruction/FractureMaterial.hpp>
#include <NF/Test/TestFramework.hpp>

#include <vector>

using namespace nf;
using namespace nf::destruction;

namespace {

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
        0u, 2u, 1u,  0u, 3u, 2u,   // -z
        1u, 6u, 5u,  1u, 2u, 6u,   // +x
        5u, 7u, 4u,  5u, 6u, 7u,   // +z
        4u, 3u, 0u,  4u, 7u, 3u,   // -x
        4u, 0u, 1u,  4u, 1u, 5u,   // -y
        3u, 6u, 2u,  3u, 7u, 6u,   // +y
    };
    return p;
}

/// One bond, two leaves: the smallest object that can come apart.
FractureAsset two_piece_asset() {
    FractureParams params;
    params.target_chunks = 2u;
    params.strength_per_area = 10.0f;

    FractureAsset asset;
    std::string error;
    build_fracture_asset(unit_cube(), params, asset, &error);
    NF_CHECK(error.empty());
    NF_CHECK(asset.bonds.size() == 1u);
    return asset;
}

/// A blast centred on `point` strong enough to shatter any bond it reaches.
DamageEvent blast_at(const Vec3& point, f32 radius, f32 impulse) {
    DamageEvent event;
    event.world_point = point;
    event.radius = radius;
    event.impulse = impulse;
    return event;
}

/// The same blast with a brisance zone: full impulse out to `inner_radius`.
DamageEvent blast_at_with_inner(const Vec3& point, f32 radius, f32 inner_radius, f32 impulse) {
    DamageEvent event = blast_at(point, radius, impulse);
    event.inner_radius = inner_radius;
    return event;
}

/// Four leaves: enough bonds at enough places to see a blast's shape, and the
/// smallest object whose complete break can exceed a debris cap of one.
FractureAsset four_piece_asset() {
    FractureParams params;
    params.target_chunks = 4u;
    params.strength_per_area = 10.0f;

    FractureAsset asset;
    std::string error;
    build_fracture_asset(unit_cube(), params, asset, &error);
    NF_CHECK(error.empty());
    NF_CHECK(asset.leaf_count() >= 2u);
    return asset;
}

/// True when `descendant` is inside the subtree rooted at `ancestor`.
bool is_descendant_of(const FractureAsset& asset, u32 descendant, u32 ancestor) {
    u32 cursor = descendant;
    while (cursor != kInvalidChunk && cursor < asset.chunks.size()) {
        if (cursor == ancestor && cursor != descendant) return true;
        cursor = asset.chunks[cursor].parent;
    }
    return false;
}

} // namespace

// =========================================================================
// Falloff and thresholds
// =========================================================================

NF_TEST(destruction_point_blank_blast_shatters_the_bond) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // Dead centre on the shared face: the falloff is 1 there, so an impulse
    // equal to the strength is exactly enough.
    const DamageEvent event = blast_at(bond.centroid, 4.0f, bond.strength);

    NF_CHECK(world.apply_damage(asset, cmp, Mat4::identity(), event) == 1u);
    NF_CHECK(!cmp.intact());
    NF_CHECK(cmp.broken_count() == 1u);
    NF_CHECK(world.bonds_shattered_total() == 1u);
    NF_CHECK(sink.records.size() == 1u);
}

NF_TEST(destruction_blast_out_of_range_breaks_nothing) {
    const FractureAsset asset = two_piece_asset();

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // Ten units away with a radius of one: the bond is not even tickled.
    const DamageEvent event = blast_at(Vec3{10.0f, 0.0f, 0.0f}, 1.0f, 1e6f);
    NF_CHECK(world.apply_damage(asset, cmp, Mat4::identity(), event) == 0u);
    NF_CHECK(cmp.intact());
    NF_CHECK(cmp.bond_stress[0u] == 0.0f);
    NF_CHECK(sink.records.empty());
}

NF_TEST(destruction_weak_blasts_accumulate_until_the_bond_gives) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // A quarter of the strength per hit: the bond must take four, not one.
    const DamageEvent event = blast_at(bond.centroid, 4.0f, bond.strength * 0.25f);

    for (u32 hit = 1u; hit <= 3u; ++hit) {
        NF_CHECK(world.apply_damage(asset, cmp, Mat4::identity(), event) == 0u);
        NF_CHECK(cmp.intact());
    }
    NF_CHECK_NEAR(cmp.bond_stress[0u], bond.strength * 0.75f, 1e-5f);

    NF_CHECK(world.apply_damage(asset, cmp, Mat4::identity(), event) == 1u);
    NF_CHECK(!cmp.intact());
}

NF_TEST(destruction_strength_scale_makes_the_object_tougher) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.strength_scale = 2.0f;
    cmp.resize_for(asset);

    // The threshold is strength * scale, so the raw strength no longer does it.
    const DamageEvent half = blast_at(bond.centroid, 4.0f, bond.strength);
    NF_CHECK(world.apply_damage(asset, cmp, Mat4::identity(), half) == 0u);
    NF_CHECK(cmp.intact());

    DestructibleComponent tough = cmp;
    tough.strength_scale = 1.0f;
    NF_CHECK(world.apply_damage(asset, tough, Mat4::identity(), half) == 1u);
}

NF_TEST(destruction_empty_asset_is_a_no_op) {
    NullDebrisSink sink;
    DestructionWorld world(sink);

    FractureAsset empty;
    DestructibleComponent cmp;

    NF_CHECK(world.apply_damage(empty, cmp, Mat4::identity(),
                                blast_at(Vec3::zero, 10.0f, 1e6f)) == 0u);
    NF_CHECK(sink.records.empty());
    NF_CHECK(world.bonds_shattered_total() == 0u);
}

NF_TEST(destruction_component_resize_keeps_existing_damage) {
    const FractureAsset asset = two_piece_asset();
    DestructibleComponent cmp;
    cmp.resize_for(asset);
    cmp.bond_stress[0u] = 3.0f;

    // Re-binding the same asset must not heal the object.
    cmp.resize_for(asset);
    NF_CHECK(cmp.bond_stress.size() == 1u);
    NF_CHECK_NEAR(cmp.bond_stress[0u], 3.0f, 1e-5f);
}

// =========================================================================
// Shard kinematics
// =========================================================================

NF_TEST(destruction_shard_mass_is_volume_times_density) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.density = 7.0f;
    cmp.resize_for(asset);

    world.apply_damage(asset, cmp, Mat4::identity(),
                       blast_at(bond.centroid, 4.0f, bond.strength * 2.0f));

    NF_CHECK(sink.records.size() == 1u);
    const FractureChunk& released = asset.chunks[sink.records[0u].spawn.chunk_id];
    NF_CHECK(sink.records[0u].spawn.chunk_id == bond.detach_chunk);
    NF_CHECK_NEAR(sink.records[0u].spawn.mass, released.volume * 7.0f, 1e-5f);
}

NF_TEST(destruction_shard_flies_away_from_the_blast) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    const DamageEvent event = blast_at(bond.centroid, 4.0f, bond.strength * 2.0f);
    world.apply_damage(asset, cmp, Mat4::identity(), event);

    const DebrisSpawn& shard = sink.records[0u].spawn;
    // The kick is radial from the chunk's centre of mass, and spawn.position is
    // the asset origin — not the COM — so the direction comes from the asset.
    const Vec3 outward = asset.chunks[bond.detach_chunk].centroid - event.world_point;

    // The kick is radial, so the velocity and the displacement agree on a
    // direction; with the blast on the shared face, the displacement is not
    // degenerate and the speed is the delivered impulse over the mass.
    NF_CHECK(shard.linear_velocity.dot(outward) > 0.0f);
    NF_CHECK_NEAR(shard.linear_velocity.length(), event.impulse / shard.mass, 1e-4f);
}

NF_TEST(destruction_shard_spins_about_the_tear_point) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // Off-centre: the impulse is not radial about the arm, so the cross product
    // is non-zero. A blast on the shared face would give a collinear arm and
    // direction and no spin at all — correct physics, but not this test.
    const DamageEvent event = blast_at(bond.centroid + Vec3{1.5f, 1.0f, 0.5f}, 10.0f,
                                       bond.strength * 2.0f);
    world.apply_damage(asset, cmp, Mat4::identity(), event);

    const DebrisSpawn& shard = sink.records[0u].spawn;
    // Both the arm and the kick direction are measured from the chunk's centre
    // of mass; spawn.position is the asset origin now, so the COM comes from
    // the asset (identity world transform here, so local == world).
    const Vec3 centroid = asset.chunks[bond.detach_chunk].centroid;
    const Vec3 arm = centroid - bond.centroid;
    const Vec3 dir = (centroid - event.world_point).normalized();
    const f32  falloff = 1.0f - (event.world_point - bond.centroid).length() / event.radius;
    const f32  delivered = event.impulse * falloff;
    const f32  arm_sq = arm.length_sq() + EPSILON;

    // The torque is r x J over the point-mass inertia, so the spin axis is
    // normal to both the arm and the kick, and the magnitude is closed form.
    NF_CHECK(shard.angular_velocity.length_sq() > 0.0f);
    NF_CHECK_NEAR(shard.angular_velocity.dot(arm), 0.0f, 1e-4f);
    NF_CHECK_NEAR(shard.angular_velocity.dot(dir), 0.0f, 1e-4f);
    NF_CHECK_NEAR(shard.angular_velocity.x, (arm.cross(dir) * (delivered / (shard.mass * arm_sq))).x, 1e-4f);
    NF_CHECK_NEAR(shard.angular_velocity.y, (arm.cross(dir) * (delivered / (shard.mass * arm_sq))).y, 1e-4f);
    NF_CHECK_NEAR(shard.angular_velocity.z, (arm.cross(dir) * (delivered / (shard.mass * arm_sq))).z, 1e-4f);
}

NF_TEST(destruction_direct_hit_has_no_direction_so_it_goes_up) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // Detonate exactly on the released chunk's centroid: the radial direction
    // is undefined, and the fallback is up rather than NaN.
    const Vec3 centroid = asset.chunks[bond.detach_chunk].centroid;
    world.apply_damage(asset, cmp, Mat4::identity(), blast_at(centroid, 4.0f, bond.strength * 2.0f));

    NF_CHECK(sink.records.size() == 1u);
    NF_CHECK(sink.records[0u].spawn.linear_velocity.y > 0.0f);
}

NF_TEST(destruction_world_transform_places_the_shard) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // The object sits ten units off the origin. A shard's hull is authored in
    // the asset's own space and the sink places that hull's local origin at
    // `position`, so the position has to be the asset's origin in world space —
    // not the chunk's centroid, which would offset the shard by its own local
    // centroid and fly it through empty air next to the object it came from.
    // The invariant that makes that visible is reconstructing the hull in world
    // space: position + rotation * vertex must equal the world transform of
    // that vertex, at every vertex. That is also exactly what a renderer
    // drawing the chunk mesh at the body's pose depends on.
    const Vec3 offset{10.0f, 0.0f, -4.0f};
    const Mat4 xform = Mat4::translate(offset);
    const DamageEvent event = blast_at(bond.centroid + offset, 4.0f, bond.strength * 2.0f);

    world.apply_damage(asset, cmp, xform, event);

    NF_CHECK(sink.records.size() == 1u);
    const DebrisSpawn& spawn = sink.records[0u].spawn;
    NF_CHECK(spawn.position.nearly_equals(offset, 1e-4f));

    const FractureChunk& chunk = asset.chunks[bond.detach_chunk];
    NF_CHECK(spawn.hull_points.size() == chunk.piece.vertices.size());
    for (size_t i = 0u; i < spawn.hull_points.size(); ++i) {
        const Vec3 placed = spawn.position + spawn.rotation.rotate(spawn.hull_points[i]);
        const Vec3 expected = xform.transform_point(chunk.piece.vertices[i]);
        NF_CHECK(placed.nearly_equals(expected, 1e-4f));
    }
}

// =========================================================================
// Emission invariants
// =========================================================================

/// A hand-built asset, so the emission tests see a known tree rather than
/// whatever the builder's planes happened to produce: the root splits into a
/// leaf (1) and a region (2) that itself splits into two leaves (3, 4).
/// Volumes are exact — chunk 2's volume is the union of its children's, which
/// is the property the one-lump emission has to reproduce.
FractureAsset five_chunk_asset() {
    FractureAsset asset;
    asset.chunks.resize(5u);
    asset.bonds.resize(2u);

    asset.chunks[0u] = FractureChunk{FracturePiece{}, Vec3{0.0f, 0.0f, 0.0f}, 8.0f,
                                     kInvalidChunk, {1u, 2u}, 0u};
    asset.chunks[1u] = FractureChunk{FracturePiece{}, Vec3{-1.0f, 0.0f, 0.0f}, 4.0f,
                                     0u, {kInvalidChunk, kInvalidChunk}, 1u};
    asset.chunks[2u] = FractureChunk{FracturePiece{}, Vec3{1.0f, 0.0f, 0.0f}, 4.0f,
                                     0u, {3u, 4u}, 1u};
    asset.chunks[3u] = FractureChunk{FracturePiece{}, Vec3{1.5f, 0.0f, 0.0f}, 2.0f,
                                     2u, {kInvalidChunk, kInvalidChunk}, 2u};
    asset.chunks[4u] = FractureChunk{FracturePiece{}, Vec3{0.5f, 0.0f, 0.0f}, 2.0f,
                                     2u, {kInvalidChunk, kInvalidChunk}, 2u};

    asset.bonds[0u] = FractureBond{0u, 2u, 1u,
                                   Vec3{1.0f, 0.0f, 0.0f}, -1e-4f, 4.0f,
                                   Vec3{0.0f, 0.0f, 0.0f}, 10.0f};
    asset.bonds[1u] = FractureBond{2u, 3u, 4u,
                                   Vec3{1.0f, 0.0f, 0.0f}, -1.0f, 2.0f,
                                   Vec3{1.0f, 0.0f, 0.0f}, 10.0f};
    return asset;
}

NF_TEST(destruction_a_detached_region_is_not_emitted_twice) {
    const FractureAsset asset = five_chunk_asset();

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // The state after the inner bond has gone in an earlier blast: the bond is
    // recorded broken, its released chunk detached, and that shard is already
    // in flight. Both flags are needed — detached is membership in the released
    // region, emitted is what actually keeps it from being spawned again.
    cmp.bond_broken[1u] = 1u;
    cmp.chunk_detached[3u] = 1u;
    cmp.chunk_emitted[3u] = 1u;
    sink.spawn(DebrisSpawn{3u, {}, {}, {}, {}, 1.0f, {}});

    // The root bond goes now. Its region's inner bond is already broken, so the
    // walk splits the region — and must skip chunk 3, which left already.
    const u32 broken = world.apply_damage(asset, cmp, Mat4::identity(),
                                          blast_at(asset.bonds[0u].centroid, 4.0f, 40.0f));
    NF_CHECK(broken == 1u);
    NF_CHECK(sink.records.size() == 2u);
    const u32 new_chunk = sink.records[1u].spawn.chunk_id;
    NF_CHECK(new_chunk == 4u);
    NF_CHECK(cmp.chunk_detached[4u] != 0u);
}

NF_TEST(destruction_intact_inner_bond_makes_the_region_fly_as_one_lump) {
    const FractureAsset asset = five_chunk_asset();

    NullDebrisSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.density = 3.0f;
    cmp.resize_for(asset);

    // The same region, but its inner bond is whole: it releases as chunk 2 —
    // one convex body — rather than as the two leaves it is made of.
    const u32 broken = world.apply_damage(asset, cmp, Mat4::identity(),
                                          blast_at(asset.bonds[0u].centroid, 4.0f, 40.0f));
    NF_CHECK(broken == 1u);
    NF_CHECK(sink.records.size() == 1u);
    NF_CHECK(sink.records[0u].spawn.chunk_id == 2u);
    NF_CHECK_NEAR(sink.records[0u].spawn.mass,
                  asset.chunks[2u].volume * 3.0f, 1e-5f);
    NF_CHECK_NEAR(sink.records[0u].spawn.mass / 3.0f,
                  asset.chunks[3u].volume + asset.chunks[4u].volume, 1e-5f);
}

NF_TEST(destruction_shards_never_overlap_or_repeat_a_chunk) {
    // Break everything that will break, over however many frames it takes, then
    // check the emitted chunks partition the object: no two shards share a
    // chunk, and no shard is inside another shard's subtree.
    FractureParams params;
    params.target_chunks = 8u;

    FractureAsset asset;
    build_fracture_asset(unit_cube(), params, asset);

    NullDebrisSink sink;
    DestructionBudget budget;
    budget.max_breaks_per_frame = 3u;          // forces the deferral path
    DestructionWorld world(sink, budget);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    // A blast at the origin reaches every bond; repeat until the world has
    // nothing left to shatter, with a ceiling so a broken loop fails loudly.
    const DamageEvent event = blast_at(Vec3::zero, 20.0f, 1e6f);
    u32 last_total = 0u;
    for (u32 frame = 0u; frame < 32u; ++frame) {
        world.apply_damage(asset, cmp, Mat4::identity(), event);
        world.tick(0.1f);
        if (world.bonds_shattered_total() == last_total) break;
        last_total = world.bonds_shattered_total();
    }

    NF_CHECK(world.bonds_shattered_total() > 0u);
    NF_CHECK(!sink.records.empty());

    std::vector<u32> emitted;
    for (const NullDebrisSink::Record& record : sink.records) {
        emitted.push_back(record.spawn.chunk_id);
    }
    for (std::size_t i = 0u; i < emitted.size(); ++i) {
        for (std::size_t j = i + 1u; j < emitted.size(); ++j) {
            NF_CHECK(emitted[i] != emitted[j]);
            NF_CHECK(!is_descendant_of(asset, emitted[i], emitted[j]));
            NF_CHECK(!is_descendant_of(asset, emitted[j], emitted[i]));
        }
    }

    // Whatever came loose is marked gone in the component, and the emitted
    // volume is a real part of the cube — never more than the whole. The emitted
    // count must equal the sink's record count exactly: chunk_emitted is the
    // component's mirror of the sink, so a drift here would mean a shard was
    // spawned without being recorded, or recorded without being spawned.
    f32 emitted_volume = 0.0f;
    for (const u32 chunk : emitted) {
        NF_CHECK(cmp.chunk_detached[chunk] != 0u);
        NF_CHECK(cmp.chunk_emitted[chunk] != 0u);
        emitted_volume += asset.chunks[chunk].volume;
    }
    NF_CHECK(emitted_volume <= 8.0f + 1e-4f);
    NF_CHECK(cmp.emitted_count() == emitted.size());
}

// =========================================================================
// Budget
// =========================================================================

NF_TEST(destruction_frame_allowance_is_shared_across_objects) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionBudget budget;
    budget.max_breaks_per_frame = 1u;
    DestructionWorld world(sink, budget);

    DestructibleComponent a;
    a.resize_for(asset);
    DestructibleComponent b;
    b.resize_for(asset);

    const DamageEvent event = blast_at(bond.centroid, 4.0f, bond.strength * 2.0f);

    // Frame 1: A shatters, B is throttled but keeps the stress it took.
    NF_CHECK(world.apply_damage(asset, a, Mat4::identity(), event) == 1u);
    NF_CHECK(world.apply_damage(asset, b, Mat4::identity(), event) == 0u);
    NF_CHECK(!a.intact());
    NF_CHECK(b.intact());
    NF_CHECK(b.bond_stress[0u] > 0.0f);

    world.tick(0.1f);

    // Frame 2: the allowance refills and B goes.
    NF_CHECK(world.apply_damage(asset, b, Mat4::identity(), event) == 1u);
    NF_CHECK(!b.intact());
    NF_CHECK(world.bonds_shattered_total() == 2u);
    NF_CHECK(sink.records.size() == 2u);
}

NF_TEST(destruction_cap_retires_the_oldest_and_keeps_the_new_one) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionBudget budget;
    budget.max_active_debris = 1u;
    DestructionWorld world(sink, budget);

    DestructibleComponent a;
    a.resize_for(asset);
    DestructibleComponent b;
    b.resize_for(asset);

    const DamageEvent event = blast_at(bond.centroid, 4.0f, bond.strength * 2.0f);
    world.apply_damage(asset, a, Mat4::identity(), event);
    const u32 first_shard = sink.records[0u].id;
    NF_CHECK(sink.active_count() == 1u);

    // A new shard under a cap of one evicts the oldest rather than refusing.
    world.apply_damage(asset, b, Mat4::identity(), event);

    NF_CHECK(sink.active_count() == 1u);
    NF_CHECK(sink.find(first_shard) == nullptr);
    NF_CHECK(sink.records[0u].spawn.chunk_id == bond.detach_chunk);
    NF_CHECK(world.active_debris() == 1u);
}

NF_TEST(destruction_lifetime_expires_the_shard) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    NullDebrisSink sink;
    DestructionBudget budget;
    budget.max_debris_lifetime = 1.0f;
    DestructionWorld world(sink, budget);

    DestructibleComponent cmp;
    cmp.resize_for(asset);
    world.apply_damage(asset, cmp, Mat4::identity(),
                       blast_at(bond.centroid, 4.0f, bond.strength * 2.0f));
    NF_CHECK(world.active_debris() == 1u);

    world.tick(0.5f);
    NF_CHECK(world.active_debris() == 1u);

    world.tick(1.0f);
    NF_CHECK(world.active_debris() == 0u);
    NF_CHECK(sink.records.empty());
}

namespace {

/// Takes nothing, so the budget's refusal counter is the only observable.
class RefusingSink final : public IDebrisSink {
public:
    u32 spawn(const DebrisSpawn&) override { return kInvalidDebris; }
    void destroy(u32) override {}
    std::size_t active_count() const override { return 0u; }
};

} // namespace

NF_TEST(destruction_refused_spawn_is_counted_not_swallowed) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    RefusingSink sink;
    DestructionWorld world(sink);

    DestructibleComponent cmp;
    cmp.resize_for(asset);

    world.apply_damage(asset, cmp, Mat4::identity(),
                       blast_at(bond.centroid, 4.0f, bond.strength * 2.0f));

    // The bond shattered and the shard was refused: both facts visible, rather
    // than a silent no-op that looks like the object survived.
    NF_CHECK(world.bonds_shattered_total() == 1u);
    NF_CHECK(world.active_debris() == 0u);
    NF_CHECK(world.shards_dropped_for_budget() == 1u);
}

NF_TEST(destruction_destroy_is_called_once_per_shard) {
    // destroy must be called exactly once per id, even under the eviction path
    // and the lifetime path in the same tick.
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    class CountingSink final : public IDebrisSink {
    public:
        u32 spawn(const DebrisSpawn&) override { return ++next_; }
        void destroy(u32 id) override {
            destroyed_.push_back(id);
        }
        std::size_t active_count() const override { return 0u; }

        u32 next_ = 0u;
        std::vector<u32> destroyed_;
    };

    CountingSink sink;
    DestructionBudget budget;
    budget.max_active_debris = 2u;
    budget.max_debris_lifetime = 1.0f;
    DestructionWorld world(sink, budget);

    DestructibleComponent cmp;
    cmp.resize_for(asset);
    world.apply_damage(asset, cmp, Mat4::identity(),
                       blast_at(bond.centroid, 4.0f, bond.strength * 2.0f));
    world.tick(2.0f);

    NF_CHECK(sink.destroyed_.size() == 1u);
    NF_CHECK(sink.destroyed_[0u] == 1u);
}

// =========================================================================
// Determinism
// =========================================================================

NF_TEST(destruction_same_blasts_give_identical_shards) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    const DamageEvent event = blast_at(bond.centroid * 1.3f, 3.0f, bond.strength * 2.0f);

    NullDebrisSink first_sink;
    DestructionWorld first(first_sink);
    DestructibleComponent first_cmp;
    first_cmp.density = 3.0f;
    first_cmp.resize_for(asset);
    first.apply_damage(asset, first_cmp, Mat4::identity(), event);

    NullDebrisSink second_sink;
    DestructionWorld second(second_sink);
    DestructibleComponent second_cmp;
    second_cmp.density = 3.0f;
    second_cmp.resize_for(asset);
    second.apply_damage(asset, second_cmp, Mat4::identity(), event);

    NF_CHECK(first.is_deterministic());
    NF_CHECK(second.is_deterministic());
    NF_CHECK(first_sink.records.size() == second_sink.records.size());
    for (std::size_t i = 0u; i < first_sink.records.size(); ++i) {
        const DebrisSpawn& a = first_sink.records[i].spawn;
        const DebrisSpawn& b = second_sink.records[i].spawn;
        NF_CHECK(a.chunk_id == b.chunk_id);
        NF_CHECK(a.position.nearly_equals(b.position, 0.0f));
        NF_CHECK(a.mass == b.mass);
        NF_CHECK(a.linear_velocity.nearly_equals(b.linear_velocity, 1e-6f));
        NF_CHECK(a.angular_velocity.nearly_equals(b.angular_velocity, 1e-6f));
    }
}

// =========================================================================
// Explosion shape: the brisance zone
// =========================================================================

NF_TEST(destruction_inner_radius_delivers_the_full_impulse) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    // A sub-strength impulse so nothing shatters and the delivered amount can
    // be read straight off the accumulated stress, not inferred from breaks.
    const f32 impulse = bond.strength * 0.5f;
    const Vec3 blast = bond.centroid + Vec3{2.0f, 0.0f, 0.0f};

    auto stress_for = [&](f32 inner_radius) {
        NullDebrisSink sink;
        DestructionWorld world(sink);
        DestructibleComponent cmp;
        cmp.resize_for(asset);
        const DamageEvent event = blast_at_with_inner(blast, 10.0f, inner_radius, impulse);
        NF_CHECK(world.apply_damage(asset, cmp, Mat4::identity(), event) == 0u);
        return cmp.bond_stress[0u];
    };

    // Plain linear blast two tenths of the way out: 80% of the impulse.
    NF_CHECK_NEAR(stress_for(0.0f), impulse * 0.8f, 1e-5f);
    // The bond sits inside the brisance zone: the whole impulse arrives even
    // though the bond is not at the blast centre.
    NF_CHECK_NEAR(stress_for(5.0f), impulse, 1e-5f);
    // Barely inside the zone edge.
    NF_CHECK_NEAR(stress_for(2.0f), impulse, 1e-5f);
    // A degenerate inner radius (>= the radius) is the plain blast, not a
    // free full-impulse hit.
    NF_CHECK_NEAR(stress_for(10.0f), impulse * 0.8f, 1e-5f);
    NF_CHECK_NEAR(stress_for(99.0f), impulse * 0.8f, 1e-5f);
}

NF_TEST(destruction_inner_radius_bends_the_blast) {
    const FractureAsset asset = four_piece_asset();
    const Vec3  blast{0.0f, 0.0f, 0.0f};
    const f32   radius = 10.0f;

    // Shards broken by a blast of the given brisance zone. The cube spans
    // [-1, 1] so every bond sits well inside the radius and the comparison is
    // about the falloff *shape*, not about reach.
    auto broken_for = [&](f32 inner_radius, f32 impulse) {
        NullDebrisSink sink;
        DestructionWorld world(sink);
        DestructibleComponent cmp;
        cmp.resize_for(asset);
        const DamageEvent event = blast_at_with_inner(blast, radius, inner_radius, impulse);
        return world.apply_damage(asset, cmp, Mat4::identity(), event);
    };

    // The piecewise falloff dominates the linear one everywhere inside the
    // radius (1 out to `inner`, then a shallower slope to zero), so no bond can
    // receive *less* stress when the brisance zone widens — the break count is
    // monotone in it for every impulse, not just a tuned one.
    u32 impulses_where_wide_breaks_more = 0u;
    for (f32 pulse = 1.0f; pulse < 500.0f; pulse *= 1.4f) {
        const u32 narrow = broken_for(0.0f, pulse);
        const u32 wide = broken_for(4.0f, pulse);
        NF_CHECK(wide >= narrow);
        if (wide > narrow) ++impulses_where_wide_breaks_more;
    }

    // ...and there is at least one impulse the two shapes actually separate on:
    // a brisance zone that changed nothing would be a no-op, and a test that
    // could not tell would be one.
    NF_CHECK(impulses_where_wide_breaks_more > 0u);
}

// =========================================================================
// Per-object shard lifetime
// =========================================================================

NF_TEST(destruction_short_lived_shards_retire_before_the_budget_lifetime) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    DestructionBudget budget;
    budget.max_debris_lifetime = 100.0f; // the world would keep the shard here

    NullDebrisSink sink;
    DestructionWorld world(sink, budget);

    DestructibleComponent cmp;
    cmp.shard_lifetime = 3.0f;
    cmp.resize_for(asset);
    NF_CHECK(world.apply_damage(asset, cmp, Mat4::identity(),
                                blast_at(bond.centroid, 4.0f, bond.strength * 2.0f)) == 1u);
    NF_CHECK(sink.records.size() == 1u);

    // Not yet: one tick short of the shard's own deadline.
    world.tick(2.9f);
    NF_CHECK(sink.records.size() == 1u);
    // The budget lifetime is 100 s, so it is the *shard's* 3 s clock that
    // retires it — the world's deadline alone would leave it live.
    world.tick(0.2f);
    NF_CHECK(sink.records.empty());
    NF_CHECK(world.active_debris() == 0u);
}

NF_TEST(destruction_shard_lifetime_zero_falls_back_to_the_budget) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    DestructionBudget budget;
    budget.max_debris_lifetime = 3.0f;

    NullDebrisSink sink;
    DestructionWorld world(sink, budget);

    DestructibleComponent cmp; // shard_lifetime == 0: use the budget
    cmp.resize_for(asset);
    world.apply_damage(asset, cmp, Mat4::identity(),
                       blast_at(bond.centroid, 4.0f, bond.strength * 2.0f));
    NF_CHECK(sink.records.size() == 1u);

    world.tick(2.9f);
    NF_CHECK(sink.records.size() == 1u);
    world.tick(0.2f);
    NF_CHECK(sink.records.empty());
}

NF_TEST(destruction_two_objects_retire_on_their_own_clocks) {
    // A short-lived shard and a lingering one from the same instant: 4 s vs
    // 40 s against a budget of 60 s. This is the pile-mixing case that makes
    // the lifetime per-object rather than per-world.
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];
    const DamageEvent event = blast_at(bond.centroid, 4.0f, bond.strength * 4.0f);

    NullDebrisSink sink;
    DestructionBudget budget;
    budget.max_debris_lifetime = 60.0f;
    DestructionWorld world(sink, budget);

    DestructibleComponent brief;
    brief.shard_lifetime = 4.0f;
    brief.resize_for(asset);
    world.apply_damage(asset, brief, Mat4::identity(), event);
    const u32 brief_id = sink.records.back().id;

    DestructibleComponent lingering;
    lingering.shard_lifetime = 40.0f;
    lingering.resize_for(asset);
    world.apply_damage(asset, lingering, Mat4::identity(), event);
    // back(), not front(): records are appended, so the first shard is still
    // the brief one's and would alias it here.
    const u32 lingering_id = sink.records.back().id;

    world.tick(5.0f);
    NF_CHECK(sink.find(brief_id) == nullptr);    // its own clock ran out
    NF_CHECK(sink.find(lingering_id) != nullptr); // not the budget's

    world.tick(40.0f);
    NF_CHECK(sink.find(lingering_id) == nullptr);
    NF_CHECK(sink.records.empty());
}

// =========================================================================
// Named material presets
// =========================================================================

NF_TEST(destruction_material_presets_are_found_by_name) {
    NF_CHECK(find_material("glass") != nullptr);
    NF_CHECK(find_material("GLASS") != nullptr);  // case-insensitive
    NF_CHECK(find_material("Wood") != nullptr);
    NF_CHECK(find_material("stone") != nullptr);
    NF_CHECK(find_material("steel") != nullptr);
    NF_CHECK(find_material("unobtanium") == nullptr); // unknown: no substitution
    NF_CHECK(find_material("") == nullptr);           // empty: no material
}

NF_TEST(destruction_material_presets_scale_up_in_strength) {
    // The table is ordered brittle -> stubborn, so a walkthrough is the
    // assertion that the shipped values are monotone: glass < wood < stone <
    // steel. A reorder would break a level that picked materials by name.
    NF_CHECK(std::size(kFractureMaterials) >= 4u);
    for (std::size_t i = 1u; i < std::size(kFractureMaterials); ++i) {
        NF_CHECK(kFractureMaterials[i].strength_scale >
                 kFractureMaterials[i - 1u].strength_scale);
    }
}

NF_TEST(destruction_apply_material_writes_the_component_knobs) {
    DestructibleComponent cmp;
    cmp.strength_scale = 7.0f;
    cmp.density = 7.0f;
    cmp.shard_lifetime = 7.0f;

    const FractureMaterial* glass = find_material("glass");
    NF_CHECK(glass != nullptr);
    apply_material(cmp, *glass);
    NF_CHECK(cmp.strength_scale == glass->strength_scale);
    NF_CHECK(cmp.density == glass->density);
    NF_CHECK(cmp.shard_lifetime == glass->shard_lifetime);

    // Wood's lifetime is the budget's, which is expressed as 0 here.
    const FractureMaterial* wood = find_material("wood");
    apply_material(cmp, *wood);
    NF_CHECK(cmp.shard_lifetime == 0.0f);
}

NF_TEST(destruction_glass_shatters_where_steel_survives_the_same_blast) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    auto breaks = [&](std::string_view material) {
        NullDebrisSink sink;
        DestructionWorld world(sink);
        DestructibleComponent cmp;
        const FractureMaterial* preset = find_material(material);
        NF_CHECK(preset != nullptr);
        apply_material(cmp, *preset);
        cmp.resize_for(asset);
        // A blast sized for wood: half the bond's raw strength.
        const DamageEvent event = blast_at(bond.centroid, 4.0f, bond.strength * 0.5f);
        return world.apply_damage(asset, cmp, Mat4::identity(), event) > 0u;
    };

    NF_CHECK(breaks("glass"));  // 0.2x strength: the half-strength blast is plenty
    NF_CHECK(!breaks("wood"));  // 1.0x: half the strength is not enough
    NF_CHECK(!breaks("stone")); // 3.0x: shrugs it off
    NF_CHECK(!breaks("steel")); // 8.0x: untouched
}

NF_TEST(destruction_material_density_sets_the_shard_mass) {
    const FractureAsset asset = two_piece_asset();
    const FractureBond& bond = asset.bonds[0u];

    auto shard_mass = [&](std::string_view material) {
        NullDebrisSink sink;
        DestructionWorld world(sink);
        DestructibleComponent cmp;
        apply_material(cmp, *find_material(material));
        cmp.resize_for(asset);
        world.apply_damage(asset, cmp, Mat4::identity(),
                           blast_at(bond.centroid, 4.0f, bond.strength * 8.0f));
        return sink.records.front().spawn.mass;
    };

    const f32 glass = shard_mass("glass");
    const f32 steel = shard_mass("steel");
    NF_CHECK(glass > 0.0f);
    NF_CHECK(steel > glass); // steel shards are dense, glass ones are light
    NF_CHECK_NEAR(steel / glass,
                  find_material("steel")->density / find_material("glass")->density, 1e-4f);
}

namespace {

/// A sink with its own hard ceiling, which is the realistic backend: Jolt can
/// be out of bodies or out of contact slots. The world's budget is a *policy*
/// cap, and it is checked before spawn is called, but a sink that refuses for
/// its own reasons exercises the accounting the refusal path has to keep.
class CappedSink final : public IDebrisSink {
public:
    explicit CappedSink(std::size_t capacity) : capacity_(capacity) {}

    u32 spawn(const DebrisSpawn& spawn) override {
        if (records.size() >= capacity_) return kInvalidDebris;
        const u32 id = next_id++;
        records.push_back(NullDebrisSink::Record{id, spawn, 0.0f});
        return id;
    }

    void destroy(u32 debris_id) override {
        for (std::size_t i = 0u; i < records.size(); ++i) {
            if (records[i].id != debris_id) continue;
            records.erase(records.begin() + i);
            return;
        }
    }

    std::size_t active_count() const override { return records.size(); }

    std::vector<NullDebrisSink::Record> records;
    const std::size_t                   capacity_;

private:
    u32 next_id = 1u;
};

} // namespace

NF_TEST(destruction_budget_is_enforced_inside_apply_damage_not_reconciled) {
    // Section 41's "budget so it does not kill the game's performance" only
    // holds if the decision happens *before* a body is asked for, not in a
    // reconciliation pass afterwards. A blast that releases several shards
    // against a ceiling of two: the live count is at the ceiling the instant
    // apply_damage returns, never above it, and no later tick is needed to
    // bring it down.
    const FractureAsset asset = four_piece_asset();

    DestructionBudget budget;
    budget.max_active_debris = 2u;          // the world's policy cap
    budget.max_breaks_per_frame = 32u;      // do not let the frame allowance mask it
    budget.max_debris_lifetime = 1000.0f;   // no lifetime pressure at all

    NullDebrisSink sink;
    DestructionWorld world(sink, budget);

    DestructibleComponent cmp;
    cmp.resize_for(asset);
    const u32 broken =
        world.apply_damage(asset, cmp, Mat4::identity(),
                           blast_at(Vec3::zero, 50.0f, 1e6f));

    // The ceiling held without a tick, and the world evicted to make room
    // rather than refusing — the newest shard is always the one kept.
    NF_CHECK(broken >= 2u);
    NF_CHECK_EQ(world.active_debris(), static_cast<std::size_t>(budget.max_active_debris));
    NF_CHECK(world.shards_dropped_for_budget() == 0u);
    NF_CHECK(cmp.emitted_count() == cmp.broken_count());
}

NF_TEST(destruction_sink_refusal_is_counted_and_never_inflates_the_count) {
    // The sink's own ceiling is lower than the world's policy cap, so the world
    // believes there is room and does not evict — but the backend refuses. The
    // books have to stay consistent anyway: the drop is counted, the live count
    // is what the sink actually holds, and chunk_emitted stays an exact mirror
    // of the sink rather than recording a shard that never became a body.
    const FractureAsset asset = four_piece_asset();

    DestructionBudget budget;
    budget.max_active_debris = 4u;          // deliberately above the sink's ceiling
    budget.max_breaks_per_frame = 32u;
    budget.max_debris_lifetime = 1000.0f;

    CappedSink sink(1u); // one body in the whole backend
    DestructionWorld world(sink, budget);

    DestructibleComponent cmp;
    cmp.resize_for(asset);
    const u32 broken =
        world.apply_damage(asset, cmp, Mat4::identity(),
                           blast_at(Vec3::zero, 50.0f, 1e6f));

    NF_CHECK(broken >= 2u);                       // the bonds went
    NF_CHECK_EQ(world.active_debris(), std::size_t{1u}); // the backend holds one
    NF_CHECK(world.shards_dropped_for_budget() > 0u);
    // chunk_emitted mirrors the sink exactly: no phantom shard.
    NF_CHECK(cmp.emitted_count() == static_cast<u32>(world.active_debris()));
    // The chunks the sink never took are still marked detached — the object is
    // genuinely in pieces — but they have no debris body, which is the honest
    // state rather than a silent success.
    NF_CHECK(cmp.detached_count() > cmp.emitted_count());
}
