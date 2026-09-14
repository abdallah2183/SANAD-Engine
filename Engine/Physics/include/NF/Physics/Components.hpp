#pragma once

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

} // namespace nf::physics
