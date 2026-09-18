// NF/Networking/PhysicsReplication.cpp — vehicle/constraint replication codecs.

#include <NF/Networking/PhysicsReplication.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf::net {

namespace {

void push_u8(std::vector<u8>& out, u8 v) {
    out.push_back(v);
}

void push_u16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

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

void push_vec3(std::vector<u8>& out, const float v[3]) {
    push_f32(out, v[0]);
    push_f32(out, v[1]);
    push_f32(out, v[2]);
}

u16 read_u16(const u8* p) {
    return static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
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

void read_vec3(const u8* p, float v[3]) {
    v[0] = read_f32(p);
    v[1] = read_f32(p + 4);
    v[2] = read_f32(p + 8);
}

bool finite3(const float v[3]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool nonzero3(const float v[3]) {
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) > 0.0f;
}

bool tick_less(u32 a, u32 b) {
    return a != b && static_cast<u32>(b - a) < 0x80000000u;
}

// A quaternion is only a rotation while it is unit length; anything else
// scales and skews every transform it touches. The tolerance is on the
// SQUARED length, so checking it costs no sqrt.
constexpr float kUnitQuatTolerance = 1e-3f;

bool is_unit_quat(float x, float y, float z, float w) {
    const float len2 = x * x + y * y + z * z + w * w;
    return std::fabs(len2 - 1.0f) <= kUnitQuatTolerance;
}

/// Angle between two orientations, radians [0, pi]. q and -q are the SAME
/// orientation, so the sign is folded before anything is measured.
///
/// 4*atan2(|a-b|, |a+b|) and not the textbook 2*acos(|dot|): acos has an
/// infinite derivative at 1, so two quaternions whose dot differs from 1 by
/// float noise (1e-8, i.e. a heading that never moved) still reported ~3e-4
/// rad of phantom correction. atan2 is well conditioned at both ends, needs
/// no clamp, and cannot produce NaN from a dot of 1+epsilon.
float quat_angle(float ax, float ay, float az, float aw, float bx, float by, float bz,
                 float bw) {
    const float dot = ax * bx + ay * by + az * bz + aw * bw;
    const float s = (dot < 0.0f) ? -1.0f : 1.0f; // fold q onto -q
    const float dx = ax - s * bx, dy = ay - s * by, dz = az - s * bz, dw = aw - s * bw;
    const float sx = ax + s * bx, sy = ay + s * by, sz = az + s * bz, sw = aw + s * bw;
    const float diff = std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw); // 2*sin(phi/2)
    const float sum = std::sqrt(sx * sx + sy * sy + sz * sz + sw * sw);  // 2*cos(phi/2)
    return 4.0f * std::atan2(diff, sum);
}

} // namespace

// --- Vehicle drive input ----------------------------------------------------

std::vector<u8> encode_drive_input(const VehicleDriveInput& input) {
    std::vector<u8> out;
    push_u32(out, input.tick);
    push_u32(out, input.entity);
    push_f32(out, input.throttle);
    push_f32(out, input.steer);
    push_f32(out, input.brake);
    return out; // 20 bytes
}

bool decode_drive_input(const u8* data, usize size, VehicleDriveInput& out,
                        std::string& out_error) {
    out = VehicleDriveInput{};
    if (!data || size != 20) {
        out_error = "drive input must be exactly 20 bytes";
        return false;
    }
    const u32 entity = read_u32(data + 4);
    const float throttle = read_f32(data + 8);
    const float steer = read_f32(data + 12);
    const float brake = read_f32(data + 16);
    if (entity == 0) {
        out_error = "drive input entity id is zero";
        return false;
    }
    // Arcade range is the protocol: gameplay only ever writes [-1, 1] /
    // [0, 1] (see VehicleComponent), so anything else is corruption, not a
    // value to clamp. Non-finite would poison the simulation outright.
    if (!std::isfinite(throttle) || !std::isfinite(steer) || !std::isfinite(brake)) {
        out_error = "drive input not finite";
        return false;
    }
    if (throttle < -1.0f || throttle > 1.0f || steer < -1.0f || steer > 1.0f ||
        brake < 0.0f || brake > 1.0f) {
        out_error = "drive input out of arcade range";
        return false;
    }
    out.tick = read_u32(data);
    out.entity = entity;
    out.throttle = throttle;
    out.steer = steer;
    out.brake = brake;
    return true;
}

