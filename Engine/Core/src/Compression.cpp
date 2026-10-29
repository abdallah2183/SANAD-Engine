#include <NF/Core/Compression.hpp>
#include <algorithm>
#include <cstring>

namespace nf::core {

// ----------------------------------------------------------------------------
// RLE (PackBits Style with 4-byte header length)
// ----------------------------------------------------------------------------

std::vector<u8> Compression::compress_rle(const void* src, size_t src_len) {
    std::vector<u8> out;
    if (!src || src_len == 0) {
        return out;
    }
    const auto* in = static_cast<const u8*>(src);

    // Write 4-byte uncompressed length header
    const u32 len32 = static_cast<u32>(src_len);
    out.push_back(static_cast<u8>(len32 & 0xFF));
    out.push_back(static_cast<u8>((len32 >> 8) & 0xFF));
    out.push_back(static_cast<u8>((len32 >> 16) & 0xFF));
    out.push_back(static_cast<u8>((len32 >> 24) & 0xFF));

    size_t i = 0;
    while (i < src_len) {
        // Look for a run of identical bytes
        size_t run_len = 1;
        while (i + run_len < src_len && in[i + run_len] == in[i] && run_len < 128) {
            run_len++;
        }

        if (run_len >= 3) {
            // Write run: high bit 1, (run_len - 1), followed by byte
            const u8 ctrl = static_cast<u8>(0x80u | (run_len - 1));
            out.push_back(ctrl);
            out.push_back(in[i]);
            i += run_len;
        } else {
            // Look for literals
            size_t lit_len = 0;
            while (i + lit_len < src_len && lit_len < 128) {
                if (i + lit_len + 2 < src_len &&
                    in[i + lit_len] == in[i + lit_len + 1] &&
                    in[i + lit_len] == in[i + lit_len + 2]) {
                    break;
                }
                lit_len++;
            }

            if (lit_len == 0) {
                lit_len = 1;
            }

            const u8 ctrl = static_cast<u8>(lit_len - 1);
            out.push_back(ctrl);
            for (size_t k = 0; k < lit_len; ++k) {
                out.push_back(in[i + k]);
            }
            i += lit_len;
        }
    }

    return out;
}

bool Compression::decompress_rle(const void* src, size_t src_len, std::vector<u8>& dst) {
    dst.clear();
    if (!src || src_len < 4) {
        return (src_len == 0);
    }
    const auto* in = static_cast<const u8*>(src);

    const u32 target_size = static_cast<u32>(in[0]) |
                           (static_cast<u32>(in[1]) << 8) |
                           (static_cast<u32>(in[2]) << 16) |
                           (static_cast<u32>(in[3]) << 24);

    dst.reserve(target_size);

    size_t i = 4;
    while (i < src_len && dst.size() < target_size) {
        const u8 ctrl = in[i++];
        if (ctrl & 0x80u) {
            // Run
            const size_t count = (ctrl & 0x7Fu) + 1;
            if (i >= src_len) {
                return false;
            }
            const u8 val = in[i++];
            dst.insert(dst.end(), count, val);
        } else {
            // Literal
            const size_t count = ctrl + 1;
            if (i + count > src_len) {
                return false;
            }
            for (size_t k = 0; k < count; ++k) {
                dst.push_back(in[i + k]);
            }
            i += count;
        }
    }
    return dst.size() == target_size;
}

// ----------------------------------------------------------------------------
// LZSS (Sliding Window Dictionary with 4-byte header length)
// ----------------------------------------------------------------------------

constexpr size_t LZSS_WINDOW_SIZE = 4096;
constexpr size_t LZSS_MAX_MATCH = 18;
constexpr size_t LZSS_MIN_MATCH = 3;

std::vector<u8> Compression::compress_lzss(const void* src, size_t src_len) {
    std::vector<u8> out;
    if (!src || src_len == 0) {
        return out;
    }
    const auto* in = static_cast<const u8*>(src);

    // 4-byte header length
    const u32 len32 = static_cast<u32>(src_len);
    out.push_back(static_cast<u8>(len32 & 0xFF));
    out.push_back(static_cast<u8>((len32 >> 8) & 0xFF));
    out.push_back(static_cast<u8>((len32 >> 16) & 0xFF));
    out.push_back(static_cast<u8>((len32 >> 24) & 0xFF));

    size_t in_pos = 0;

    while (in_pos < src_len) {
        const size_t flag_byte_pos = out.size();
        out.push_back(0); // placeholder for 8 item flags
        u8 flag_byte = 0;

        for (u32 bit = 0; bit < 8 && in_pos < src_len; ++bit) {
            const size_t window_start = (in_pos > LZSS_WINDOW_SIZE) ? (in_pos - LZSS_WINDOW_SIZE) : 0;
            const size_t max_len = std::min(LZSS_MAX_MATCH, src_len - in_pos);

            size_t best_len = 0;
            size_t best_offset = 0;

            if (max_len >= LZSS_MIN_MATCH) {
                for (size_t w = window_start; w < in_pos; ++w) {
                    size_t match_len = 0;
                    while (match_len < max_len && in[w + match_len] == in[in_pos + match_len]) {
                        match_len++;
                    }
                    if (match_len > best_len) {
                        best_len = match_len;
                        best_offset = in_pos - w;
                        if (best_len == max_len) {
                            break;
                        }
                    }
                }
            }

            if (best_len >= LZSS_MIN_MATCH) {
                // Match: flag bit = 0
                const u16 offset_val = static_cast<u16>((best_offset - 1) & 0x0FFF);
                const u8 len_val = static_cast<u8>((best_len - LZSS_MIN_MATCH) & 0x0F);
                const u8 b1 = static_cast<u8>(offset_val & 0xFF);
                const u8 b2 = static_cast<u8>(((offset_val >> 8) & 0x0F) | (len_val << 4));

                out.push_back(b1);
                out.push_back(b2);
                in_pos += best_len;
            } else {
                // Literal: flag bit = 1
                flag_byte |= static_cast<u8>(1u << bit);
                out.push_back(in[in_pos++]);
            }
        }

        out[flag_byte_pos] = flag_byte;
    }

    return out;
}

bool Compression::decompress_lzss(const void* src, size_t src_len, std::vector<u8>& dst, size_t /*expected_size*/) {
    dst.clear();
    if (!src || src_len < 4) {
        return (src_len == 0);
    }
    const auto* in = static_cast<const u8*>(src);

    const u32 target_size = static_cast<u32>(in[0]) |
                           (static_cast<u32>(in[1]) << 8) |
                           (static_cast<u32>(in[2]) << 16) |
                           (static_cast<u32>(in[3]) << 24);

    dst.reserve(target_size);

    size_t in_pos = 4;
    while (in_pos < src_len && dst.size() < target_size) {
        const u8 flag_byte = in[in_pos++];
        for (u32 bit = 0; bit < 8 && in_pos < src_len && dst.size() < target_size; ++bit) {
            if (flag_byte & (1u << bit)) {
                // Literal
                dst.push_back(in[in_pos++]);
            } else {
                // Reference (2 bytes)
                if (in_pos + 1 >= src_len) {
                    return false;
                }
                const u8 b1 = in[in_pos++];
                const u8 b2 = in[in_pos++];
                const size_t offset = static_cast<size_t>(b1 | ((b2 & 0x0F) << 8)) + 1;
                const size_t length = static_cast<size_t>((b2 >> 4) + LZSS_MIN_MATCH);

                if (offset > dst.size()) {
                    return false;
                }

                const size_t ref_start = dst.size() - offset;
                for (size_t k = 0; k < length && dst.size() < target_size; ++k) {
                    dst.push_back(dst[ref_start + k]);
                }
            }
        }
    }

    return dst.size() == target_size;
}

std::vector<u8> Compression::compress(const void* src, size_t src_len, CompressionType type) {
    return (type == CompressionType::RLE) ? compress_rle(src, src_len) : compress_lzss(src, src_len);
}

bool Compression::decompress(const void* src, size_t src_len, std::vector<u8>& dst, CompressionType type, size_t expected_size) {
    return (type == CompressionType::RLE) ? decompress_rle(src, src_len, dst) : decompress_lzss(src, src_len, dst, expected_size);
}

} // namespace nf::core
