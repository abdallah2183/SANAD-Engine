// NF/Physics/JoltWorld.cpp — Jolt backend wrapper (single TU touching Jolt).
//
// Jolt headers are warning-noisy under /W4 /WX, so they live behind a
// warning push/pop here; no other engine TU includes them (see JoltWorld.hpp).

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <NF/Physics/JoltWorld.hpp>

#include <atomic>
#include <mutex>

namespace nf::physics {

namespace {

// Process-wide Jolt one-time init (allocator + factory + types). Refcounted:
// every JoltWorld holds one reference, the last one unregisters.
std::mutex& jolt_mutex() {
    static std::mutex m;
    return m;
}
std::atomic<int>& jolt_refs() {
    static std::atomic<int> refs{0};
    return refs;
}

namespace Layers {
constexpr JPH::ObjectLayer kNonMoving = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::uint kNumObjectLayers = 2;
// Broad-phase layers are ours to define (the SDK ships no defaults):
// 0 = static scenery, 1 = everything that moves.
const JPH::BroadPhaseLayer kBpNonMoving(0);
const JPH::BroadPhaseLayer kBpMoving(1);
constexpr JPH::uint kNumBroadPhaseLayers = 2;
} // namespace Layers

} // namespace

struct JoltWorld::Impl {
    JPH::PhysicsSystem physics;
    JPH::TempAllocatorImpl* temp = nullptr;
    JPH::JobSystemSingleThreaded* jobs = nullptr;
    JPH::BroadPhaseLayerInterfaceTable* broadphase = nullptr;
    JPH::ObjectLayerPairFilterTable* layer_filter = nullptr;
    JPH::ObjectVsBroadPhaseLayerFilterTable* bp_filter = nullptr;
    bool ok = false;