// --- Vehicle snapshot -------------------------------------------------------

std::vector<u8> encode_vehicle_snapshot(const VehicleSnapshot& snapshot) {
    std::vector<VehicleNetState> sorted = snapshot.vehicles;
    std::sort(sorted.begin(), sorted.end(),
              [](const VehicleNetState& a, const VehicleNetState& b) {
                  return a.entity < b.entity;
              });
    std::vector<u8> out;
    out.push_back('N');
    out.push_back('F');
    out.push_back('V');
    out.push_back('S');
    push_u16(out, 2); // version (2 = records carry orientation)
    push_u32(out, snapshot.tick);
    push_u32(out, static_cast<u32>(sorted.size()));
    for (const auto& v : sorted) {
        push_u32(out, v.entity);
        push_f32(out, v.x);
        push_f32(out, v.y);
        push_f32(out, v.z);
        push_f32(out, v.vx);
        push_f32(out, v.vy);
        push_f32(out, v.vz);
        push_f32(out, v.qx);
        push_f32(out, v.qy);
        push_f32(out, v.qz);
        push_f32(out, v.qw);
    }
    return out;
}

bool decode_vehicle_snapshot(const u8* data, usize size, VehicleSnapshot& out,
                             std::string& out_error) {
    out = VehicleSnapshot{};
    // Header: magic(4) version(2) tick(4) count(4) = 14 bytes. Version 2
    // widened the record from 28 to 44 bytes by appending the orientation
    // quaternion; version 1 is not silently accepted, it is refused.
    constexpr usize kHeader = 14;
    constexpr usize kRecord = 44;
    if (!data || size < kHeader) {
        out_error = "vehicle snapshot too short";
        return false;
    }
    if (data[0] != 'N' || data[1] != 'F' || data[2] != 'V' || data[3] != 'S') {
        out_error = "not a vehicle snapshot (bad magic)";
        return false;
    }
    if (read_u16(data + 4) != 2) {
        out_error = "unsupported vehicle snapshot version";
        return false;
    }
    const u32 count = read_u32(data + 10);
    if (count > 10000) {
        out_error = "vehicle snapshot count absurd";
        return false;
    }
    if (size != kHeader + static_cast<usize>(count) * kRecord) {
        out_error = "vehicle snapshot size mismatch";
        return false;
    }
    out.tick = read_u32(data + 6);
    for (u32 i = 0; i < count; ++i) {
        const u8* p = data + kHeader + i * kRecord;
        VehicleNetState v;
        v.entity = read_u32(p);
        v.x = read_f32(p + 4);
        v.y = read_f32(p + 8);
        v.z = read_f32(p + 12);
        v.vx = read_f32(p + 16);
        v.vy = read_f32(p + 20);
        v.vz = read_f32(p + 24);
        v.qx = read_f32(p + 28);
        v.qy = read_f32(p + 32);
        v.qz = read_f32(p + 36);
        v.qw = read_f32(p + 40);
        if (v.entity == 0) {
            out_error = "vehicle snapshot entity id is zero";
            out = VehicleSnapshot{};
            return false;
        }
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
            !std::isfinite(v.vx) || !std::isfinite(v.vy) || !std::isfinite(v.vz) ||
            !std::isfinite(v.qx) || !std::isfinite(v.qy) || !std::isfinite(v.qz) ||
            !std::isfinite(v.qw)) {
            out_error = "vehicle snapshot not finite";
            out = VehicleSnapshot{};
            return false;
        }
        // A quaternion that is not unit length is not a rotation: applied as
        // one it scales and skews the chassis. Normalizing here would be the
        // silent repair this module refuses to make.
        if (!is_unit_quat(v.qx, v.qy, v.qz, v.qw)) {
            out_error = "vehicle snapshot orientation is not a unit quaternion";
            out = VehicleSnapshot{};
            return false;
        }
        out.vehicles.push_back(v);
    }
    return true;
}

