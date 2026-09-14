// NF/Core/UUID.cpp

#include <NF/Core/UUID.hpp>
#include <NF/Core/Assert.hpp>

#include <cstdio>
#include <random>
#include <chrono>

namespace nf {

namespace {
std::mt19937_64& rng() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    return gen;
}
} // namespace

UUID UUID::generate() {
    // v4 UUID (random), with version and variant bits set
    auto& g = rng();
    u64 hi = g();
    u64 lo = g();

    UUID uuid;
    std::memcpy(uuid.bytes.data(), &hi, 8);
    std::memcpy(uuid.bytes.data() + 8, &lo, 8);

    // Set version to 4 (random)
    uuid.bytes[6] = (uuid.bytes[6] & 0x0F) | 0x40;
    // Set variant to RFC 4122
    uuid.bytes[8] = (uuid.bytes[8] & 0x3F) | 0x80;

    return uuid;
}

UUID UUID::from_string(std::string_view str) {
    UUID uuid{};
    // Parse xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    int byte_idx = 0;
    int nibble = 0;
    u8 current = 0;

    for (char c : str) {
        if (c == '-') continue;

        u8 val = 0;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else continue;

        if (nibble == 0) {
            current = val << 4;
            nibble = 1;
        } else {
            current |= val;
            if (byte_idx < 16) {
                uuid.bytes[byte_idx++] = current;
            }
            nibble = 0;
        }
    }
    return uuid;
}

std::string UUID::to_string() const {
    char buf[37];
    const u8* b = bytes.data();
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(buf);
}

} // namespace nf
