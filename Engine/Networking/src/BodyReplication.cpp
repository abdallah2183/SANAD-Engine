// NF/Networking/src/BodyReplication.cpp — net-entity body replication impl.
//
// Byte-for-byte deterministic framing, the same shape as every other codec
// in this module: little-endian, magic + version header, exact-size decode
// per op, and nonzero-padding rejection so an old build cannot be fooled
// into accepting a field it does not know about.

#include <NF/Networking/BodyReplication.hpp>

#include <cmath>
#include <cstring>

namespace nf::net {

namespace {

void push_u8(std::vector<u8>& out, u8 v) {
    out.push_back(v);
}

void push_u16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
}

void push_u32(std::vector<u8>& out, u32 v) {
    for (usize i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>((v >> (8 * i)) & 0xFFu));
    }
}

void push_f32(std::vector<u8>& out, float v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    push_u32(out, bits);
}

u8 read_u8(const u8* p) {
    return p[0];
}

u16 read_u16(const u8* p) {
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
}

u32 read_u32(const u8* p) {
    u32 v = 0;
    for (usize i = 0; i < 4; ++i) {
        v |= static_cast<u32>(p[i]) << (8 * i);
    }
    return v;
}

float read_f32(const u8* p) {
    u32 bits = read_u32(p);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

} // namespace

// --- BodyRegistry --------------------------------------------------------------

bool BodyRegistry::bind(u32 net_id, u32 body_id, bool is_static) {
    if (net_id == 0 || body_id == 0) return false;

    const auto it = m_by_net.find(net_id);
    if (it != m_by_net.end()) {
        // Hierarchy priority: a Static holder is the authoritative content of
        // the net id (level geometry). A Dynamic re-spawn for the same id is
        // refused rather than evicting scenery.
        if (it->second.is_static && !is_static) return false;
    }

    BodyRegistryEntry& entry = m_by_net[net_id];
    if (entry.body_handle != 0 && entry.body_handle != body_id) {
        m_net_by_body.erase(entry.body_handle);
    }
    const u32 previous_generation = entry.generation;
    entry = BodyRegistryEntry{};
    entry.net_id = net_id;
    entry.body_handle = body_id;
    entry.is_static = is_static;
    entry.generation = previous_generation + 1;
    m_net_by_body[body_id] = net_id;
    return true;
}

void BodyRegistry::unbind(u32 net_id) {
    const auto it = m_by_net.find(net_id);
    if (it == m_by_net.end()) return;
    m_net_by_body.erase(it->second.body_handle);
    m_by_net.erase(it); // the net id is free again
}

u32 BodyRegistry::body_of(u32 net_id) const {
    const auto it = m_by_net.find(net_id);
    if (it == m_by_net.end()) return 0;
    return it->second.body_handle;
}

bool BodyRegistry::entry(u32 net_id, BodyRegistryEntry& out) const {
    const auto it = m_by_net.find(net_id);
    if (it == m_by_net.end()) return false;
    out = it->second;
    return true;
}

// --- BodyEvent ------------------------------------------------------------------

bool BodyEvent::operator==(const BodyEvent& o) const {
    return op == o.op && net_id == o.net_id && type == o.type && radius == o.radius &&
           x == o.x && y == o.y && z == o.z && mass == o.mass && friction == o.friction &&
           restitution == o.restitution && linear_damping == o.linear_damping &&
           angular_damping == o.angular_damping && allow_sleep == o.allow_sleep;
}

namespace {

// magic 'NFBE'(4) + version(u16) + op(u8) + type(u8) = 8, then:
//   Spawn: net_id(4) + radius(4) + x/y/z(12) + mass(4) + friction(4) +
//          restitution(4) + linear_damping(4) + angular_damping(4) +
//          allow_sleep(1) = 41
//   Remove: net_id(4)
/// One body lifecycle event record, including the 8-byte header.
usize body_event_size(BodyOp op) {
    switch (op) {
    case BodyOp::Spawn: return 8 + 41;  // 49
    case BodyOp::Remove: return 8 + 4;  // 12
    }
    return 0;
}

} // namespace