// --- Constraint events ------------------------------------------------------

bool ConstraintEvent::operator==(const ConstraintEvent& o) const {
    if (op != o.op || kind != o.kind || net_id != o.net_id || body_a != o.body_a ||
        body_b != o.body_b || source != o.source || param0 != o.param0 ||
        param1 != o.param1) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (point[i] != o.point[i] || axis[i] != o.axis[i] ||
            anchor_b[i] != o.anchor_b[i]) {
            return false;
        }
    }
    for (int i = 0; i < 6; ++i) {
        if (limits_min[i] != o.limits_min[i] || limits_max[i] != o.limits_max[i]) {
            return false;
        }
    }
    return true;
}

namespace {

// Exact wire size for a canonical (op, kind); 0 = illegal combination.
usize constraint_event_size(ConstraintOp op, ConstraintKind kind) {
    constexpr usize kHeader = 8; // magic(4) version(2) op(1) kind(1)
    if (op == ConstraintOp::Remove) {
        return kind == ConstraintKind::Fixed ? kHeader + 4 : 0;
    }
    if (op == ConstraintOp::Clone) {
        return kind == ConstraintKind::Fixed ? kHeader + 16 : 0;
    }
    if (op != ConstraintOp::Spawn) return 0;
    constexpr usize kIds = 12; // net_id(4) body_a(4) body_b(4)
    switch (kind) {
    case ConstraintKind::Fixed: return kHeader + kIds;
    case ConstraintKind::Point: return kHeader + kIds + 12;
    case ConstraintKind::Hinge:
    case ConstraintKind::Slider: return kHeader + kIds + 24;
    case ConstraintKind::Distance: return kHeader + kIds + 32;
    case ConstraintKind::Cone: return kHeader + kIds + 28;
    case ConstraintKind::SixDOF: return kHeader + kIds + 60;
    }
    return 0;
}

} // namespace

std::vector<u8> encode_constraint_event(const ConstraintEvent& event) {
    std::vector<u8> out;
    out.push_back('N');
    out.push_back('F');
    out.push_back('C');
    out.push_back('E');
    push_u16(out, 1); // version
    push_u8(out, static_cast<u8>(event.op));
    push_u8(out, static_cast<u8>(event.kind));
    if (event.op == ConstraintOp::Remove) {
        push_u32(out, event.net_id);
        return out;
    }
    if (event.op == ConstraintOp::Clone) {
        push_u32(out, event.net_id);
        push_u32(out, event.source);
        push_u32(out, event.body_a);
        push_u32(out, event.body_b);
        return out;
    }
    push_u32(out, event.net_id);
    push_u32(out, event.body_a);
    push_u32(out, event.body_b);
    switch (event.kind) {
    case ConstraintKind::Fixed:
        break;
    case ConstraintKind::Point:
        push_vec3(out, event.point);
        break;
    case ConstraintKind::Hinge:
    case ConstraintKind::Slider:
        push_vec3(out, event.point);
        push_vec3(out, event.axis);
        break;
    case ConstraintKind::Distance:
        push_vec3(out, event.point); // anchor_a
        push_vec3(out, event.anchor_b);
        push_f32(out, event.param0); // min_dist
        push_f32(out, event.param1); // max_dist
        break;
    case ConstraintKind::Cone:
        push_vec3(out, event.point);
        push_vec3(out, event.axis);
        push_f32(out, event.param0); // max_angle_rad
        break;
    case ConstraintKind::SixDOF:
        push_vec3(out, event.point);
        for (int i = 0; i < 6; ++i) push_f32(out, event.limits_min[i]);
        for (int i = 0; i < 6; ++i) push_f32(out, event.limits_max[i]);
        break;
    }
    return out;
}

