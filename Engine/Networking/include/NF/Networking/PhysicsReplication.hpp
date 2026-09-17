#pragma once

// NF/Networking/PhysicsReplication.hpp — replication for physics-driven objects.
//
// The kinematic player path (NetWorld / AuthoritativeServer / PredictedClient)
// cannot carry Jolt vehicles: a VehicleConstraint is not a constant-velocity
// body, and Jolt ids are world-local. This header adds the three pieces
// vehicle/constraint multiplayer needs, as transport-agnostic pure logic like
// the rest of this module (no sockets, no Jolt — the game layer owns both):
//
//   1. VehicleDriveInput — client→server arcade intent (tick + vehicle net id +
//      throttle/steer/brake). Fixed 20-byte payload with exact-size decode;
//      rides the ReliableChannel exactly like NetInput.
//   2. VehicleSnapshot — server→client chassis truth (tick + per-vehicle
//      position/velocity). Own magic 'NFVS', versioned framing; ships as raw
//      UDP datagrams, loss-tolerant because the next tick supersedes.
//   3. ConstraintEvent — reliable ordered joint spawn/remove/clone (magic
//      'NFCE'). Endpoints are NET entity ids; each side maps net id → local
//      JoltBody, and the server assigns net constraint ids. Bodies' motion
//      already replicates via snapshots, so joints need events, not per-tick
//      state. Must travel over the ReliableChannel (a lost spawn that arrives
//      after its remove would resurrect a dead joint).
//   4. VehicleInputQueue — server-side per-vehicle per-tick input store:
//      tick-matched consume, missing input = neutral, resubmit wins, late
//      input never rewinds (mirrors AuthoritativeServer's input handling).
//   5. VehicleGhostClient — client-side authoritative ghosts: overwrite on
//      snapshot, wrap-aware stale rejection, last_correction() telemetry
//      (mirrors PredictedClient's correction reporting).
//
// Wiring recipe (game layer; this module never sees a JoltWorld):
//   server: queue.submit_drive_input(e, in) → each fixed tick:
//     queue.consume(vehicle, tick, drive) into VehicleComponent
//     {throttle, steer, brake} → VehicleSystem::update + world.step() →
//     VehicleSnapshot from JoltVehicle::chassis_state() → encode + UDP send.
//   client: input mapper → VehicleDriveInput → send_reliable; on each datagram
//     → decode + ghost.apply_vehicle_snapshot() → pose ghost transforms from
//     ghost.state(). ConstraintEvent bytes ride the same reliable channel as
//     the drive inputs; applying one is the matching JoltWorld::add_* call
//     with the endpoints resolved through the net-id → JoltBody map.
//
// Vehicles are server-authoritative WITHOUT client prediction: a Jolt
// constraint step is not replayable from inputs alone on a second world, so
// the client applies authority directly. Drive inputs still feel responsive
// because they are sent unbuffered every tick. Constraint recreation is
// exact: a spawn event carries the same world-space setup its add_* call
// took (point/axis/anchors/limits), and clone reuses clone_constraint.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::net {

// --- Vehicle drive input (client → server, reliable channel payload) ------

/// Arcade intent for one vehicle on one tick. Ranges mirror VehicleComponent:
/// throttle/steer in [-1, 1], brake in [0, 1]. Out-of-range or non-finite
/// values are corruption: decode rejects them (apply-time clamping in
/// vehicle_drive stays as defense in depth, not as the protocol).
struct VehicleDriveInput {
    u32 tick = 0;
    u32 entity = 0; // net id of the driven vehicle (never 0)
    float throttle = 0.0f;
    float steer = 0.0f;
    float brake = 0.0f;

    bool operator==(const VehicleDriveInput& o) const {
        return tick == o.tick && entity == o.entity && throttle == o.throttle &&
               steer == o.steer && brake == o.brake;
    }
};

/// Fixed 20-byte encoding (little-endian): tick(4) entity(4) throttle(4)
/// steer(4) brake(4).
std::vector<u8> encode_drive_input(const VehicleDriveInput& input);
bool decode_drive_input(const u8* data, usize size, VehicleDriveInput& out,
                        std::string& out_error);

// --- Vehicle snapshot (server → client, raw UDP datagram) ------------------

/// Authoritative chassis state for one vehicle. Mirrors JoltBodyState
/// (position + linear velocity); orientation is NOT carried — the game layer
/// keeps heading on its ghost Transform (or derives it from velocity), so a
/// snapshot stays a flat pod the renderer can apply without touching Jolt.
struct VehicleNetState {
    u32 entity = 0; // net id of the vehicle (never 0)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;

    bool operator==(const VehicleNetState& o) const {
        return entity == o.entity && x == o.x && y == o.y && z == o.z &&
               vx == o.vx && vy == o.vy && vz == o.vz;
    }
};

struct VehicleSnapshot {
    u32 tick = 0;
    std::vector<VehicleNetState> vehicles; // sorted by entity on encode
};

