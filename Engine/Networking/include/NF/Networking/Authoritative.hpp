#pragma once

// NF/Networking/Authoritative.hpp — server-authoritative replication with
// client-side prediction and reconciliation (design doc Sections 65-70).
//
// Model (documented, not accidental):
//   - The SHARED simulation is kinematic and transform-only: every player
//     entity has a constant-speed velocity from its latest input, integrated
//     at a fixed dt. Server and client run the SAME code path, so prediction
//     is exact when no correction intervenes.
//   - Server: collects NetInputs per player, steps the world, publishes a
//     versioned Snapshot per tick. Inputs arrive over the ReliableChannel;
//     snapshots go out as raw UDP datagrams (loss is fine — the next tick
//     supersedes).
//   - Client: applies each local input to its predicted world IMMEDIATELY
//     (zero perceived latency), keeps (tick -> input, position) history, and
//     on each snapshot overwrites with authority then re-simulates newer
//     inputs. last_correction() reports the snap distance (telemetry for
//     lag compensation tuning).
//   - Ticks are u32 and wrap; comparison is wrap-aware like the channel.

#include <NF/Networking/Snapshot.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::net {

/// Player intent for one tick. Fixed 12-byte encoding (little-endian).
struct NetInput {
    u32 tick = 0;
    float move_x = 0.0f;
    float move_z = 0.0f;
    bool jump = false; // reserved: the kinematic demo sim ignores verticality

    bool operator==(const NetInput& o) const {
        return tick == o.tick && move_x == o.move_x && move_z == o.move_z && jump == o.jump;
    }
};

std::vector<u8> encode_input(const NetInput& input);
bool decode_input(const u8* data, usize size, NetInput& out, std::string& out_error);

/// The shared kinematic simulation. Identical code on both ends.
class NetWorld {
public:
    explicit NetWorld(float fixed_dt = 1.0f / 60.0f, float speed = 6.0f);

    void add_entity(u32 id, float x, float y, float z);
    void remove_entity(u32 id);
    bool has_entity(u32 id) const;

    void apply_input(u32 id, const NetInput& input);
    void step(); // integrates one fixed tick for every entity

    Snapshot to_snapshot(u32 tick) const;
    void apply_snapshot(const Snapshot& snapshot); // authoritative overwrite

    bool position(u32 id, float& x, float& y, float& z) const;
    float fixed_dt() const { return m_dt; }

private:
    struct Body {
        float x = 0, y = 0, z = 0;
        float vx = 0, vz = 0;
    };
    float m_dt = 1.0f / 60.0f;
    float m_speed = 6.0f;
    std::unordered_map<u32, Body> m_bodies;
};

/// Authoritative game server: owns the world, consumes inputs, emits snapshots.
class AuthoritativeServer {
public:
    explicit AuthoritativeServer(float fixed_dt = 1.0f / 60.0f);

    void add_player(u32 entity_id, float x, float y, float z);
    /// Queues an input for its tick (re-submission replaces: the latest
    /// intent for a tick wins). Ticks are consumed in order by tick().
    void submit_input(u32 entity_id, const NetInput& input);
    /// Steps exactly one fixed tick, consuming tick-matched inputs
    /// (missing input = neutral), and publishes its snapshot.
    void tick();

    const Snapshot& last_snapshot() const { return m_last_snapshot; }
    u32 tick_index() const { return m_tick; }
    NetWorld& world() { return m_world; }
    // Const overload: a read-only host (a dedicated server reporting state, or
    // a test asserting on it) must not be forced to drop const to look.
    const NetWorld& world() const { return m_world; }

private:
    NetWorld m_world;
    u32 m_tick = 0;
    Snapshot m_last_snapshot;
    std::unordered_map<u32, std::map<u32, NetInput>> m_inputs; // player -> tick -> input
};

/// Predicting client: zero-latency local play with server reconciliation.
class PredictedClient {
public:
    PredictedClient(u32 entity_id, float fixed_dt = 1.0f / 60.0f);

    /// Applies the input to the predicted world NOW and records it for
    /// re-simulation. Tick is assigned monotonically.
    void push_local_input(float move_x, float move_z, bool jump = false);
    /// Authoritative correction + re-simulation of newer inputs.
    void on_snapshot(const Snapshot& snapshot);

    bool predicted_position(float& x, float& y, float& z) const;
    u32 next_tick() const { return m_next_tick; }
    u32 last_acked_tick() const { return m_acked; }
    float last_correction() const { return m_last_correction; }
    NetWorld& world() { return m_world; }

private:
    u32 m_entity = 0;
    NetWorld m_world;
    u32 m_next_tick = 0;
    u32 m_acked = 0; // 0 = nothing acked yet (ticks start at 0; see note)
    bool m_has_acked = false;
    float m_last_correction = 0.0f;
    std::vector<NetInput> m_history; // inputs newer than m_acked, in order
};

} // namespace nf::net
