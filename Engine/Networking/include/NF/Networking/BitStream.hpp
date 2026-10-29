#pragma once

#include <NF/Core/Types.hpp>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace nf::net {

/// Bit-level stream writer with arbitrary bit-width packing
class BitWriter {
public:
    explicit BitWriter(size_t initial_bytes = 64);

    /// Write between 1 and 64 bits
    void write_bits(u64 value, u32 bit_count);

    /// Write exactly 1 bit
    void write_bool(bool val) { write_bits(val ? 1u : 0u, 1); }

    void write_u8(u8 val)   { write_bits(val, 8); }
    void write_u16(u16 val) { write_bits(val, 16); }
    void write_u32(u32 val) { write_bits(val, 32); }
    void write_u64(u64 val) { write_bits(val, 64); }

    void write_i32(i32 val);
    void write_float(f32 val);
    void write_string(const std::string& str);
    void write_bytes(const void* data, size_t byte_count);

    /// Align to the next whole byte boundary
    void flush_byte();

    [[nodiscard]] size_t bit_count() const noexcept { return m_bit_count; }
    [[nodiscard]] size_t byte_count() const noexcept { return (m_bit_count + 7) / 8; }
    [[nodiscard]] const u8* data() const noexcept { return m_buffer.data(); }
    [[nodiscard]] const std::vector<u8>& buffer() const noexcept { return m_buffer; }

    void clear();

private:
    std::vector<u8> m_buffer;
    size_t m_bit_count = 0;
};

/// Bit-level stream reader
class BitReader {
public:
    BitReader(const u8* data, size_t byte_count);
    explicit BitReader(const std::vector<u8>& buffer);

    /// Read between 1 and 64 bits
    [[nodiscard]] u64 read_bits(u32 bit_count);

    [[nodiscard]] bool read_bool() { return read_bits(1) != 0; }

    [[nodiscard]] u8  read_u8()  { return static_cast<u8>(read_bits(8)); }
    [[nodiscard]] u16 read_u16() { return static_cast<u16>(read_bits(16)); }
    [[nodiscard]] u32 read_u32() { return static_cast<u32>(read_bits(32)); }
    [[nodiscard]] u64 read_u64() { return read_bits(64); }

    [[nodiscard]] i32 read_i32();
    [[nodiscard]] f32 read_float();
    [[nodiscard]] std::string read_string();
    void read_bytes(void* dest, size_t byte_count);

    /// Align to next byte boundary
    void flush_byte();

    [[nodiscard]] size_t bit_position() const noexcept { return m_bit_pos; }
    [[nodiscard]] size_t total_bits() const noexcept { return m_total_bits; }
    [[nodiscard]] size_t bits_remaining() const noexcept {
        return (m_bit_pos < m_total_bits) ? (m_total_bits - m_bit_pos) : 0;
    }
    [[nodiscard]] bool is_overflow() const noexcept { return m_overflow; }

private:
    const u8* m_data = nullptr;
    size_t m_total_bits = 0;
    size_t m_bit_pos = 0;
    bool m_overflow = false;
};

} // namespace nf::net
