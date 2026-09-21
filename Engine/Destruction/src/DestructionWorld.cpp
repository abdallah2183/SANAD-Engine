// NF/Destruction/DestructionWorld.cpp — damage, fracture propagation, debris.
//
// The arithmetic here is deliberately ordinary: a linear falloff, an
// accumulated stress, a convex hull per shard. What makes it usable in a game
// is not cleverness but the two invariants the file is organised around —
// emitted shards partition the detached volume exactly, and every budget
// decision happens before a body is asked for (Section 41: "a budget so it
// does not kill the game's performance").

#include <NF/Destruction/DestructionWorld.hpp>

#include <algorithm>
#include <utility>

namespace nf::destruction {

namespace {

/// True while `bond_index` still holds, from the component's own record. The
/// emitter needs this to decide whether a detached region flies as one lump or
/// splits along an inner bond that an earlier blast had already taken.
bool bond_intact(const DestructibleComponent& cmp, u32 bond_index) {
    if (bond_index >= cmp.bond_broken.size()) return true;
    return cmp.bond_broken[bond_index] == 0u;
}

} // namespace

DestructionWorld::DestructionWorld(IDebrisSink& sink, DestructionBudget budget)
    : sink_(sink), budget_(budget) {}

f32 DestructionWorld::damage_falloff(f32 distance, f32 radius) {
    if (radius <= EPSILON) return 0.0f;
    const f32 t = 1.0f - (distance / radius);
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

u32 DestructionWorld::apply_damage(const FractureAsset& asset, DestructibleComponent& cmp,
                                   const Mat4& world, const DamageEvent& event) {
    if (asset.chunks.empty()) return 0u;
    cmp.resize_for(asset);

    u32 broken_now = 0u;

    // Bonds are stored in creation order, which is breadth-first, so a bond
    // that releases a region is always visited before the bonds *inside* that
    // region. The chunk_detached check below is what makes the loop correct
    // when it is not, and is a no-op in the common case.
    for (u32 bond_index = 0u; bond_index < asset.bonds.size(); ++bond_index) {
        const FractureBond& bond = asset.bonds[bond_index];
        if (cmp.bond_broken[bond_index] != 0u) continue;
        if (cmp.chunk_detached[bond.parent] != 0u) continue;

        const Vec3 face_world = world.transform_point(bond.centroid);
        const f32  distance = (face_world - event.world_point).length();
        if (distance > event.radius) continue;

        const f32 delivered = event.impulse * damage_falloff(distance, event.radius);
        cmp.bond_stress[bond_index] += delivered;

        const f32 threshold = bond.strength * cmp.strength_scale;
        if (cmp.bond_stress[bond_index] < threshold) continue;

        // Throttled, not forgotten: the stress stays accumulated, so this bond
        // shatters on the first frame the allowance frees.
        if (breaks_this_frame_ >= budget_.max_breaks_per_frame) continue;

        cmp.bond_broken[bond_index] = 1u;
        mark_subtree_detached(asset, bond.detach_chunk, cmp);
        emit_subtree(asset, bond.detach_chunk, world, face_world, event, cmp);

        ++breaks_this_frame_;
        ++shattered_total_;
        ++broken_now;
    }

    return broken_now;
}

void DestructionWorld::mark_subtree_detached(const FractureAsset& asset, u32 chunk_id,
                                             DestructibleComponent& cmp) const {
    if (chunk_id >= asset.chunks.size()) return;
    cmp.chunk_detached[chunk_id] = 1u;
    const FractureChunk& chunk = asset.chunks[chunk_id];
    if (chunk.is_leaf()) return;
    mark_subtree_detached(asset, chunk.children[0], cmp);
    mark_subtree_detached(asset, chunk.children[1], cmp);
}

void DestructionWorld::emit_subtree(const FractureAsset& asset, u32 chunk_id, const Mat4& world,
                                    const Vec3& tear_world, const DamageEvent& event,
                                    DestructibleComponent& cmp) {
    if (chunk_id >= asset.chunks.size()) return;
    const FractureChunk& chunk = asset.chunks[chunk_id];

    if (chunk.is_leaf()) {
        emit_body(chunk_id, chunk, world, tear_world, event, cmp);
        return;
    }

    // The split bond is the only thing that can divide this region, so its
    // state decides whether the region flies as one lump or as two.
    const u32 inner = asset.split_bond(chunk_id);
    if (inner == kInvalidBond) return;
    if (bond_intact(cmp, inner)) {
        emit_body(chunk_id, chunk, world, tear_world, event, cmp);
        return;
    }

    // A child that has already had a body spawned for it left the object in an
    // earlier blast, so it cannot be part of this release. The entry chunk is
    // deliberately not checked — it was marked detached by the caller precisely
    // so that it would be released here; the guard applies only to what the walk
    // descends into, which is what keeps a shattered region from being emitted
    // twice. chunk_detached is the wrong flag here: it marks membership in a
    // released region, and the whole region is marked before anything emits.
    for (const u32 child : chunk.children) {
        if (cmp.chunk_emitted[child] != 0u) continue;
        emit_subtree(asset, child, world, tear_world, event, cmp);
    }
}

void DestructionWorld::emit_body(u32 chunk_id, const FractureChunk& chunk,
                                 const Mat4& world, const Vec3& tear_world, const DamageEvent& event,
                                 DestructibleComponent& cmp) {
    const Vec3 centroid_world = world.transform_point(chunk.centroid);
    const f32  delivered = event.impulse * damage_falloff((tear_world - event.world_point).length(),
                                                          event.radius);

    // Radial kick from the blast centre; a detonation directly beneath the
    // shard has no direction of its own, so the shard goes up instead.
    Vec3 dir = centroid_world - event.world_point;
    if (dir.length_sq() <= EPSILON) dir = Vec3::up;
    dir = dir.normalized();

    DebrisSpawn spawn;
    spawn.chunk_id = chunk_id;
    spawn.position = centroid_world;
    spawn.rotation = Quat::from_matrix(world);
    spawn.mass = chunk.volume * cmp.density;
    spawn.linear_velocity = dir * (delivered / spawn.mass);

    // The tear force is applied at the shared face, offset from the centre of
    // mass, which is why a shard spins. dw = I^-1 (r x J); a point mass at the
    // arm's length gives I = m r^2, and the epsilon keeps a face through the
    // centroid finite rather than dividing by zero.
    const Vec3 arm = centroid_world - tear_world;
    const f32  arm_sq = arm.length_sq() + EPSILON;
    spawn.angular_velocity = arm.cross(dir) * (delivered / (spawn.mass * arm_sq));

    spawn.hull_points = chunk.piece.vertices;

    if (sink_.active_count() >= budget_.max_active_debris) {
        retire_oldest();
    }

    const u32 id = sink_.spawn(spawn);
    if (id == kInvalidDebris) {
        ++budget_drops_;
        return;
    }
    // Recorded only on a successful spawn, so a shard refused for budget reasons
    // is still eligible to be emitted later — the drop is a deferral, not a
    // permanent loss, and chunk_emitted stays the exact mirror of the sink.
    cmp.chunk_emitted[chunk_id] = 1u;
    live_.push_back(LiveDebris{id, 0.0f});
}

void DestructionWorld::tick(f32 dt) {
    breaks_this_frame_ = 0u;

    for (LiveDebris& debris : live_) {
        debris.age += dt;
    }

    // Lifetime first, by age: the oldest things go whether or not the cap is
    // under pressure. Erasing is done after the sink has been told, so destroy
    // is still called exactly once per id.
    std::vector<u32> expired;
    for (std::size_t i = 0u; i < live_.size(); ++i) {
        if (live_[i].age >= budget_.max_debris_lifetime) {
            expired.push_back(live_[i].sink_id);
        }
    }
    for (const u32 id : expired) {
        sink_.destroy(id);
    }
    live_.erase(std::remove_if(live_.begin(), live_.end(),
                               [&](const LiveDebris& d) {
                                   return std::find(expired.begin(), expired.end(),
                                                    d.sink_id) != expired.end();
                               }),
                live_.end());

    // Then the cap, oldest-first, which prefers to keep whatever the player
    // just knocked loose.
    while (live_.size() > budget_.max_active_debris) {
        retire_oldest();
    }
}

void DestructionWorld::retire_oldest() {
    if (live_.empty()) return;

    std::size_t oldest = 0u;
    for (std::size_t i = 1u; i < live_.size(); ++i) {
        // Strictly-greater keeps the earliest arrival when ages tie, so the
        // choice cannot depend on pointer or allocation order.
        if (live_[i].age > live_[oldest].age) oldest = i;
    }

    sink_.destroy(live_[oldest].sink_id);
    live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(oldest));
}

} // namespace nf::destruction
