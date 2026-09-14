#pragma once

#include <NF/Core/UUID.hpp>

#include <string>
#include <string_view>

namespace nf::assets {

struct AssetId {
    UUID uuid;

    AssetId() = default;
    explicit AssetId(const UUID& u) : uuid(u) {}

    static AssetId generate() { return AssetId(UUID::generate()); }
    static AssetId from_string(std::string_view s) { return AssetId(UUID::from_string(s)); }
    std::string to_string() const { return uuid.to_string(); }

    bool valid() const { return uuid.is_valid(); }
    bool operator==(const AssetId& other) const = default;
    bool operator!=(const AssetId& other) const = default;
    bool operator<(const AssetId& other) const { return uuid.to_string() < other.uuid.to_string(); }
};

} // namespace nf::assets

namespace std {
template<> struct hash<nf::assets::AssetId> {
    size_t operator()(const nf::assets::AssetId& id) const noexcept {
        return std::hash<nf::UUID>{}(id.uuid);
    }
};
} // namespace std
