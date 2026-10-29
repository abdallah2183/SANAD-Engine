#pragma once

#include <NF/Core/Types.hpp>
#include <array>

namespace nf::physics {

/// Collision Filter with Layer ID (0..31), Layer Bitmask, and Group ID.
struct CollisionFilter {
    u32 layer = 0;              // Layer index (0..31)
    u32 mask = 0xFFFFFFFF;      // Bitmask of layers this body collides with
    i32 group_id = 0;           // Optional signed group: >0 always collide, <0 never collide with same group, 0=use layer matrix

    [[nodiscard]] static CollisionFilter all(u32 layer_index = 0) noexcept {
        CollisionFilter f;
        f.layer = layer_index;
        f.mask = 0xFFFFFFFF;
        f.group_id = 0;
        return f;
    }
};

/// 32x32 Collision Layer Matrix for filtering collisions between discrete layers.
class CollisionLayerMatrix {
public:
    CollisionLayerMatrix();

    /// Enable or disable collision between layer_a and layer_b (symmetric)
    void set_layer_collision(u32 layer_a, u32 layer_b, bool can_collide);

    /// Check if two layers can collide according to the matrix
    [[nodiscard]] bool can_layers_collide(u32 layer_a, u32 layer_b) const noexcept;

    /// Full collision check between two filters (combining group_id, bitmask, and layer matrix)
    [[nodiscard]] bool should_collide(const CollisionFilter& a, const CollisionFilter& b) const noexcept;

    /// Reset matrix so all layers collide with all layers (or none)
    void reset_all(bool default_collide = true);

private:
    std::array<u32, 32> m_layer_masks{};
};

} // namespace nf::physics