bool decode_constraint_event(const u8* data, usize size, ConstraintEvent& out,
                             std::string& out_error) {
    out = ConstraintEvent{};
    constexpr usize kHeader = 8;
    if (!data || size < kHeader) {
        out_error = "constraint event too short";
        return false;
    }
    if (data[0] != 'N' || data[1] != 'F' || data[2] != 'C' || data[3] != 'E') {
        out_error = "not a constraint event (bad magic)";
        return false;
    }
    if (read_u16(data + 4) != 1) {
        out_error = "unsupported constraint event version";
        return false;
    }
    const u8 op_raw = data[6];
    const u8 kind_raw = data[7];
    if (op_raw < 1 || op_raw > 3) {
        out_error = "unknown constraint op";
        return false;
    }
    if (kind_raw < 1 || kind_raw > 7) {
        out_error = "unknown constraint kind";
        return false;
    }
    const auto op = static_cast<ConstraintOp>(op_raw);
    const auto kind = static_cast<ConstraintKind>(kind_raw);
    if (size != constraint_event_size(op, kind)) {
        out_error = "constraint event size mismatch";
        return false;
    }
    out.op = op;
    out.kind = kind;
    const u8* p = data + kHeader;
    if (op == ConstraintOp::Remove) {
        out.net_id = read_u32(p);
        if (out.net_id == 0) {
            out_error = "constraint remove targets id zero";
            out = ConstraintEvent{};
            return false;
        }
        return true;
    }
    if (op == ConstraintOp::Clone) {
        out.net_id = read_u32(p);
        out.source = read_u32(p + 4);
        out.body_a = read_u32(p + 8);
        out.body_b = read_u32(p + 12);
        if (out.net_id == 0 || out.source == 0 || out.body_a == 0 || out.body_b == 0) {
            out_error = "constraint clone has a zero id";
            out = ConstraintEvent{};
            return false;
        }
        if (out.body_a == out.body_b) {
            out_error = "constraint clone needs two bodies";
            out = ConstraintEvent{};
            return false;
        }
        return true;
    }
    // Spawn.
    out.net_id = read_u32(p);
    out.body_a = read_u32(p + 4);
    out.body_b = read_u32(p + 8);
    if (out.net_id == 0 || out.body_a == 0 || out.body_b == 0) {
        out_error = "constraint spawn has a zero id";
        out = ConstraintEvent{};
        return false;
    }
    if (out.body_a == out.body_b) {
        out_error = "constraint spawn needs two bodies";
        out = ConstraintEvent{};
        return false;
    }
    p += 12;
    switch (kind) {
    case ConstraintKind::Fixed:
        break;
    case ConstraintKind::Point:
        read_vec3(p, out.point);
        if (!finite3(out.point)) {
            out_error = "constraint point not finite";
            out = ConstraintEvent{};
            return false;
        }
        break;
    case ConstraintKind::Hinge:
    case ConstraintKind::Slider:
        read_vec3(p, out.point);
        read_vec3(p + 12, out.axis);
        if (!finite3(out.point) || !finite3(out.axis)) {
            out_error = "constraint hinge/slider not finite";
            out = ConstraintEvent{};
            return false;
        }
        if (!nonzero3(out.axis)) {
            out_error = "constraint axis is degenerate";
            out = ConstraintEvent{};
            return false;
        }
        break;
    case ConstraintKind::Distance:
        read_vec3(p, out.point);
        read_vec3(p + 12, out.anchor_b);
        out.param0 = read_f32(p + 24);
        out.param1 = read_f32(p + 28);
        if (!finite3(out.point) || !finite3(out.anchor_b) ||
            !std::isfinite(out.param0) || !std::isfinite(out.param1)) {
            out_error = "constraint distance not finite";
            out = ConstraintEvent{};
            return false;
        }
        if (out.param0 < 0.0f || out.param1 < out.param0) {
            out_error = "constraint distance limits inverted";
            out = ConstraintEvent{};
            return false;
        }
        break;
    case ConstraintKind::Cone:
        read_vec3(p, out.point);
        read_vec3(p + 12, out.axis);
        out.param0 = read_f32(p + 24);
        if (!finite3(out.point) || !finite3(out.axis) || !std::isfinite(out.param0)) {
            out_error = "constraint cone not finite";
            out = ConstraintEvent{};
            return false;
        }
        if (!nonzero3(out.axis)) {
            out_error = "constraint axis is degenerate";
            out = ConstraintEvent{};
            return false;
        }
        if (out.param0 < 0.0f || out.param0 > 3.14159274f) {
            out_error = "constraint cone angle out of range";
            out = ConstraintEvent{};
            return false;
        }
        break;
    case ConstraintKind::SixDOF:
        read_vec3(p, out.point);
        for (int i = 0; i < 6; ++i) out.limits_min[i] = read_f32(p + 12 + i * 4);
        for (int i = 0; i < 6; ++i) out.limits_max[i] = read_f32(p + 36 + i * 4);
        if (!finite3(out.point)) {
            out_error = "constraint sixdof center not finite";
            out = ConstraintEvent{};
            return false;
        }
        for (int i = 0; i < 6; ++i) {
            if (!std::isfinite(out.limits_min[i]) || !std::isfinite(out.limits_max[i])) {
                out_error = "constraint sixdof limits not finite";
                out = ConstraintEvent{};
                return false;
            }
        }
        break;
    }
    return true;
}

