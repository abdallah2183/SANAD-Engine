#pragma once

// NF/Networking/BodyReplication.hpp — net-entity body replication (Phase 17).
//
// Phase 16 shipped the joint lifecycle over the wire (ConstraintEvent,
// PhysicsReplication.hpp): codecs, a reliable-channel round trip, per-tick
// drive inputs and ghost snapshots. What it deliberately did NOT ship was
// the other half of §371's first item — "clone_constraint over the wire"
// needs SOMETHING to clone onto: bodies. A joint event names its endpoints
// as net entity ids, and until those ids resolve to live Jolt bodies on the
// receiving side, a Spawn/Clone event has nowhere to land except the floor.
//
// This header closes that gap with the same discipline the rest of the
// module uses — transport-agnostic PURE LOGIC. It knows nothing about
// sockets (the game layer owns them, exactly like the ConstraintEvent
// wiring recipe documented on PhysicsReplication.hpp) and nothing about
// Jolt types beyond the two handles the physics API deals in everywhere:
//
//   1. BodyRegistry — net entity id <-> JoltBody map, plus the hierarchy
//      priority table from §371 ("priority of the hierarchy table for
//      static bodies"): a static body is booked as scenery once and is then
//      the authoritative answer for its net id, so a late Spawn for an id a
//      dynamic body already holds cannot evict the level geometry. Lookup is
//      O(1) both directions; one net id maps to exactly one body.
//   2. BodyEvent + codec — spawn/remove body lifecycle over the reliable
//      channel (magic 'NFBE'). Spawn carries the net id + the BodyDesc the
//      JoltWorld::add_body call took (type, shape radius, position, mass,
//      friction, restitution, damping, sleep), Remove carries the id only.
//      Exact-size decode per op, as with every other framing here.
//   3. ConstraintReplicator — the applier. Takes a BodyRegistry-resolved
//      stream of BodyEvent + ConstraintEvent in RELIABLE ORDER and issues the
//      matching JoltWorld calls, holding the resulting handles in the same
//      registry. This is the class that makes "clone_constraint over the
//      wire" real: a Clone event resolves its template through the server's
//      net constraint ids and its new endpoints through the registry, then
//      hands the pair to JoltWorld::clone_constraint.
//
// Layering (design §372): this module never includes JoltWorld.hpp and never
// links Jolt. The applier is templated on the world type so tests can drive
// it against a fake, and the production path sees only the four
// add_body/add_*/clone_constraint/remove_constraint signatures the real
// class already exposes.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::net {

// --- Net id <-> body registry -------------------------------------------------

/// One entry in the hierarchy priority table. Static bodies win a net-id
/// collision because level geometry is the authoritative content of the id:
/// see BodyRegistry::resolve_for_spawn.
struct BodyRegistryEntry {
    u32 net_id = 0;          // never 0 in the table
    u32 body_handle = 0;     // the physics handle (0 = unbound)
    u32 generation = 0;      // bindings counter for this id (telemetry)
    bool is_static = false;  // a Static body books the id as scenery
};

class BodyRegistry {
public:
    /// Records a net id -> body mapping. A Spawn whose id is ALREADY held is
    /// not an error (reliable order means the sender's intent is real), but a
    /// Static holder is never evicted by a Dynamic re-spawn — that is the
    /// hierarchy priority: scenery stays scenery. Returns true when the
    /// mapping was stored (new id, or a Dynamic replaced by any kind).
    /// `body_id` 0 is refused (the invalid handle value).
    bool bind(u32 net_id, u32 body_id, bool is_static);

    /// Drops a net id (a Remove event, or the body dying). A no-op for an
    /// unknown id. Ordering comes from the reliable channel, not from the
    /// generation counter (telemetry only): events for one id always arrive
    /// in send order, so a late Spawn is a re-spawn, never a time traveler.
    void unbind(u32 net_id);

    /// Current body handle for a net id (0 when unknown). Generation-free: a
    /// stale holder is the caller's bookkeeping problem, same as the rest of
    /// the module.
    u32 body_of(u32 net_id) const;

    /// True when the net id is currently held.
    bool has(u32 net_id) const { return body_of(net_id) != 0; }

    /// Registry bookkeeping for the applier: the entry, for the priority
    /// decision and generation telemetry.
    bool entry(u32 net_id, BodyRegistryEntry& out) const;

    /// Number of net ids that currently resolve to a body.
    usize size() const { return m_by_net.size(); }

private:
    std::unordered_map<u32, BodyRegistryEntry> m_by_net;
    std::unordered_map<u32, u32> m_net_by_body; // body -> net, for unbind paths
};

// --- Body events (reliable channel payload) ------------------------------------

enum class BodyOp : u8 {
    Spawn = 1,
    Remove = 2,
};

/// One body lifecycle event. Only the fields its op needs are encoded; the
/// rest must be zero (decode rejects nonzero padding, so a future field
/// cannot smuggle meaning through an old build — same rule as
/// ConstraintEvent):
///   Spawn: net_id + type + radius + position + mass + friction +
///     restitution + damping + allow_sleep. Velocity is NOT carried: the
///     BodyDesc contract on JoltWorld is that initial velocity is applied
///     with set_linear_velocity() after creation, and over the wire that
///     arrives via the snapshot channel (the authoritative truth), not here.
///   Remove: net_id only (type must be Static — the value is ignored — and
///     every other field zero).
struct BodyEvent {
    BodyOp op = BodyOp::Spawn;
    u32 net_id = 0; // never 0
    u8 type = 0;    // BodyType as a u8: 0 Static, 1 Dynamic, 2 Kinematic
    float radius = 0.0f;     // sphere radius (Spawn)
    float x = 0.0f, y = 0.0f, z = 0.0f; // spawn position
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.1f;
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    u8 allow_sleep = 1;

    bool operator==(const BodyEvent& o) const;
};

/// Deterministic bytes: magic 'NFBE'(4) version(u16) op(1) type(1) + the
/// op payload (44 bytes for Spawn, 12 for Remove).
std::vector<u8> encode_body_event(const BodyEvent& event);
bool decode_body_event(const u8* data, usize size, BodyEvent& out, std::string& out_error);

} // namespace nf::net
