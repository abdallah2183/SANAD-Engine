#include <NF/Physics/CollisionFilter.hpp>

namespace nf::physics {

CollisionLayerMatrix::CollisionLayerMatrix() {
    reset_all(true);
}

void CollisionLayerMatrix::reset_all(bool default_collide) {
    const u32 val = default_collide ? 0xFFFFFFFFu : 0u;
    m_layer_masks.fill(val);
}

void CollisionLayerMatrix::set_layer_collision(u32 layer_a, u32 layer_b, bool can_collide) {
    if (layer_a >= 32 || layer_b >= 32) {
        return;
    }
    if (can_collide) {
        m_layer_masks[layer_a] |= (1u << layer_b);
        m_layer_masks[layer_b] |= (1u << layer_a);
    } else {
        m_layer_masks[layer_a] &= ~(1u << layer_b);
        m_layer_masks[layer_b] &= ~(1u << layer_a);
    }
}

bool CollisionLayerMatrix::can_layers_collide(u32 layer_a, u32 layer_b) const noexcept {
    if (layer_a >= 32 || layer_b >= 32) {
        return false;
    }
    return (m_layer_masks[layer_a] & (1u << layer_b)) != 0;
}

bool CollisionLayerMatrix::should_collide(const CollisionFilter& a, const CollisionFilter& b) const noexcept {
    // 1. Group ID rules:
    // If both have identical non-zero group:
    // Positive group: always collide regardless of layers.
    // Negative group: never collide with each other (e.g., parts of the same ragdoll).
    if (a.group_id != 0 && a.group_id == b.group_id) {
        return a.group_id > 0;
    }

    // 2. Layer bounds check
    if (a.layer >= 32 || b.layer >= 32) {
        return false;
    }

    // 3. Matrix check
    if (!can_layers_collide(a.layer, b.layer)) {
        return false;
    }

    // 4. Individual filter bitmask check: a must mask b's layer, and b must mask a's layer
    const bool a_accepts_b = (a.mask & (1u << b.layer)) != 0;
    const bool b_accepts_a = (b.mask & (1u << a.layer)) != 0;

    return a_accepts_b && b_accepts_a;
}

} // namespace nf::physics
