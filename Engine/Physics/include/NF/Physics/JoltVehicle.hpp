#pragma once

// NF/Physics/JoltVehicle.hpp — drivable arcade vehicle on Jolt (Phase 16).
//
// A box chassis on four raycast-style wheels (Jolt VehicleConstraint):
// front axle steers, rear axle drives, suspension keeps it planted. Inputs
// are normalized arcade controls (forward/steer/brake), not torques, so a
// gamepad, a keyboard, or a network packet drives the same API.
//
// The vehicle owns its chassis body and constraint inside a JoltWorld; the
// world must outlive it. No rendering coupling: read chassis_state() and
// pose your mesh.
//
// IMPLEMENTATION NOTE: every Jolt object lives in JoltWorld.cpp (single TU
// owning the PhysicsSystem). This header stays Jolt-free; the class is a
// thin handle over JoltWorld::VehicleHandle. Do NOT move Jolt calls back
// into JoltVehicle.cpp — cross-TU Jolt allocation heap-corrupts (custom
// Jolt allocator + per-TU operator new/delete).

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Physics/JoltWorld.hpp>

namespace nf::physics {

class JoltVehicle {
public:
    JoltVehicle(JoltWorld& world, const JoltVehicleConfig& config, Vec3 spawn);
    ~JoltVehicle();

    JoltVehicle(const JoltVehicle&) = delete;
    JoltVehicle& operator=(const JoltVehicle&) = delete;

    bool valid() const;

    /// Arcade inputs: forward/steer in [-1, 1], brake in [0, 1].
    /// Applied on the next world.step().
    void drive(float forward, float steer, float brake = 0.0f);

    JoltBodyState chassis_state() const;
    /// Horizontal speed in m/s.
    float speed_ms() const;

private:
    JoltWorld* m_world = nullptr;
    JoltWorld::VehicleHandle m_handle;
    bool m_ok = false;
};

} // namespace nf::physics