std::vector<u8> encode_body_event(const BodyEvent& event) {
    std::vector<u8> out;
    const usize size = body_event_size(event.op);
    if (size == 0) return out;
    out.reserve(size);

    out.push_back('N');
    out.push_back('F');
    out.push_back('B');
    out.push_back('E');
    push_u16(out, 1); // version
    push_u8(out, static_cast<u8>(event.op));
    push_u8(out, event.type);
    push_u32(out, event.net_id);

    if (event.op == BodyOp::Spawn) {
        push_f32(out, event.radius);
        push_f32(out, event.x);
        push_f32(out, event.y);
        push_f32(out, event.z);
        push_f32(out, event.mass);
        push_f32(out, event.friction);
        push_f32(out, event.restitution);
        push_f32(out, event.linear_damping);
        push_f32(out, event.angular_damping);
        push_u8(out, event.allow_sleep);
    }

    return out;
}

bool decode_body_event(const u8* data, usize size, BodyEvent& out, std::string& out_error) {
    out = BodyEvent{};

    if (data == nullptr) {
        out_error = "BodyEvent: null buffer";
        return false;
    }
    if (size < 8) {
        out_error = "BodyEvent: truncated header";
        return false;
    }
    if (!(data[0] == 'N' && data[1] == 'F' && data[2] == 'B' && data[3] == 'E')) {
        out_error = "BodyEvent: bad magic";
        return false;
    }
    const u16 version = read_u16(data + 4);
    if (version != 1) {
        out_error = "BodyEvent: unsupported version";
        return false;
    }
    const BodyOp op = static_cast<BodyOp>(read_u8(data + 6));
    if (op != BodyOp::Spawn && op != BodyOp::Remove) {
        out_error = "BodyEvent: bad op";
        return false;
    }
    out.op = op;
    out.type = read_u8(data + 7);
    out.net_id = read_u32(data + 8);

    if (op == BodyOp::Remove) {
        if (out.type != 0) {
            out_error = "BodyEvent: Remove must carry type 0";
            return false;
        }
        if (size != body_event_size(BodyOp::Remove)) {
            out_error = "BodyEvent: wrong size for Remove";
            return false;
        }
        if (out.net_id == 0) {
            out_error = "BodyEvent: Remove net_id must be nonzero";
            return false;
        }
        return true;
    }

    // Spawn
    if (size != body_event_size(BodyOp::Spawn)) {
        out_error = "BodyEvent: wrong size for Spawn";
        return false;
    }
    if (out.type > 2) {
        out_error = "BodyEvent: bad body type";
        return false;
    }
    if (out.net_id == 0) {
        out_error = "BodyEvent: Spawn net_id must be nonzero";
        return false;
    }
    out.radius = read_f32(data + 12);
    out.x = read_f32(data + 16);
    out.y = read_f32(data + 20);
    out.z = read_f32(data + 24);
    out.mass = read_f32(data + 28);
    out.friction = read_f32(data + 32);
    out.restitution = read_f32(data + 36);
    out.linear_damping = read_f32(data + 40);
    out.angular_damping = read_f32(data + 44);
    out.allow_sleep = read_u8(data + 48);
    // Spawn physics values are validated like every other framing here: a NaN
    // position or a non-positive radius/mass would poison the world on the
    // other side, and allow_sleep is a bool (0/1) on the wire.
    if (!std::isfinite(out.radius) || !std::isfinite(out.x) || !std::isfinite(out.y) ||
        !std::isfinite(out.z) || !std::isfinite(out.mass) || !std::isfinite(out.friction) ||
        !std::isfinite(out.restitution) || !std::isfinite(out.linear_damping) ||
        !std::isfinite(out.angular_damping)) {
        out_error = "BodyEvent: Spawn values not finite";
        out = BodyEvent{};
        return false;
    }
    if (out.radius <= 0.0f || out.mass <= 0.0f) {
        out_error = "BodyEvent: Spawn needs positive radius and mass";
        out = BodyEvent{};
        return false;
    }
    if (out.allow_sleep > 1) {
        out_error = "BodyEvent: allow_sleep must be 0 or 1";
        out = BodyEvent{};
        return false;
    }
    return true;
}

} // namespace nf::net
