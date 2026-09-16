// NF/Networking/Authoritative.cpp — authoritative server + prediction.

#include <NF/Networking/Authoritative.hpp>

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
    return true;
}

// --- NetWorld ---------------------------------------------------------------

NetWorld::NetWorld(float fixed_dt, float speed)
    : m_dt(fixed_dt > 0.0f ? fixed_dt : 1.0f / 60.0f), m_speed(speed) {}

void NetWorld::add_entity(u32 id, float x, float y, float z) {
    m_bodies[id] = Body{x, y, z, 0.0f, 0.0f};
}

void NetWorld::remove_entity(u32 id) {
    m_bodies.erase(id);
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
        e.generation = 0;
        e.x = b.x;
        e.y = b.y;
        e.z = b.z;
        s.entities.push_back(e);
    }
    return s;
}

void NetWorld::apply_snapshot(const Snapshot& snapshot) {
    for (const auto& e : snapshot.entities) {
        auto it = m_bodies.find(e.id);
        if (it == m_bodies.end()) {
            m_bodies[e.id] = Body{e.x, e.y, e.z, 0.0f, 0.0f};
        } else {
            it->second.x = e.x;
            it->second.y = e.y;
            it->second.z = e.z;
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

void AuthoritativeServer::submit_input(u32 entity_id, const NetInput& input) {
    m_inputs[entity_id][input.tick] = input;
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
