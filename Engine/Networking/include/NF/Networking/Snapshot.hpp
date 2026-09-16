#pragma once

// NF/Networking/Snapshot.hpp — entity state snapshot framing (Phase 16).
//
// A snapshot is one tick's authoritative world slice: tick id + a flat list
// of (entity id, generation, position) records in deterministic order. The
// framing is fixed-size little-endian records with a magic + version, so a
// corrupted or truncated datagram fails loudly instead of Respawning garbage.
// Rotation/compression ride in later versions; the framing already versions.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nf::net {

struct SnapshotEntity {
    u32 id = 0;
    u32 generation = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;

    bool operator==(const SnapshotEntity& o) const {
        return id == o.id && generation == o.generation && x == o.x && y == o.y && z == o.z;
    }
};

struct Snapshot {
    u32 tick = 0;
    std::vector<SnapshotEntity> entities; // sorted by (id, generation) on encode
};

/// Deterministic bytes: magic(4) version(2) tick(4) count(4) + 20B records.
std::vector<u8> encode_snapshot(const Snapshot& snapshot);
bool decode_snapshot(const u8* data, usize size, Snapshot& out, std::string& out_error);

} // namespace nf::net
