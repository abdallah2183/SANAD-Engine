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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    PhysicsSettings m_settings;
};

} // namespace nf::physics
