// NF/Networking/Authoritative.cpp — authoritative server + prediction.

#include <NF/Networking/Authoritative.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf::net {

namespace {

void push_u32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
    out.push_back(static_cast<u8>((v >> 16) & 0xFF));
    out.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

void push_f32(std::vector<u8>& out, float v) {
    u32 u = 0;
    std::memcpy(&u, &v, 4);
    push_u32(out, u);
}

u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

float read_f32(const u8* p) {
    u32 u = read_u32(p);
    float v = 0.0f;
    std::memcpy(&v, &u, 4);
    return v;
}

bool tick_less(u32 a, u32 b) {
    return a != b && static_cast<u32>(b - a) < 0x80000000u;
}

} // namespace

std::vector<u8> encode_input(const NetInput& input) {
    std::vector<u8> out;
    push_u32(out, input.tick);
    push_f32(out, input.move_x);
    push_f32(out, input.move_z);
    out.push_back(input.jump ? u8{1} : u8{0});
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    return out; // 16 bytes
}

bool decode_input(const u8* data, usize size, NetInput& out, std::string& out_error) {
    out = NetInput{};
    if (!data || size != 16) {
        out_error = "net input must be exactly 16 bytes";
        return false;
    }
    out.tick = read_u32(data);
    out.move_x = read_f32(data + 4);
    out.move_z = read_f32(data + 8);
    if (data[12] > 1) {
        out_error = "net input jump flag corrupt";
        return false;
    }
    out.jump = data[12] == 1;
    // A NaN in a movement axis is STICKY: it survives every integration step,
    // so one hostile packet would put NaN into the authoritative position,
    // into every snapshot the server publishes from then on, and into every
    // client that applies one. Every other codec in this module rejects
    // non-finite values for exactly this reason.
    if (!std::isfinite(out.move_x) || !std::isfinite(out.move_z)) {
        out_error = "net input movement not finite";
        out = NetInput{};
        return false;
    }
    // Gameplay only ever writes the axes in [-1, 1] (they are scaled by the
    // movement speed), so anything else is corruption rather than a value to
    // honour: clamp it back into the gameplay range instead of letting a
    // bogus axis drive the player.
    out.move_x = std::clamp(out.move_x, -1.0f, 1.0f);
    out.move_z = std::clamp(out.move_z, -1.0f, 1.0f);
    return true;
}

// --- NetWorld ---------------------------------------------------------------

NetWorld::NetWorld(float fixed_dt, float speed)
    : m_dt(fixed_dt > 0.0f ? fixed_dt : 1.0f / 60.0f), m_speed(speed) {}

void NetWorld::add_entity(u32 id, float x, float y, float z) {
    // The generation counter lives OUTSIDE the body: a removed id that is
    // handed out again is a NEW entity, and the stale snapshot of the old
    // one must still be recognisable as stale.
    const u32 generation = ++m_generations[id];
    m_bodies[id] = Body{x, y, z, 0.0f, 0.0f, generation};
}

void NetWorld::remove_entity(u32 id) {
    m_bodies.erase(id);
    // The generation is NOT forgotten: the id may be handed out again, and
    // the new entity must not be confused with the old one.
}

bool NetWorld::has_entity(u32 id) const {
    return m_bodies.find(id) != m_bodies.end();
}

void NetWorld::apply_input(u32 id, const NetInput& input) {
    auto it = m_bodies.find(id);
    if (it == m_bodies.end()) return;
    it->second.vx = input.move_x * m_speed;
    it->second.vz = input.move_z * m_speed;
}

void NetWorld::step() {
    for (auto& [id, b] : m_bodies) {
        (void)id;
        b.x += b.vx * m_dt;
        b.z += b.vz * m_dt;
    }
}

Snapshot NetWorld::to_snapshot(u32 tick) const {
    Snapshot s;
    s.tick = tick;
    for (const auto& [id, b] : m_bodies) {
        SnapshotEntity e;
        e.id = id;
        e.generation = b.generation;
        e.x = b.x;
        e.y = b.y;
        e.z = b.z;
        s.entities.push_back(e);
    }
    return s;
}

void NetWorld::apply_snapshot(const Snapshot& snapshot) {
    for (const auto& e : snapshot.entities) {
        // The generation this side has ISSUED for the id (0 = never seen).
        u32& issued = m_generations[e.id];
        if (e.generation < issued) {
            // STALE record: the local id was recycled after this snapshot
            // was taken, so the record describes an entity that no longer
            // exists. Applying it would resurrect the old one.
            continue;
        }
        issued = e.generation;
        auto it = m_bodies.find(e.id);
        if (it == m_bodies.end()) {
            // Unknown here: adopt the record as-is, generation included.
            m_bodies[e.id] = Body{e.x, e.y, e.z, 0.0f, 0.0f, e.generation};
        } else {
            it->second.x = e.x;
            it->second.y = e.y;
            it->second.z = e.z;
            it->second.generation = e.generation;
        }
    }
}

