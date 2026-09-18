#pragma once

// NF/Networking/ConstraintReplicator.hpp — applies net events to a physics
// world (Phase 17).
//
// Phase 16 shipped the bytes: ConstraintEvent spawn/clone/remove codecs, a
// reliable-channel round trip, per-tick vehicle inputs, ghost snapshots.
// What the wire could not do was land anywhere — ConstraintEvent.body_a is
// a NET id, and until Phase 17's BodyRegistry exists, the receiving side has
// no body for it. This class is the missing half of §371's first item:
// "clone_constraint over the wire", end to end.
//
// It is the APPLIER, and it is pure logic:
//   * transport-agnostic — never sees a socket (the game layer owns them,
//     exactly like the ConstraintEvent wiring recipe on
//     PhysicsReplication.hpp); the caller drains the ReliableChannel and
//     hands the payloads here in reliable order.
//   * Jolt-agnostic — templated on the world type and constrained to the
//     signatures JoltWorld already exposes (add_body/add_fixed/add_hinge/
//     .../clone_constraint/remove_constraint), so the networking module
//     keeps linking nothing but ws2_32 and tests can drive a fake world.
//     The production instantiation is nf::physics::JoltWorld.
//
// The protocol rules it enforces (each is a test below):
//   1. ORDER: bodies before their joints. A Spawn/Clone whose endpoints are
//      not yet live is queued, not dropped — the reliable channel guarantees
//      delivery in send order, so a pending body event is always moments
//      behind, and the joint lands the instant its endpoints bind. A joint
//      whose body NEVER arrives fails loudly via pending() telemetry instead
//      of silently vanishing.
//   2. AUTHORITY: the server assigns every net constraint id, and the client
//      trusts that id. A Spawn/Clone whose net_id is already applied is a
//      duplicate (a resent reliable packet) and is ignored — the same
//      idempotency the ghost client applies to snapshots.
//   3. HIERARCHY PRIORITY (§371): a static body books its net id as
//      scenery; a later Dynamic Spawn for the same id cannot evict it.
//
// The result: the joint lifecycle is exact on both ends. A chain built as
// one template hinge + N Clone events is N identical hinges on N pairs of
// bodies, each body and joint replicated by net id, the whole thing driven
// from bytes that travel the reliable channel.

