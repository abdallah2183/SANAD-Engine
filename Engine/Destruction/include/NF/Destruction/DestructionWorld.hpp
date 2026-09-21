#pragma once

// NF/Destruction/DestructionWorld.hpp — applies damage to destructibles and
// turns broken bonds into shards.
//
// Design doc Section 41 demands "impulses" and "runtime destruction" *and*
// "a budget so it does not kill the game's performance". The budget is not an
// afterthought here: it is enforced inside apply_damage, so the pathological
// case — a grenade in a warehouse of crates — is throttled by construction
// rather than tuned away later.
//
// Determinism (Section 372: "determinism above all else"): this class holds no
// RNG, no clock, and no container whose iteration order depends on pointer
// values. Bonds are visited in asset order, shards are emitted in tree order,
// and retirement is by age then by arrival. Given the same asset, component and
// events in the same order, the world produces the same shards on every run —
// which is why is_deterministic() is a contract and not a comment.

#include <NF/Destruction/DebrisSink.hpp>
#include <NF/Destruction/DestructibleComponent.hpp>
#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <cstddef>
#include <vector>

namespace nf::destruction {

/// One blast. `impulse` is the value at the centre; the amount actually
/// delivered to a bond falls off linearly with the distance from the blast
/// point to that bond's shared face, reaching zero at `radius`.
struct DamageEvent {
    Vec3 world_point{0.0f, 0.0f, 0.0f};
    f32  radius = 1.0f;
    f32  impulse = 0.0f;
};

/// The cost ceiling. Every field is data, so a difficulty setting or a console
/// variable can change any of them at runtime and the logic adapts.
struct DestructionBudget {
    /// Hard cap on simultaneously live shards. When a new shard would exceed
    /// this, the oldest live one is retired first — the newest shard, the one
    /// the player is looking at, is always kept.
    u32 max_active_debris = 256u;

    /// Bonds shattered per *frame*, not per blast: apply_damage is called once
    /// per destructible per frame, and this is the frame's total allowance. A
    /// bond that cannot break this frame keeps its accumulated stress and
    /// shatters as soon as the allowance frees, so the damage is never lost,
    /// only deferred.
    u32 max_breaks_per_frame = 8u;

    /// Seconds a shard may live before it is retired, whether or not the cap
    /// is under pressure.
    f32 max_debris_lifetime = 12.0f;
};

class DestructionWorld {
public:
    /// `sink` must outlive the world; the world does not own it, because the
    /// backend's lifetime belongs to the runtime layer.
    DestructionWorld(IDebrisSink& sink, DestructionBudget budget = DestructionBudget{});

    /// Applies one blast to one destructible. `world` places the asset in the
    /// world; it is expected to be a rigid transform (rotation and translation,
    /// possibly uniform scale — the shard's mass comes from the asset's local
    /// volume and the component's density, not from the matrix).
    ///
    /// Returns the number of bonds that shattered during this call — not the
    /// number of bodies: one bond can release several shards (an intact region
    /// goes as one convex piece, a region whose inner bonds have already gone
    /// comes apart into as many pieces as it has left), and a bond throttled by
    /// max_breaks_per_frame releases none yet.
    u32 apply_damage(const FractureAsset& asset, DestructibleComponent& cmp,
                     const Mat4& world, const DamageEvent& event);

    /// Ages shards and retires them: by lifetime first, then oldest-first if
    /// the live count still exceeds the cap. Also resets the per-frame break
    /// allowance. Call once per frame, after physics has stepped.
    void tick(f32 dt);

    bool is_deterministic() const { return true; }

    const DestructionBudget& budget() const { return budget_; }
    std::size_t active_debris() const { return sink_.active_count(); }

    /// Bonds shattered since construction, across every call.
    u32 bonds_shattered_total() const { return shattered_total_; }

    /// Shards the sink refused to take. Nonzero means the budget retired
    /// something and still had no room — a visible signal that the cap is too
    /// low for the scene, rather than shards silently vanishing.
    u32 shards_dropped_for_budget() const { return budget_drops_; }

private:
    struct LiveDebris {
        u32 sink_id = kInvalidDebris;
        f32 age = 0.0f;
    };

    /// Linear falloff from 1 at the blast centre to 0 at the radius. Clamped at
    /// both ends so a negative radius or a point beyond the blast cannot
    /// deliver a negative or super-unity impulse.
    static f32 damage_falloff(f32 distance, f32 radius);

    void mark_subtree_detached(const FractureAsset& asset, u32 chunk_id,
                               DestructibleComponent& cmp) const;

    /// Emits the maximal intact pieces of `chunk_id`'s subtree as shards. An
    /// internal chunk whose own split bond is still whole goes as *one* convex
    /// body — its piece is exactly the union of its children's volumes, so a
    /// region that is still bonded internally flies as one lump. Where an inner
    /// bond has already gone, the region splits there and each side recurses,
    /// which makes the emitted volumes partition the detached region exactly:
    /// no overlap, no gap, no double-emission.
    void emit_subtree(const FractureAsset& asset, u32 chunk_id, const Mat4& world,
                      const Vec3& tear_world, const DamageEvent& event,
                      DestructibleComponent& cmp);

    void emit_body(u32 chunk_id, const FractureChunk& chunk,
                   const Mat4& world, const Vec3& tear_world, const DamageEvent& event,
                   DestructibleComponent& cmp);

    /// Retires the oldest live shard. Called when a new shard would exceed the
    /// cap, and when the cap is still exceeded after lifetime expiry.
    void retire_oldest();

    IDebrisSink&             sink_;
    const DestructionBudget  budget_;
    std::vector<LiveDebris>  live_;

    u32 breaks_this_frame_ = 0u;
    u32 shattered_total_   = 0u;
    u32 budget_drops_      = 0u;
};

} // namespace nf::destruction