/// Deterministic bytes: magic 'NFVS'(4) version(u16) tick(4) count(4) +
/// 28-byte records (entity + 6 floats).
std::vector<u8> encode_vehicle_snapshot(const VehicleSnapshot& snapshot);
bool decode_vehicle_snapshot(const u8* data, usize size, VehicleSnapshot& out,
                             std::string& out_error);

// --- Constraint events (reliable channel payload) --------------------------

enum class ConstraintOp : u8 {
    Spawn = 1,
    Remove = 2,
    Clone = 3,
};

/// Joint flavors, matching JoltWorld::add_* one to one (SixDOF carries the
/// full 12-float limit set; Fixed carries no parameters).
enum class ConstraintKind : u8 {
    Fixed = 1,
    Point = 2,
    Hinge = 3,
    Slider = 4,
    Distance = 5,
    Cone = 6,
    SixDOF = 7,
};

/// One joint lifecycle event. Only the fields its (op, kind) needs are
/// encoded; the rest must be zero (decode rejects nonzero padding so a
/// future field cannot smuggle meaning through an old build):
///   Spawn: net_id + kind + body_a/body_b + kind params (point/axis,
///     anchors + min/max, cone angle, or the 12 SixDOF limits).
///   Remove: net_id only (kind must be Fixed — the value is ignored — and
///     every other field zero).
///   Clone: net_id (the NEW joint) + source (template net constraint id) +
///     body_a/body_b (the new pair); kind params unused.
/// All body references are net entity ids (never 0); spawn/clone require
/// body_a != body_b, mirroring the JoltWorld refusal.
struct ConstraintEvent {
    ConstraintOp op = ConstraintOp::Spawn;
    ConstraintKind kind = ConstraintKind::Fixed;
    u32 net_id = 0;   // server-assigned joint id (spawn/clone) or target (remove)
    u32 body_a = 0;   // net entity ids (spawn/clone)
    u32 body_b = 0;
    u32 source = 0;   // template joint net id (clone only)
    float point[3] = {0, 0, 0}; // hinge/point/slider/cone anchor, sixdof center
    float axis[3] = {0, 0, 0};  // hinge/slider/cone axis
    float anchor_b[3] = {0, 0, 0}; // distance: second anchor
    float param0 = 0.0f; // distance min_dist / cone max_angle_rad
    float param1 = 0.0f; // distance max_dist
    float limits_min[6] = {0, 0, 0, 0, 0, 0}; // sixdof only
    float limits_max[6] = {0, 0, 0, 0, 0, 0};

    bool operator==(const ConstraintEvent& o) const;
};

/// Deterministic bytes: magic 'NFCE'(4) version(u16) op(1) kind(1) + the
/// op/kind payload (12–80 bytes total). Exact-size decode per (op, kind).
std::vector<u8> encode_constraint_event(const ConstraintEvent& event);
bool decode_constraint_event(const u8* data, usize size, ConstraintEvent& out,
                             std::string& out_error);

// --- Server-side drive-input queue ------------------------------------------

/// Per-vehicle per-tick arcade intent store. consume() hands the game exactly
/// one tick's input per vehicle (missing = neutral zeros); resubmission
/// replaces (latest intent for a tick wins); ticks at or below the consumed
/// one are dropped so late input never rewinds the simulation.
class VehicleInputQueue {
public:
    void submit_drive_input(u32 entity, const VehicleDriveInput& input);
    /// Returns true when a real input matched (entity, tick); false leaves
    /// `out` neutral (zeros, tick/entity filled in for bookkeeping).
    bool consume(u32 entity, u32 tick, VehicleDriveInput& out);
    usize pending(u32 entity) const;

private:
    std::unordered_map<u32, std::map<u32, VehicleDriveInput>> m_inputs;
};

// --- Client-side authoritative ghosts ----------------------------------------

/// Server truth as the client sees it: one entry per replicated vehicle,
/// overwritten whole on every snapshot (no prediction — see header note).
class VehicleGhostClient {
public:
    explicit VehicleGhostClient(u32 focus_entity);

    /// Authoritative overwrite + correction telemetry. Stale snapshots
    /// (tick <= last applied, wrap-aware) are ignored and report 0.
    void apply_vehicle_snapshot(const VehicleSnapshot& snapshot);
    bool state(u32 entity, VehicleNetState& out) const;
    usize ghost_count() const { return m_ghosts.size(); }

    u32 focus() const { return m_focus; }
    u32 last_tick() const { return m_acked; }
    bool has_snapshot() const { return m_has_acked; }
    /// Positional snap distance the focused ghost moved under the last
    /// APPLIED snapshot (0 when the snapshot was stale or the ghost is new).
    float last_correction() const { return m_last_correction; }

private:
    u32 m_focus = 0;
    std::unordered_map<u32, VehicleNetState> m_ghosts;
    u32 m_acked = 0;
    bool m_has_acked = false;
    float m_last_correction = 0.0f;
};

} // namespace nf::net
