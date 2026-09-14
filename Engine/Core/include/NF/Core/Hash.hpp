#pragma once

// NF/Core/Hash.hpp — Hashing utilities

#include <NF/Core/Types.hpp>

#include <cstring>
#include <string_view>

namespace nf {

// FNV-1a 64-bit hash — fast, good distribution for short keys
inline constexpr u64 fnv1a_64(std::string_view str) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (char c : str) {
        hash ^= static_cast<u8>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

inline u64 fnv1a_64(const void* data, usize size) {
    const u8* bytes = static_cast<const u8*>(data);
    u64 hash = 0xcbf29ce484222325ULL;
    for (usize i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// MurmurHash3 32-bit
inline u32 murmur3_32(const void* data, usize size, u32 seed = 0) {
    const u8* bytes = static_cast<const u8*>(data);
    u32 h = seed;
    const u32 c1 = 0xcc9e2d51;
    const u32 c2 = 0x1b873593;

    usize nblocks = size / 4;
    const u32* blocks = reinterpret_cast<const u32*>(bytes);

    for (usize i = 0; i < nblocks; ++i) {
        u32 k = blocks[i];
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;

        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }

    // Tail
    const u8* tail = bytes + nblocks * 4;
    u32 k = 0;
    switch (size & 3) {
        case 3: k ^= tail[2] << 16; [[fallthrough]];
        case 2: k ^= tail[1] << 8;  [[fallthrough]];
        case 1: k ^= tail[0];
            k *= c1;
            k = (k << 15) | (k >> 17);
            k *= c2;
            h ^= k;
            break;
        default: break;
    }

    // Finalize
    h ^= size;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

// Combine hashes (for composite keys)
inline void hash_combine(u64& seed, u64 value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

} // namespace nf
