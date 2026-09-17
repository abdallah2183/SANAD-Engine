// NF/Physics/JoltVehicle.cpp — thin handle over JoltWorld vehicle wiring.
//
// ALL Jolt objects live in JoltWorld.cpp (single TU owning the
// PhysicsSystem). Cross-TU Jolt allocation heap-corrupts (custom Jolt
// allocator + per-TU operator new/delete), so this TU makes ZERO Jolt calls.
//
// LIFECYCLE: the world must outlive the vehicle; destruction order is
// enforced by vehicle_destroy/ragdoll_destroy symmetry in JoltWorld.

#include <NF/Physics/JoltVehicle.hpp>

#include <cmath>

namespace nf::physics {

JoltVehicle::JoltVehicle(JoltWorld& world, const JoltVehicleConfig& config, Vec3 spawn)
    : m_world(&world), m_handle(world.vehicle_create(config, spawn)),
      m_ok(m_handle.chassis.valid()) {
}

JoltVehicle::~JoltVehicle() {
    if (m_world && m_ok) m_world->vehicle_destroy(m_handle);
}

bool JoltVehicle::valid() const {
    return m_world && m_ok;
}

void JoltVehicle::drive(float forward, float steer, float brake) {
    if (!m_world || !m_ok) return;
    m_world->vehicle_drive(m_handle, forward, steer, brake);
}

JoltBodyState JoltVehicle::chassis_state() const {
    if (!m_world || !m_ok) return JoltBodyState{};
    return m_world->vehicle_chassis_state(m_handle);
}

float JoltVehicle::speed_ms() const {
    const JoltBodyState st = chassis_state();
    const float vx = st.linear_velocity.x;
    const float vz = st.linear_velocity.z;
    return std::sqrt(vx * vx + vz * vz);
}

std::vector<JoltWheelState> JoltVehicle::wheel_states() const {
    if (!m_world || !m_ok) return {};
    return m_world->vehicle_wheel_states(m_handle);
}

void JoltVehicle::reset(Vec3 position) {
    if (!m_world || !m_ok) return;
    m_world->vehicle_reset(m_handle, position);
}

} // namespace nf::physics

