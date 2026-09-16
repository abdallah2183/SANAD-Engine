// NF/Physics/JoltRagdoll.cpp — thin handle over JoltWorld ragdoll wiring.
//
// ALL Jolt objects live in JoltWorld.cpp (single TU owning the
// PhysicsSystem). Cross-TU Jolt allocation heap-corrupts (custom Jolt
// allocator + per-TU operator new/delete — proven by the vehicle crash),
// so this TU makes ZERO Jolt calls.
//
// LIFECYCLE: the world must outlive the ragdoll; destruction order is
// enforced by vehicle_destroy/ragdoll_destroy symmetry in JoltWorld.

#include <NF/Physics/JoltRagdoll.hpp>

namespace nf::physics {

struct JoltRagdoll::Impl {
    JoltWorld* world = nullptr;
    JoltWorld::RagdollHandle handle;
    bool ok = false;
};

JoltRagdoll::JoltRagdoll(JoltWorld& world, const std::vector<RagdollJointDesc>& joints, Vec3 spawn)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->world = &world;
    m_impl->handle = world.ragdoll_create(joints, spawn);
    m_impl->ok = m_impl->handle.valid();
}

JoltRagdoll::~JoltRagdoll() {
    if (m_impl && m_impl->world && m_impl->ok) m_impl->world->ragdoll_destroy(m_impl->handle);
}

bool JoltRagdoll::valid() const {
    return m_impl && m_impl->ok;
}

usize JoltRagdoll::joint_count() const {
    if (!m_impl || !m_impl->world || !m_impl->ok) return 0;
    return m_impl->world->ragdoll_joint_count(m_impl->handle);
}

std::vector<RagdollJointState> JoltRagdoll::joint_states() const {
    if (!m_impl || !m_impl->world || !m_impl->ok) return {};
    return m_impl->world->ragdoll_joint_states(m_impl->handle);
}

void JoltRagdoll::apply_impulse(usize joint_index, const Vec3& impulse) {
    if (!m_impl || !m_impl->world || !m_impl->ok) return;
    m_impl->world->ragdoll_apply_impulse(m_impl->handle, joint_index, impulse);
}

void JoltRagdoll::activate() {
    if (!m_impl || !m_impl->world || !m_impl->ok) return;
    m_impl->world->ragdoll_activate(m_impl->handle);
}

JoltBody JoltRagdoll::body_at(usize joint_index) const {
    if (!m_impl || !m_impl->world || !m_impl->ok) return JoltBody{};
    return m_impl->world->ragdoll_body_at(m_impl->handle, joint_index);
}

} // namespace nf::physics

