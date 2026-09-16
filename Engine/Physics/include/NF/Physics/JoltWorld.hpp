#pragma once

// NF/Physics/JoltWorld.hpp — Jolt Physics backend (Phase 16).
//
// The engine's default solver stays first-party (PhysicsWorld); this class
// offers the same shape vocabulary (sphere/box/plane, static/dynamic,
// gravity, fixed steps) on top of the vendored Jolt SDK for scenes that need
// industrial-strength stacking, complex meshes and vehicles.
//
// The header is Jolt-free (pimpl): including it never drags the SDK into a
// translation unit, and NFPhysics links `jolt` privately. Deterministic when
// stepped with fixed dt (Jolt is built JPH_CROSS_PLATFORM_DETERMINISTIC).

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Physics/PhysicsWorld.hpp> // PhysicsSettings (gravity) + BodyType

#include <memory>
#include <string>
#include <vector>

namespace nf::physics {

/// Opaque Jolt body id. Invalid by default; comparable.
struct JoltBody {
    u32 id = 0xFFFFFFFFu;
    bool valid() const { return id != 0xFFFFFFFFu; }
    bool operator==(const JoltBody& o) const { return id == o.id; }
    bool operator!=(const JoltBody& o) const { return !(*this == o); }
};

struct JoltBodyState {
    Vec3 position{0, 0, 0};
    Vec3 linear_velocity{0, 0, 0};
    bool active = false;
};

struct RagdollJointState {
    Vec3 position{0, 0, 0};
    Vec3 linear_velocity{0, 0, 0};
    JoltBody body;
};

struct RagdollJointDesc {
    std::string name;
    std::string parent; // empty = root
    Vec3 local_offset{0, 0, 0}; // offset from parent joint in rest pose
    float radius = 0.15f;
    float mass = 1.0f;
    float swing_limit_deg = 30.0f; // max swing angle from parent axis
    float twist_limit_deg = 15.0f; // max twist angle around parent axis
};

/// Arcade vehicle setup (wheels, suspension attach, engine). Lives here (not
/// in JoltVehicle.hpp) so JoltWorld::vehicle_create can take it without a
/// circular include.
struct JoltVehicleConfig {
    Vec3 chassis_half_extents{0.9f, 0.5f, 2.0f};
    float chassis_mass = 1500.0f;
    float wheel_radius = 0.35f;
    float wheel_width = 0.3f;
    float track_half_width = 0.9f; // |x| of wheels
    float wheelbase_half_length = 1.3f; // |z| of axles (front = +z)
    float wheel_y = -0.35f; // attachment height, chassis-local
    float max_steer_deg = 30.0f;
    float engine_max_torque = 500.0f;
};

/// Opaque two-body constraint handle. 0 = invalid.
struct JoltConstraint {
    u32 id = 0;
    bool valid() const { return id != 0; }
    bool operator==(const JoltConstraint& o) const { return id == o.id; }
    bool operator!=(const JoltConstraint& o) const { return !(*this == o); }
};

class JoltWorld {
public:
    explicit JoltWorld(const PhysicsSettings& settings = PhysicsSettings{});
    ~JoltWorld();

    JoltWorld(const JoltWorld&) = delete;
    JoltWorld& operator=(const JoltWorld&) = delete;

    bool valid() const;

    /// Adds a sphere/box/plane body (other shapes map to a sphere of equal
    /// bounding radius — documented, never silent: unsupported shapes keep
    /// simulating instead of vanishing).
    JoltBody add_body(const BodyDesc& desc);
    void remove_body(JoltBody handle);
    bool is_alive(JoltBody handle) const;

    JoltBodyState state(JoltBody handle) const;
    void set_linear_velocity(JoltBody handle, const Vec3& velocity);

    /// One fixed step (dt <= 0 is a no-op).
    void step(float dt);

    usize body_count() const;
    const PhysicsSettings& settings() const { return m_settings; }

    /// Escape hatch for advanced integrations (vehicles, custom constraints):
    /// the JPH::PhysicsSystem*. Same-module code only; never store it past
    /// the world's lifetime.
    void* system_handle();

    // --- Vehicle wiring (implemented in JoltWorld.cpp so ALL Jolt object
    // --- creation — chassis Body*, VehicleConstraint, tester — happens in the
    // --- single TU that owns the PhysicsSystem. Cross-TU Jolt allocation
    // --- (custom Jolt allocator + per-TU operator new/delete) heap-corrupts.
    struct VehicleHandle {
        u32 constraint_id = 0; // index into m_impl->constraints (0 = none)
        JoltBody chassis;
    };
    VehicleHandle vehicle_create(const JoltVehicleConfig& config, Vec3 spawn);
    void vehicle_destroy(VehicleHandle handle);
    JoltBodyState vehicle_chassis_state(VehicleHandle handle) const;
    void vehicle_drive(VehicleHandle handle, float forward, float steer, float brake);

    // --- Ragdoll wiring (same single-TU rule as vehicles: skeleton,
    // --- RagdollSettings, JPH::Ragdoll all born in JoltWorld.cpp).
    struct RagdollHandle {
        u32 id = 0; // index into m_impl->ragdolls (0 = none)
        bool valid() const { return id != 0; }
    };
    RagdollHandle ragdoll_create(const std::vector<RagdollJointDesc>& joints, Vec3 spawn);
    void ragdoll_destroy(RagdollHandle handle);
    usize ragdoll_joint_count(RagdollHandle handle) const;
    std::vector<RagdollJointState> ragdoll_joint_states(RagdollHandle handle) const;
    void ragdoll_apply_impulse(RagdollHandle handle, usize joint_index, const Vec3& impulse);
    void ragdoll_activate(RagdollHandle handle);
    JoltBody ragdoll_body_at(RagdollHandle handle, usize joint_index) const;

    // --- Two-body constraints (world-space setup) ---
    /// Welds two bodies rigidly in their current relative transform.
    JoltConstraint add_fixed(JoltBody a, JoltBody b);
    /// Hinge around world_axis through world_point (both bodies).
    JoltConstraint add_hinge(JoltBody a, JoltBody b, Vec3 world_point, Vec3 world_axis);
    /// Ball-and-socket at world_point.
    JoltConstraint add_point(JoltBody a, JoltBody b, Vec3 world_point);
    /// Slider along world_axis through world_point (linear sliding, no rotation).
    JoltConstraint add_slider(JoltBody a, JoltBody b, Vec3 world_point, Vec3 world_axis);
    /// Distance constraint: keeps bodies between min_dist and max_dist apart.
    JoltConstraint add_distance(JoltBody a, JoltBody b, Vec3 anchor_a, Vec3 anchor_b,
                                float min_dist, float max_dist);
    /// Cone twist constraint at world_point with max cone angle (radians).
    JoltConstraint add_cone(JoltBody a, JoltBody b, Vec3 world_point, Vec3 world_axis,
                            float max_angle_rad);
    /// SixDOF constraint: per-axis limits on all 6 degrees of freedom.
    /// limit_min/limit_max per axis; 0 = locked, negative = free.
    JoltConstraint add_sixdof(JoltBody a, JoltBody b, Vec3 world_point,
                              const Vec3 limit_min[6], const Vec3 limit_max[6]);
    void remove_constraint(JoltConstraint handle);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    PhysicsSettings m_settings;
};

} // namespace nf::physics