    ~Impl() {
        delete bp_filter;
        delete layer_filter;
        delete broadphase;
        delete jobs;
        delete temp;
    }
};

JoltWorld::JoltWorld(const PhysicsSettings& settings) : m_settings(settings) {
    m_impl = std::make_unique<Impl>();
    Impl& impl = *m_impl;
    {
        std::lock_guard lock(jolt_mutex());
        if (jolt_refs().fetch_add(1) == 0) {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }
    impl.temp = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    impl.jobs = new JPH::JobSystemSingleThreaded(JPH::cMaxPhysicsJobs);
    impl.broadphase =
        new JPH::BroadPhaseLayerInterfaceTable(Layers::kNumObjectLayers, Layers::kNumBroadPhaseLayers);
    impl.broadphase->MapObjectToBroadPhaseLayer(Layers::kNonMoving, Layers::kBpNonMoving);
    impl.broadphase->MapObjectToBroadPhaseLayer(Layers::kMoving, Layers::kBpMoving);
    impl.layer_filter = new JPH::ObjectLayerPairFilterTable(Layers::kNumObjectLayers);
    impl.layer_filter->EnableCollision(Layers::kMoving, Layers::kMoving);
    impl.layer_filter->EnableCollision(Layers::kMoving, Layers::kNonMoving);
    impl.bp_filter = new JPH::ObjectVsBroadPhaseLayerFilterTable(
        *impl.broadphase, Layers::kNumBroadPhaseLayers, *impl.layer_filter, Layers::kNumObjectLayers);

    constexpr JPH::uint kMaxBodies = 4096;
    constexpr JPH::uint kBodyMutexes = 0;
    constexpr JPH::uint kMaxPairs = 8192;
    constexpr JPH::uint kMaxContacts = 4096;
    impl.physics.Init(kMaxBodies, kBodyMutexes, kMaxPairs, kMaxContacts, *impl.broadphase,
                      *impl.bp_filter, *impl.layer_filter);
    JPH::PhysicsSettings ps;
    ps.mNumVelocitySteps = settings.velocity_iterations;
    ps.mNumPositionSteps = settings.position_iterations;
    ps.mBaumgarte = settings.baumgarte;
    ps.mPenetrationSlop = settings.penetration_slop;
    impl.physics.SetPhysicsSettings(ps);
    // Gravity lives on the system (not the settings) since Jolt 5.x.
    impl.physics.SetGravity(JPH::Vec3(settings.gravity.x, settings.gravity.y, settings.gravity.z));
    impl.ok = true;
}

JoltWorld::~JoltWorld() {
    m_impl.reset();
    std::lock_guard lock(jolt_mutex());
    if (jolt_refs().fetch_sub(1) <= 1) {
        jolt_refs().store(0);
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

bool JoltWorld::valid() const {
    return m_impl && m_impl->ok;
}

JoltBody JoltWorld::add_body(const BodyDesc& desc) {
    JoltBody out;
    if (!valid()) return out;
    Impl& impl = *m_impl;

    JPH::ShapeRefC shape;
    switch (desc.shape.type) {
        case ShapeType::Sphere:
            shape = new JPH::SphereShape(desc.shape.sphere.radius);
            break;
        case ShapeType::Box: {
            const Vec3& h = desc.shape.box.half_extents;
            shape = new JPH::BoxShape(JPH::Vec3(h.x, h.y, h.z));
            break;
        }
        case ShapeType::Plane: {
            const Vec3& n = desc.shape.plane.normal;
            shape = new JPH::PlaneShape(JPH::Plane(JPH::Vec3(n.x, n.y, n.z), 0.0f));
            break;
        }
        default:
            // Unsupported shape (capsule/mesh/...): fall back to a bounding
            // sphere so the body still simulates instead of vanishing.
            shape = new JPH::SphereShape(0.5f);
            break;
    }

    const bool dynamic = desc.type == BodyType::Dynamic;
    JPH::BodyCreationSettings settings(
        shape, JPH::RVec3(desc.position.x, desc.position.y, desc.position.z),
        JPH::Quat::sIdentity(),
        dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
        dynamic ? Layers::kMoving : Layers::kNonMoving);
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mLinearDamping = desc.linear_damping;
    settings.mAngularDamping = desc.angular_damping;
    settings.mAllowSleeping = desc.allow_sleep;
    if (dynamic && desc.mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
    }
    JPH::BodyID id =
        impl.physics.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid()) return out;
    out.id = id.GetIndexAndSequenceNumber();
    return out;
}

void JoltWorld::remove_body(JoltBody handle) {
    if (!valid() || !handle.valid()) return;
    Impl& impl = *m_impl;
    JPH::BodyID id(handle.id);
    if (impl.physics.GetBodyInterface().IsAdded(id)) {
        impl.physics.GetBodyInterface().RemoveBody(id);
    }
    impl.physics.GetBodyInterface().DestroyBody(id);
}

bool JoltWorld::is_alive(JoltBody handle) const {
    if (!valid() || !handle.valid()) return false;
    const Impl& impl = *m_impl;
    JPH::BodyID id(handle.id);
    // Added == live in the simulation (removal destroys immediately).
    return const_cast<JPH::BodyInterface&>(impl.physics.GetBodyInterface()).IsAdded(id);
}

JoltBodyState JoltWorld::state(JoltBody handle) const {
    JoltBodyState out;
    if (!is_alive(handle)) return out;
    const Impl& impl = *m_impl;
    JPH::BodyID id(handle.id);
    const JPH::RVec3 p =
        const_cast<JPH::BodyInterface&>(impl.physics.GetBodyInterface()).GetCenterOfMassPosition(id);
    const JPH::Vec3 v =
        const_cast<JPH::BodyInterface&>(impl.physics.GetBodyInterface()).GetLinearVelocity(id);
    out.position = Vec3{static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                        static_cast<float>(p.GetZ())};
    out.linear_velocity = Vec3{v.GetX(), v.GetY(), v.GetZ()};
    return out;
}

void JoltWorld::set_linear_velocity(JoltBody handle, const Vec3& velocity) {
    if (!is_alive(handle)) return;
    Impl& impl = *m_impl;
    impl.physics.GetBodyInterface().SetLinearVelocity(
        JPH::BodyID(handle.id), JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

void JoltWorld::step(float dt) {
    if (!valid() || !(dt > 0.0f)) return;
    Impl& impl = *m_impl;
    impl.physics.Update(dt, 1, impl.temp, impl.jobs);
}

usize JoltWorld::body_count() const {
    if (!valid()) return 0;
    return m_impl->physics.GetNumBodies();
}

} // namespace nf::physics