// --- Server-side drive-input queue ------------------------------------------

void VehicleInputQueue::submit_drive_input(u32 entity, const VehicleDriveInput& input) {
    m_inputs[entity][input.tick] = input;
}

bool VehicleInputQueue::consume(u32 entity, u32 tick, VehicleDriveInput& out) {
    out = VehicleDriveInput{};
    out.tick = tick;
    out.entity = entity;
    auto it = m_inputs.find(entity);
    if (it == m_inputs.end()) return false;
    auto& per_tick = it->second;
    const auto hit = per_tick.find(tick);
    if (hit == per_tick.end()) return false;
    out = hit->second;
    // Drop this tick and anything older (late input never rewinds).
    per_tick.erase(per_tick.begin(), std::next(hit));
    return true;
}

usize VehicleInputQueue::pending(u32 entity) const {
    const auto it = m_inputs.find(entity);
    return it == m_inputs.end() ? 0 : it->second.size();
}

// --- Client-side authoritative ghosts ----------------------------------------

VehicleGhostClient::VehicleGhostClient(u32 focus_entity) : m_focus(focus_entity) {}

void VehicleGhostClient::apply_vehicle_snapshot(const VehicleSnapshot& snapshot) {
    if (m_has_acked && !tick_less(m_acked, snapshot.tick)) {
        return; // stale snapshot: ignore, never rewind
    }
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float pqx = 0.0f, pqy = 0.0f, pqz = 0.0f, pqw = 1.0f;
    bool had_focus = false;
    const auto prev = m_ghosts.find(m_focus);
    if (prev != m_ghosts.end()) {
        px = prev->second.x;
        py = prev->second.y;
        pz = prev->second.z;
        pqx = prev->second.qx;
        pqy = prev->second.qy;
        pqz = prev->second.qz;
        pqw = prev->second.qw;
        had_focus = true;
    }
    for (const auto& v : snapshot.vehicles) {
        m_ghosts[v.entity] = v;
    }
    m_acked = snapshot.tick;
    m_has_acked = true;
    const auto cur = m_ghosts.find(m_focus);
    if (cur == m_ghosts.end() || !had_focus) {
        m_last_correction = 0.0f;
        m_last_angle = 0.0f; // brand-new ghost: nothing to snap from
        return;
    }
    const float dx = cur->second.x - px;
    const float dy = cur->second.y - py;
    const float dz = cur->second.z - pz;
    m_last_correction = std::sqrt(dx * dx + dy * dy + dz * dz);
    m_last_angle = quat_angle(pqx, pqy, pqz, pqw, cur->second.qx, cur->second.qy,
                              cur->second.qz, cur->second.qw);
}

bool VehicleGhostClient::state(u32 entity, VehicleNetState& out) const {
    const auto it = m_ghosts.find(entity);
    if (it == m_ghosts.end()) return false;
    out = it->second;
    return true;
}

} // namespace nf::net
