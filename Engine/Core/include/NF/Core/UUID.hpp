#pragma once

// NF/Core/UUID.hpp — UUID generation for asset IDs and entity references

#include <NF/Core/Types.hpp>

#include <array>
#include <string>
#include <string_view>

namespace nf {

struct UUID {
    std::array<u8, 16> bytes{};

    static UUID generate();
    static UUID from_string(std::string_view str);
    std::string to_string() const;

    constexpr bool operator==(const UUID& other) const = default;
    constexpr bool operator!=(const UUID& other) const = default;

    bool is_valid() const {
        for (u8 b : bytes) {
            if (b != 0) return true;
        }
        return false;
    }
};

} // namespace nf

// std::hash specialization for UUID
namespace std {
template<>
struct hash<nf::UUID> {
    size_t operator()(const nf::UUID& uuid) const noexcept {
        const nf::u64* hi = reinterpret_cast<const nf::u64*>(uuid.bytes.data());
        size_t h = 14695981039346656037ULL;
        for (int i = 0; i < 2; ++i) {
            h ^= hi[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
};
} // namespace std
