#pragma once

#include <NF/ECS/ECS.hpp>
#include <NF/Physics/JoltVehicle.hpp>
#include <NF/Physics/PhysicsWorld.hpp>

namespace nf::physics {

/// Marks an entity as simulated. Pairs with a ColliderComponent (which shape to
/// use) and a scene::Transform (where it is).
///
/// The split between body and collider is deliberate: several entities can share
/// a collider description, and a body with no collider is a legitimate (if
/// useless) state the editor should be able to show rather than crash on.
struct RigidBodyComponent {
    BodyType type = BodyType::Dynamic;
    f32 mass = 1.0f;
    f32 friction = 0.5f;
    f32 restitution = 0.1f;
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.05f;
    bool allow_sleep = true;
    Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    Vec3 angular_velocity{0.0f, 0.0f, 0.0f};

    /// Filled in when the runtime creates the body. A runtime handle, not scene
    /// data: it is deliberately not serialized, because a handle from a previous
    /// session is meaningless (and, with generation checking, correctly rejected).
    BodyHandle body{};
};

/// The shape an entity collides with.
struct ColliderComponent {
    Shape shape = Shape::make_sphere(0.5f);
};

/// Marks an entity as a drivable Jolt vehicle. Pairs with a scene::Transform
/// (spawn pose) and is simulated only while the runtime owns a JoltWorld.
///
/// The component holds CONFIGURATION (what the vehicle is); the live JoltVehicle
/// object is owned by VehicleSystem (below), because it must outlive any single
/// entity view and its lifetime is tied to the physics world, not the scene.
struct VehicleComponent {
    JoltVehicleConfig config;
    /// Arcade drive inputs in [-1, 1]. Written by gameplay (input, AI, a network
    /// packet), read by VehicleSystem every fixed step. This is the ONLY way to
    /// drive a vehicle from game code — torque fields are deliberately not
    /// exposed, so a gamepad and a replay file drive the same API.
    f32 throttle = 0.0f;
    f32 steer = 0.0f;
    f32 brake = 0.0f;
    /// Rear-wheel-only clamp at the hand-brake torque: breaks traction into a
    /// slide rather than stopping the car, so it is a separate channel from
    /// brake even though both live in [0, 1].
    f32 handbrake = 0.0f;
    /// Runtime-only: the physics vehicle, created when the body spawns. Not
    /// serialized (a handle from a previous session is meaningless).
    JoltVehicle* vehicle = nullptr;
};

/// Marks an entity as a cloned constraint anchor: the joint between `other`
/// and this entity is a copy of `template_constraint` (built once on a template
/// pair). Lets a scene declare "these two doors hang the same way" without
/// re-specifying hinge geometry per instance.
struct ConstraintCloneComponent {
    /// Entity whose constraint is the template (must itself carry a
    /// ConstraintComponent or otherwise have a live JoltConstraint).
    ecs::Entity other;
    JoltConstraint template_constraint;
    /// Filled in at spawn. Invalid until the runtime clones the joint.
    JoltConstraint cloned;
};

} // namespace nf::physics

