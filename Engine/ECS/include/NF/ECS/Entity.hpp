#pragma once

#include <NF/Core/Types.hpp>

#include <functional>

namespace nf::ecs {

struct Entity {
    u32 id = u32_max;
    u32 generation = 0;

    bool valid() const { return id != u32_max; }
    bool operator==(const Entity& other) const { return id == other.id && generation == other.generation; }
    bool operator!=(const Entity& other) const { return !(*this == other); }
    // Total order over (id, generation) so an Entity can key a std::set /
    // std::map. Keying a set on the id alone is a trap: two entities can
    // share an id with different generations, and the stale one must not be
    // mistaken for the live one.
    bool operator<(const Entity& other) const {
        return id != other.id ? id < other.id : generation < other.generation;
    }
};

static constexpr Entity kInvalidEntity{};

} // namespace nf::ecs

namespace std {
template<> struct hash<nf::ecs::Entity> {
    size_t operator()(const nf::ecs::Entity& e) const noexcept {
        return (static_cast<size_t>(e.id) << 32) ^ e.generation;
    }
};
} // namespace std
