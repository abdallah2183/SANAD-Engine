#pragma once

// NF/Physics/JoltRagdoll.hpp — multi-body ragdoll on Jolt (Phase 16).
//
// A ragdoll is a chain of dynamic bodies (one per joint) connected by
// swing-twist constraints. Each body is a sphere; the constraint limits
// how far each child can swing from its parent's axis. The root body has
// no constraint.
//
// Example skeleton: pelvis → spine → head, pelvis → L thigh → L shin, ...
// Each joint entry gives a name, parent name, local offset, radius and mass.
//
// Like JoltVehicle, this owns its bodies and constraints inside a JoltWorld
// that must outlive it. No rendering coupling: read joint_states() and pose
// your skeleton.
//
// IMPLEMENTATION NOTE: joint descs/state structs live in JoltWorld.hpp so
// JoltWorld::ragdoll_create can take them without a circular include; every
// Jolt object lives in JoltWorld.cpp (single TU owning the PhysicsSystem).

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Physics/JoltWorld.hpp>

namespace nf::physics {


class JoltRagdoll {
public:
    JoltRagdoll(JoltWorld& world, const std::vector<RagdollJointDesc>& joints, Vec3 spawn);
    ~JoltRagdoll();

    JoltRagdoll(const JoltRagdoll&) = delete;
    JoltRagdoll& operator=(const JoltRagdoll&) = delete;

    bool valid() const;
    usize joint_count() const;

    /// State for each joint (same order as input).
    std::vector<RagdollJointState> joint_states() const;

    /// Apply an impulse to a joint's body.
    void apply_impulse(usize joint_index, const Vec3& impulse);

    /// Activate (wake) all bodies.
    void activate();

    /// Access the JoltBody for a joint.
    JoltBody body_at(usize joint_index) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace nf::physics
