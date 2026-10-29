#pragma once

#include <NF/Core/Types.hpp>
#include <vector>
#include <cstddef>

namespace nf::core {

enum class CompressionType : u8 {
    RLE = 0,
    LZSS = 1
};

/// High-speed deterministic in-memory compression and decompression
class Compression {
public:
    // --- Run-Length Encoding (RLE) ---
    /// Fast byte-level run-length compression (ideal for sparse buffers, heightmaps, mask textures)
    static std::vector<u8> compress_rle(const void* src, size_t src_len);
    static bool decompress_rle(const void* src, size_t src_len, std::vector<u8>& dst);

    // --- LZSS (Sliding Window Dictionary) ---
    /// Fast sliding-window dictionary compression (ideal for package chunks, savegame state, network packets)
    static std::vector<u8> compress_lzss(const void* src, size_t src_len);
    static bool decompress_lzss(const void* src, size_t src_len, std::vector<u8>& dst, size_t expected_size = 0);

    /// Unified compression wrapper
    static std::vector<u8> compress(const void* src, size_t src_len, CompressionType type = CompressionType::LZSS);
    static bool decompress(const void* src, size_t src_len, std::vector<u8>& dst, CompressionType type = CompressionType::LZSS, size_t expected_size = 0);
};

} // namespace nf::core