bool NetWorld::position(u32 id, float& x, float& y, float& z) const {
    auto it = m_bodies.find(id);
    if (it == m_bodies.end()) return false;
    x = it->second.x;
    y = it->second.y;
    z = it->second.z;
    return true;
}

// --- AuthoritativeServer -----------------------------------------------------

AuthoritativeServer::AuthoritativeServer(float fixed_dt) : m_world(fixed_dt) {}

void AuthoritativeServer::add_player(u32 entity_id, float x, float y, float z) {
    m_world.add_entity(entity_id, x, y, z);
}

bool AuthoritativeServer::submit_input(u32 entity_id, const NetInput& input) {
    // Only a REGISTERED entity may submit. An unknown id would create a
    // permanent entry in the outer map, and a client inventing ids would
    // grow the server's memory one entry at a time.
    if (!m_world.has_entity(entity_id)) return false;
    // Ticks are u32 and wrap, so the window is compared wrap-aware.
    if (tick_less(m_tick, input.tick)) {
        // Too far in the future: refused, never stored. A client that
        // submits tick 10'000'000 must not make the server hold it.
        if (static_cast<u32>(input.tick - m_tick) > kNetInputLeadTicks) return false;
    } else if (tick_less(input.tick, m_tick)) {
        // Too far in the past: late input never rewinds, so it is dropped.
        if (static_cast<u32>(m_tick - input.tick) > kNetInputWindowTicks) return false;
    }
    auto& per_tick = m_inputs[entity_id];
    per_tick[input.tick] = input;
    // Trim the tail: an entry that has fallen out of the window is dead
    // weight, and without this the map would only shrink when the current
    // tick happens to arrive.
    if (per_tick.size() > kNetInputWindowTicks + kNetInputLeadTicks + 1) {
        const u32 oldest_kept = m_tick - kNetInputWindowTicks;
        for (auto it = per_tick.begin(); it != per_tick.end();) {
            if (tick_less(it->first, oldest_kept)) {
                it = per_tick.erase(it);
            } else {
                ++it;
            }
        }
    }
    return true;
}

void AuthoritativeServer::tick() {
    for (auto& [id, per_tick] : m_inputs) {
        auto it = per_tick.find(m_tick);
        if (it != per_tick.end()) {
            m_world.apply_input(id, it->second);
            // Drop this tick and anything older (late input never rewinds).
            per_tick.erase(per_tick.begin(), std::next(it));
        } else {
            m_world.apply_input(id, NetInput{}); // missing input = neutral
        }
    }
    m_world.step();
    m_last_snapshot = m_world.to_snapshot(m_tick);
    ++m_tick;
}

// --- PredictedClient ----------------------------------------------------------

PredictedClient::PredictedClient(u32 entity_id, float fixed_dt)
    : m_entity(entity_id), m_world(fixed_dt) {
    m_world.add_entity(entity_id, 0.0f, 0.0f, 0.0f);
}

void PredictedClient::push_local_input(float move_x, float move_z, bool jump) {
    NetInput in;
    in.tick = m_next_tick++;
    in.move_x = move_x;
    in.move_z = move_z;
    in.jump = jump;
    m_world.apply_input(m_entity, in);
    m_world.step();
    m_history.push_back(in);
    // History cap: 10 seconds at 60Hz. Beyond that the connection is dead
    // and the next snapshot rebuilds everything anyway.
    while (m_history.size() > 600) m_history.erase(m_history.begin());
}

void PredictedClient::on_snapshot(const Snapshot& snapshot) {
    if (m_has_acked && !tick_less(m_acked, snapshot.tick)) {
        return; // stale snapshot: ignore, never rewind
    }
    float px = 0, py = 0, pz = 0;
    m_world.position(m_entity, px, py, pz);
    m_world.apply_snapshot(snapshot);
    float ax = 0, ay = 0, az = 0;
    m_world.position(m_entity, ax, ay, az);
    const float dx = ax - px;
    const float dy = ay - py;
    const float dz = az - pz;
    m_last_correction = std::sqrt(dx * dx + dy * dy + dz * dz);
    m_acked = snapshot.tick;
    m_has_acked = true;
    // Drop acked history, re-simulate the rest on top of authority.
    std::vector<NetInput> remaining;
    for (const auto& in : m_history) {
        if (tick_less(m_acked, in.tick)) remaining.push_back(in);
    }
    m_history = std::move(remaining);
    for (const auto& in : m_history) {
        m_world.apply_input(m_entity, in);
        m_world.step();
    }
}

bool PredictedClient::predicted_position(float& x, float& y, float& z) const {
    return m_world.position(m_entity, x, y, z);
}

} // namespace nf::net