#include <NF/Core/Types.hpp>
#include <NF/Networking/BodyReplication.hpp>
#include <NF/Networking/PhysicsReplication.hpp> // ConstraintEvent / kinds / ops

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nf::net {

/// What one apply() call did. Every field is testable, and the counts double
/// as the telemetry a debug overlay would read (a stuck `pending` that never
/// drains is a missing body event — a wire or authority bug, not a physics
/// one).
struct ApplyResult {
    usize bodies_spawned = 0;
    usize bodies_removed = 0;
    usize constraints_spawned = 0;
    usize constraints_cloned = 0;
    usize constraints_removed = 0;
    usize duplicate_events = 0;  // net constraint id already applied
    usize deferred_events = 0;   // endpoints not live yet -> queued
    usize failed_events = 0;     // the physics world refused (bad params)
};

/// A net id -> local constraint handle map, so a Clone event can resolve its
/// template constraint the way it resolves its bodies: by server-assigned id.
class ConstraintRegistry {
public:
    bool bind(u32 net_constraint_id, u32 constraint_handle);
    void unbind(u32 net_constraint_id);
    u32 handle_of(u32 net_constraint_id) const;
    bool has(u32 net_constraint_id) const { return handle_of(net_constraint_id) != 0; }
    usize size() const { return m_by_net.size(); }

private:
    std::unordered_map<u32, u32> m_by_net;
};

/// The world interface ConstraintApplier uses. Every method matches a
/// JoltWorld signature by name and arity; the real JoltWorld satisfies it
/// without a single adapter line, and a test fake satisfies it too.
struct IReplicatedWorld {
    virtual ~IReplicatedWorld() = default;

    // Bodies: opaque u32 handles instead of JoltBody/JoltConstraint so the
    // networking layer never names a Jolt type. 0 = invalid, exactly the
    // invalid() value of both real handle types.
    virtual u32 add_body_sphere(u8 type, float radius, float x, float y, float z,
                                float mass, float friction, float restitution,
                                float linear_damping, float angular_damping,
                                bool allow_sleep) = 0;
    virtual void remove_body(u32 body) = 0;

    // Two-body constraints, one per ConstraintKind (mirrors JoltWorld::add_*).
    virtual u32 add_fixed(u32 a, u32 b) = 0;
    virtual u32 add_hinge(u32 a, u32 b, float px, float py, float pz,
                          float ax, float ay, float az) = 0;
    virtual u32 add_point(u32 a, u32 b, float px, float py, float pz) = 0;
    virtual u32 add_slider(u32 a, u32 b, float px, float py, float pz,
                           float ax, float ay, float az) = 0;
    virtual u32 add_distance(u32 a, u32 b, float aax, float aay, float aaz,
                             float abx, float aby, float abz,
                             float min_dist, float max_dist) = 0;
    virtual u32 add_cone(u32 a, u32 b, float px, float py, float pz,
                         float ax, float ay, float az, float max_angle) = 0;
    virtual u32 add_sixdof(u32 a, u32 b, float px, float py, float pz,
                           const float limit_min[6], const float limit_max[6]) = 0;

    /// Copies a two-body constraint onto a different pair. Matches
    /// JoltWorld::clone_constraint: keeps type/limits/axis/anchors, refuses
    /// a non-two-body source (a vehicle constraint), returns 0 on failure.
    virtual u32 clone_constraint(u32 source, u32 new_a, u32 new_b) = 0;

    virtual void remove_constraint(u32 handle) = 0;
};

/// Applies BodyEvent + ConstraintEvent streams to an IReplicatedWorld.
///
/// Usage (server side, or a client that owns no authority — the class is
/// role-agnostic, the CALLER decides who may send):
///   ConstraintApplier applier(world);
///   for (auto& payload : channel.receive(packet)) {
///       if (starts_with(payload, "NFBE")) applier.apply_body(payload);
///       if (starts_with(payload, "NFCE")) applier.apply_constraint(payload);
///   }
///   applier.flush_pending(); // joints whose bodies arrived this packet
///
/// LIFECYCLE RULE (read before wiring a sender): a joint belongs to the exact
/// bodies it was built on. When a body is removed — or replaced by a re-spawn
/// under the same net id — every applied joint touching it is torn down with
/// it (world.remove_constraint + unbind, counted as constraints_removed), and
/// deferred joints waiting on it keep waiting. A re-spawned body is a NEW
/// object: the sender re-spawns the joints it still wants. Without this, the
/// registry would hand out handles to joints whose body is gone.
class ConstraintApplier {
public:
    explicit ConstraintApplier(IReplicatedWorld& world);

    /// Decodes and applies ONE body event. Returns false only on a codec
    /// failure (a malformed payload is a protocol bug worth surfacing).
    bool apply_body(const u8* data, usize size, ApplyResult& result, std::string& err);
    bool apply_body(const std::vector<u8>& payload, ApplyResult& result, std::string& err) {
        return apply_body(payload.data(), payload.size(), result, err);
    }

    /// Decodes and applies ONE constraint event. Endpoints not yet live are
    /// queued (deferred_events++) and retried by flush_pending().
    bool apply_constraint(const u8* data, usize size, ApplyResult& result, std::string& err);
    bool apply_constraint(const std::vector<u8>& payload, ApplyResult& result, std::string& err) {
        return apply_constraint(payload.data(), payload.size(), result, err);
    }

    /// Retries every deferred joint whose endpoints have since bound. Call
    /// after each packet, or once per tick — cheap (a small vector scan).
    void flush_pending(ApplyResult& result);

    /// Telemetry: joints waiting on bodies that have not arrived yet.
    usize pending_count() const { return m_pending.size(); }
    /// Telemetry: every net constraint id this side has applied.
    const ConstraintRegistry& constraints() const { return m_constraints; }
    /// Telemetry: every net body id this side has bound.
    const BodyRegistry& bodies() const { return m_bodies; }

private:
    bool endpoints_live(const ConstraintEvent& e) const;
    void apply_constraint_event(const ConstraintEvent& e, ApplyResult& result);
    /// Tears down every applied joint touching `net_body` (joints first, then
    /// the caller removes the body): the world joint referenced the exact body
    /// going away, so keeping the registry entry would hand out a dead
    /// handle to the next Remove/Clone. Counted as constraints_removed.
    void remove_joints_on_body(u32 net_body, ApplyResult& result);

    IReplicatedWorld& m_world;
    BodyRegistry m_bodies;
    ConstraintRegistry m_constraints;
    // Applied joint endpoints by net constraint id: the ownership map that
    // makes remove_joints_on_body possible (a joint belongs to the exact
    // bodies it was built on).
    std::unordered_map<u32, std::pair<u32, u32>> m_joint_bodies;
    std::vector<ConstraintEvent> m_pending;
};

} // namespace nf::net
