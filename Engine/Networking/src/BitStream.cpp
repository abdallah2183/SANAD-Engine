#include <NF/Networking/BitStream.hpp>

namespace nf::net {

BitWriter::BitWriter(size_t initial_bytes) {
    m_buffer.reserve(initial_bytes);
}

void BitWriter::write_bits(u64 value, u32 bit_count) {
    if (bit_count == 0) {
        return;
    }
    bit_count = std::min(bit_count, 64u);

    // Mask value to bit_count
    if (bit_count < 64) {
        value &= ((1ULL << bit_count) - 1ULL);
    }

    for (u32 i = 0; i < bit_count; ++i) {
        const u8 bit = static_cast<u8>((value >> i) & 1u);
        const size_t byte_idx = m_bit_count / 8;
        const u32 bit_idx = static_cast<u32>(m_bit_count % 8);

        if (byte_idx >= m_buffer.size()) {
            m_buffer.push_back(0);
        }

        if (bit) {
            m_buffer[byte_idx] |= static_cast<u8>(1u << bit_idx);
        } else {
            m_buffer[byte_idx] &= static_cast<u8>(~(1u << bit_idx));
        }
        m_bit_count++;
    }
}

void BitWriter::write_i32(i32 val) {
    u32 uval = 0;
    std::memcpy(&uval, &val, sizeof(i32));
    write_u32(uval);
}

void BitWriter::write_float(f32 val) {
    u32 uval = 0;
    std::memcpy(&uval, &val, sizeof(f32));
    write_u32(uval);
}

void BitWriter::write_string(const std::string& str) {
    const u16 len = static_cast<u16>(std::min(str.size(), static_cast<size_t>(65535)));
    write_u16(len);
    for (u16 i = 0; i < len; ++i) {
        write_u8(static_cast<u8>(str[i]));
    }
}

void BitWriter::write_bytes(const void* data, size_t byte_count) {
    if (!data || byte_count == 0) {
        return;
    }
    const auto* src = static_cast<const u8*>(data);
    for (size_t i = 0; i < byte_count; ++i) {
        write_u8(src[i]);
    }
}

void BitWriter::flush_byte() {
    const size_t rem = m_bit_count % 8;
    if (rem != 0) {
        write_bits(0, static_cast<u32>(8 - rem));
    }
}

void BitWriter::clear() {
    m_buffer.clear();
    m_bit_count = 0;
}

// ----------------------------------------------------------------------------
// BitReader
// ----------------------------------------------------------------------------

BitReader::BitReader(const u8* data, size_t byte_count)
    : m_data(data)
    , m_total_bits(byte_count * 8)
    , m_bit_pos(0)
    , m_overflow(false) {}

BitReader::BitReader(const std::vector<u8>& buffer)
    : m_data(buffer.data())
    , m_total_bits(buffer.size() * 8)
    , m_bit_pos(0)
    , m_overflow(false) {}

u64 BitReader::read_bits(u32 bit_count) {
    if (bit_count == 0) {
        return 0;
    }
    bit_count = std::min(bit_count, 64u);

    if (m_bit_pos + bit_count > m_total_bits) {
        m_overflow = true;
        return 0;
    }

    u64 value = 0;
    for (u32 i = 0; i < bit_count; ++i) {
        const size_t byte_idx = m_bit_pos / 8;
        const u32 bit_idx = static_cast<u32>(m_bit_pos % 8);

        const u8 byte_val = m_data[byte_idx];
        const u64 bit = (byte_val >> bit_idx) & 1u;
        value |= (bit << i);

        m_bit_pos++;
    }

    return value;
}

i32 BitReader::read_i32() {
    const u32 uval = read_u32();
    i32 val = 0;
    std::memcpy(&val, &uval, sizeof(i32));
    return val;
}

f32 BitReader::read_float() {
    const u32 uval = read_u32();
    f32 val = 0.0f;
    std::memcpy(&val, &uval, sizeof(f32));
    return val;
}

std::string BitReader::read_string() {
    const u16 len = read_u16();
    if (m_overflow) {
        return "";
    }
    std::string str;
    str.resize(len);
    for (u16 i = 0; i < len; ++i) {
        str[i] = static_cast<char>(read_u8());
    }
    return str;
}

void BitReader::read_bytes(void* dest, size_t byte_count) {
    if (!dest || byte_count == 0) {
        return;
    }
    auto* dst = static_cast<u8*>(dest);
    for (size_t i = 0; i < byte_count; ++i) {
        dst[i] = read_u8();
    }
}

void BitReader::flush_byte() {
    const size_t rem = m_bit_pos % 8;
    if (rem != 0) {
        m_bit_pos += (8 - rem);
    }
}

} // namespace nf::net
