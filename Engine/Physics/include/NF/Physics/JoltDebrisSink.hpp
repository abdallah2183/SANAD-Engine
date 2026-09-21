#pragma once

// NF/Physics/JoltDebrisSink.hpp — the IDebrisSink that makes shards real.
//
// NFDestruction is deliberately arithmetic-only behind that seam (see
// DebrisSink.hpp): the budget, the stress accumulation and the shard kinematics
// are all decided without a physics backend, which is what lets the destruction
// suite run on a machine that has no Jolt and no GPU. This file is the other
// half of the contract — the implementation that turns a DebrisSpawn into a
// Jolt convex-hull body, gives it the velocities the fracture math worked out,
// and removes the body again when the budget retires the shard.
//
// It lives in NFPhysics, not NFDestruction, for exactly that reason: no
// implementation that touches a backend may live in the module whose selling
// point is that it needs none. The dependency runs one way only — physics
// implements the interface, and the fracture logic never learns what is
// carrying its shards away.
//
// The sink tracks its own live set rather than reporting world.body_count():
// the budget is a ceiling on *debris*, and the world also holds the level, the
// vehicles and the player. Counting those would retire shards to make room for
// scenery that was never subject to the budget in the first place.
//
// Determinism (Section 372): no RNG, no clock, ids from a counter, bodies
// created in arrival order. Given the same spawns in the same order and a
// fixed dt, Jolt (built JPH_CROSS_PLATFORM_DETERMINISTIC) reproduces the same
// trajectories — the seam keeps the part that must be deterministic in the
// module that is tested for it, and leaves the backend to honour its own
// contract.

#include <NF/Core/Types.hpp>
#include <NF/Destruction/DebrisSink.hpp>
#include <NF/Physics/JoltWorld.hpp>

#include <cstddef>
#include <map>

namespace nf::physics {

/// Surface properties every shard shares. Individual shards differ in mass and
/// velocity — from the asset and the blast — but not in what they are made of,
/// so the material lives here once instead of being copied into every spawn.
struct DebrisSinkConfig {
    f32  friction = 0.6f;
    f32  restitution = 0.05f;
    f32  linear_damping = 0.05f;
    f32  angular_damping = 0.1f;
    /// Shards that come to rest stop simulating. A pile of debris that never
    /// sleeps costs a frame forever for no visible benefit, and the lifetime
    /// budget retires those shards anyway.
    bool allow_sleep = true;
};

class JoltDebrisSink final : public nf::destruction::IDebrisSink {
public:
    /// `world` must outlive the sink: the sink creates bodies in it and removes
    /// them again, so a sink whose world has gone would dereference it on
    /// destruction. The runtime layer owns both and tears the sink down first.
    explicit JoltDebrisSink(JoltWorld& world, DebrisSinkConfig config = DebrisSinkConfig{});
    ~JoltDebrisSink() override;

    JoltDebrisSink(const JoltDebrisSink&) = delete;
    JoltDebrisSink& operator=(const JoltDebrisSink&) = delete;

    /// Creates the shard as a dynamic convex-hull body at the spawn's pose with
    /// the spawn's mass, then applies both velocities (a BodyDesc's velocity
    /// fields are not applied at creation — see JoltWorld). Returns
    /// kInvalidDebris, adding no body, when the world is unusable, the hull is
    /// not a hull (fewer than four points), the mass is not positive, or Jolt
    /// rejected the hull as degenerate. Each refusal is counted by the
    /// destruction world rather than swallowed.
    u32 spawn(const nf::destruction::DebrisSpawn& spawn) override;

    /// Removes the body. Unknown and already-retired ids are a no-op, which is
    /// what makes the retirement path safe to call speculatively.
    void destroy(u32 debris_id) override;

    /// Shards currently live in this sink — not the world's total body count.
    std::size_t active_count() const override;

    /// The body a live shard occupies, or an invalid handle for an unknown or
    /// retired id. A renderer poses the shard mesh from this rather than
    /// maintaining a second mapping from debris id to body.
    JoltBody body_of(u32 debris_id) const;

    /// Ids of every live shard, in ascending order. A renderer walks this and
    /// body_of() to pose debris; the order is the same on every run.
    std::vector<u32> live_ids() const;

    /// Retires every shard still live. Called when a scene unloads so debris
    /// from the previous scene cannot outlive it — and so a test can reset a
    /// world to a known state without rebuilding it.
    void clear();

    const DebrisSinkConfig& config() const { return config_; }

private:
    JoltWorld&                                world_;
    const DebrisSinkConfig                    config_;
    /// Ordered by id, so iteration order cannot depend on allocation or
    /// pointer values — the same spawns always visit the same bodies in the
    /// same order.
    std::map<u32, JoltBody>                   live_;
    u32                                       next_id_ = 1u; // 0 is never handed out
};

} // namespace nf::physics
