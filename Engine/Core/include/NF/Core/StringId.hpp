#pragma once

#include <NF/Core/Types.hpp>

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace nf {

/// 64-bit FNV-1a compile-time string hashing.
constexpr uint64_t fnv1a_64(const char* str, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(str[i]));
        hash *= 1099511628211ULL;
    }
    return hash;
}

constexpr uint64_t fnv1a_64(std::string_view str) {
    return fnv1a_64(str.data(), str.size());
}

/// A lightweight, 64-bit hashed string identifier with O(1) comparisons
/// and thread-safe string interning for debug retrieval.
class StringId {
public:
    constexpr StringId() : m_hash(0) {}
    constexpr explicit StringId(uint64_t hash) : m_hash(hash) {}
    StringId(const char* str);
    StringId(std::string_view str);
    StringId(const std::string& str);

    constexpr uint64_t hash() const { return m_hash; }
    constexpr bool valid() const { return m_hash != 0; }

    constexpr bool operator==(const StringId& other) const { return m_hash == other.m_hash; }
    constexpr bool operator!=(const StringId& other) const { return m_hash != other.m_hash; }
    constexpr bool operator<(const StringId& other) const { return m_hash < other.m_hash; }

    /// Retrieves original debug string from interner (returns empty if not found).
    std::string_view str() const;
    const char* c_str() const;

private:
    uint64_t m_hash = 0;
};

/// User defined literal for compile-time StringId: "Player.Health"_sid
constexpr StringId operator""_sid(const char* str, size_t len) {
    return StringId(fnv1a_64(str, len));
}

/// Global thread-safe string interner
class StringInterner {
public:
    static StringInterner& instance();

    StringId intern(std::string_view str);
    std::string_view lookup(uint64_t hash) const;
    void clear();

private:
    StringInterner() = default;
    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, std::string> m_strings;
};

namespace strings {

std::string trim(std::string_view str);
std::vector<std::string> split(std::string_view str, char delimiter);
std::string join(const std::vector<std::string>& parts, std::string_view delimiter);
std::string to_lower(std::string_view str);
std::string to_upper(std::string_view str);
bool starts_with(std::string_view str, std::string_view prefix);
bool ends_with(std::string_view str, std::string_view suffix);
std::string replace_all(std::string_view str, std::string_view from, std::string_view to);

} // namespace strings

} // namespace nf

namespace std {
template<> struct hash<nf::StringId> {
    size_t operator()(const nf::StringId& sid) const noexcept {
        return static_cast<size_t>(sid.hash());
    }
};
} // namespace std
