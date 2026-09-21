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
    const Vec3 outward = shard.position - event.world_point;

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
    const Vec3 arm = shard.position - bond.centroid;
    const Vec3 dir = (shard.position - event.world_point).normalized();
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

    // The object sits ten units off the origin, so the blast is placed in world
    // space and the shard must land where the chunk actually is, not at its
    // asset-local centroid.
    const Vec3 offset{10.0f, 0.0f, -4.0f};
    const Mat4 xform = Mat4::translate(offset);
    const DamageEvent event = blast_at(bond.centroid + offset, 4.0f, bond.strength * 2.0f);

    world.apply_damage(asset, cmp, xform, event);

    NF_CHECK(sink.records.size() == 1u);
    NF_CHECK(sink.records[0u].spawn.position.nearly_equals(
        asset.chunks[bond.detach_chunk].centroid + offset, 1e-4f));
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
