// NF/Networking/Snapshot.cpp — snapshot framing.

#include <NF/Networking/Snapshot.hpp>

#include <algorithm>
#include <cstring>

namespace nf::net {

namespace {

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

} // namespace

std::vector<u8> encode_snapshot(const Snapshot& snapshot) {
    std::vector<SnapshotEntity> sorted = snapshot.entities;
    std::sort(sorted.begin(), sorted.end(), [](const SnapshotEntity& a, const SnapshotEntity& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.generation < b.generation;
    });
    std::vector<u8> out;
    out.push_back('N');
    out.push_back('F');
    out.push_back('S');
    out.push_back('N');
    push_u16(out, 1); // version
    push_u32(out, snapshot.tick);
    push_u32(out, static_cast<u32>(sorted.size()));
    for (const auto& e : sorted) {
        push_u32(out, e.id);
        push_u32(out, e.generation);
        push_f32(out, e.x);
        push_f32(out, e.y);
        push_f32(out, e.z);
    }
    return out;
}

bool decode_snapshot(const u8* data, usize size, Snapshot& out, std::string& out_error) {
    out = Snapshot{};
    // Header: magic(4) version(2) tick(4) count(4) = 14 bytes; record = 20 bytes.
    constexpr usize kHeader = 14;
    constexpr usize kRecord = 20;
    if (!data || size < kHeader) {
        out_error = "snapshot too short";
        return false;
    }
    if (data[0] != 'N' || data[1] != 'F' || data[2] != 'S' || data[3] != 'N') {
        out_error = "not a snapshot (bad magic)";
        return false;
    }
    if (read_u16(data + 4) != 1) {
        out_error = "unsupported snapshot version";
        return false;
    }
    const u32 count = read_u32(data + 10);
    if (count > 100000) {
        out_error = "snapshot entity count absurd";
        return false;
    }
    if (size != kHeader + static_cast<usize>(count) * kRecord) {
        out_error = "snapshot size mismatch";
        return false;
    }
    out.tick = read_u32(data + 6);
    for (u32 i = 0; i < count; ++i) {
        const u8* p = data + kHeader + i * kRecord;
        SnapshotEntity e;
        e.id = read_u32(p);
        e.generation = read_u32(p + 4);
        e.x = read_f32(p + 8);
        e.y = read_f32(p + 12);
        e.z = read_f32(p + 16);
        out.entities.push_back(e);
    }
    return true;
}

} // namespace nf::net
