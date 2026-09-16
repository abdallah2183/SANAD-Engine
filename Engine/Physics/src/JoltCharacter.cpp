// NF/Physics/JoltCharacter.cpp — thin handle over JoltWorld character wiring.
//
// ALL Jolt objects live in JoltWorld.cpp (single TU owning the PhysicsSystem).
// Cross-TU Jolt allocation heap-corrupts (custom Jolt allocator + per-TU
// operator new/delete), so this TU makes ZERO Jolt calls.
//
// LIFECYCLE: the world must outlive the character; destruction order is
// enforced by character_destroy symmetry in JoltWorld.

#include <NF/Physics/JoltCharacter.hpp>

namespace nf::physics {

JoltCharacter::JoltCharacter(JoltWorld& world, const JoltCharacterConfig& config, Vec3 spawn)
    : m_world(&world), m_handle(world.character_create(config, spawn)),
      m_ok(m_handle.valid()) {
}

JoltCharacter::~JoltCharacter() {
    if (m_world && m_ok) m_world->character_destroy(m_handle);
}

bool JoltCharacter::valid() const {
    return m_world && m_ok;
}

void JoltCharacter::move(Vec3 wish_dir, bool jump, float dt) {
    if (!m_world || !m_ok) return;
    m_world->character_move(m_handle, wish_dir, jump, dt);
}

void JoltCharacter::set_climbing(bool enabled) {
    if (!m_world || !m_ok) return;
    m_world->character_set_climbing(m_handle, enabled);
}

void JoltCharacter::set_crouch(bool crouched) {
    if (!m_world || !m_ok) return;
    m_world->character_set_crouch(m_handle, crouched);
}

bool JoltCharacter::is_crouched() const {
    if (!m_world || !m_ok) return false;
    return m_world->character_is_crouched(m_handle);
}

bool JoltCharacter::is_grounded() const {
    if (!m_world || !m_ok) return false;
    return m_world->character_is_grounded(m_handle);
}

Vec3 JoltCharacter::position() const {
    if (!m_world || !m_ok) return Vec3{0, 0, 0};
    return m_world->character_position(m_handle);
}

Vec3 JoltCharacter::velocity() const {
    if (!m_world || !m_ok) return Vec3{0, 0, 0};
    return m_world->character_velocity(m_handle);
}

} // namespace nf::physics
