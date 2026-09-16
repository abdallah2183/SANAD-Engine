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
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/VehicleController.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <NF/Physics/JoltWorld.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
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

// --- Scene query plumbing (rays, sweeps, overlaps, sensors) ----------------
//
// Conversion helpers. JPH_DOUBLE_PRECISION is OFF in this build, so RVec3 and
// Vec3 are the same type and RVec3Arg == Vec3Arg: the "real" variants below
// cannot be overloads (they would be redefinitions) and are named instead.
// They are kept separate so enabling double precision later only costs a
// rebuild, not a rewrite of these queries.
JPH::Vec3 to_jph(const Vec3& v) {
    return JPH::Vec3(v.x, v.y, v.z);
}
JPH::RVec3 to_jph_real(const Vec3& v) {
    return JPH::RVec3(v.x, v.y, v.z);
}
Vec3 to_nf(JPH::Vec3Arg v) {
    return Vec3{v.GetX(), v.GetY(), v.GetZ()};
}
Vec3 to_nf_real(JPH::RVec3Arg v) {
    return Vec3{static_cast<float>(v.GetX()), static_cast<float>(v.GetY()),
                static_cast<float>(v.GetZ())};
}

/// Skips sensor (trigger) bodies and, optionally, one ignored body.
///
/// The two halves of the filter are called at different times by Jolt:
/// ShouldCollide() per broadphase candidate, where only the id is known (so
/// the ignored body is a plain id compare and a dead id simply matches
/// nothing), and ShouldCollideLocked() once the body is locked — the only
/// point where sensor-ness is visible.
class SkipSensorsAndBody final : public JPH::BodyFilter {
public:
    explicit SkipSensorsAndBody(JPH::BodyID ignore = JPH::BodyID()) : mIgnore(ignore) {}

    bool ShouldCollide(const JPH::BodyID& inBodyID) const override { return inBodyID != mIgnore; }
    bool ShouldCollideLocked(const JPH::Body& inBody) const override { return !inBody.IsSensor(); }

private:
    JPH::BodyID mIgnore;
};

/// Engine handle -> filter id. An invalid handle means "ignore nothing", so
/// callers can pass their ignore argument through unconditionally.
JPH::BodyID ignore_id(JoltBody handle) {
    return handle.valid() ? JPH::BodyID(handle.id) : JPH::BodyID();
}

/// Shared body of overlap_sphere / overlap_box / trigger_overlaps: collide
/// `shape` (positioned by `transform`) against the world and return the
/// overlapping body handles, deduped per body and without sensors.
std::vector<JoltBody> collect_overlaps(const JPH::PhysicsSystem& physics, const JPH::Shape* shape,
                                       JPH::RMat44Arg transform, JPH::BodyID ignore) {
    std::vector<JoltBody> out;
    // mMaxSeparationDistance stays 0: only true overlaps, no "nearly touching".
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const SkipSensorsAndBody filter(ignore);
    // Base offset zero puts the results straight into world space. Jolt
    // suggests a nearby offset for precision far from the origin; this wrapper
    // hands out float Vec3 positions everywhere, so a double offset would buy
    // accuracy that is thrown away one line later.
    physics.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sOne(), transform, settings,
                                               JPH::RVec3::sZero(), collector, {}, {}, filter);
    // A shape with several parts reports one hit per part: dedupe by body.
    std::vector<JPH::uint32> seen;
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        const JPH::uint32 id = hit.mBodyID2.GetIndexAndSequenceNumber();
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        JoltBody body;
        body.id = id;
        out.push_back(body);
    }
    return out;
}

} // namespace

struct JoltWorld::Impl {
    JPH::PhysicsSystem physics;
    JPH::TempAllocatorImpl* temp = nullptr;
    JPH::JobSystemSingleThreaded* jobs = nullptr;
    JPH::BroadPhaseLayerInterfaceTable* broadphase = nullptr;
    JPH::ObjectLayerPairFilterTable* layer_filter = nullptr;
    JPH::ObjectVsBroadPhaseLayerFilterTable* bp_filter = nullptr;
    std::map<JPH::uint32, JPH::Ref<JPH::Constraint>> constraints;
    JPH::uint32 next_constraint = 1;
    // Vehicle testers keyed by the same id as `constraints`: the
    // VehicleConstraint only holds a RAW tester pointer, so the world must
    // own the Ref for exactly as long as the constraint lives.
    std::map<JPH::uint32, JPH::Ref<JPH::VehicleCollisionTester>> vehicle_testers;
    // Ragdolls: JPH::Ragdoll owns bodies + constraints; bodies vector mirrors
    // GetBodyIDs() for the JoltBody view. Same id space family, own counter.
    struct RagdollEntry {
        JPH::Ref<JPH::Ragdoll> ragdoll;
        std::vector<JoltBody> bodies;
    };
    std::map<JPH::uint32, RagdollEntry> ragdolls;
    JPH::uint32 next_ragdoll = 1;
    bool ok = false;

    ~Impl() {
        constraints.clear(); // released before the system dies
        vehicle_testers.clear();
        ragdolls.clear();
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
    return add_body_internal(desc, /*sensor=*/false);
}

JoltBody JoltWorld::add_trigger(const BodyDesc& desc) {
    // Same creation path as any body, plus Jolt's sensor flag: a sensor is
    // still broadphase-tracked (so queries and trigger_overlaps find it) but
    // contributes no contacts, so dynamics pass through untouched.
    return add_body_internal(desc, /*sensor=*/true);
}

JoltBody JoltWorld::add_body_internal(const BodyDesc& desc, bool sensor) {
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
        JPH::Quat(desc.orientation.x, desc.orientation.y, desc.orientation.z,
                  desc.orientation.w),
        dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
        dynamic ? Layers::kMoving : Layers::kNonMoving);
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mLinearDamping = desc.linear_damping;
    settings.mAngularDamping = desc.angular_damping;
    settings.mAllowSleeping = desc.allow_sleep;
    settings.mIsSensor = sensor; // triggers: overlap detection, no response
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

// --- Scene queries, sensors (triggers), continuous collision -------------
//
// Every entry point below keeps the same defensive shape as the rest of the
// wrapper: reject an invalid world / invalid handle / unusable numeric input
// up front and return a miss or an empty vector. Sensors are filtered out at
// the body-filter level (see SkipSensorsAndBody), so a trigger can never be
// the answer to "what is in front of me".

JoltWorld::QueryHit JoltWorld::ray_cast(Vec3 origin, Vec3 direction, float max_distance,
                                        JoltBody ignore) const {
    QueryHit out;
    if (!valid() || !(max_distance > 0.0f)) return out;
    const float len = direction.length();
    if (!(len > 1e-6f)) return out; // zero-length direction: miss, not NaN
    direction = direction / len;
    const Impl& impl = *m_impl;
    // The cast length lives in the direction vector: Jolt reports a fraction
    // of that length, which is why distance is fraction * max_distance.
    const JPH::RRayCast ray(to_jph_real(origin), to_jph(direction * max_distance));
    const SkipSensorsAndBody filter(ignore_id(ignore));
    JPH::RayCastResult hit;
    if (!impl.physics.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return out;
    // Lock only to read the surface normal; a body removed between cast and
    // lock simply yields a miss instead of a dangling read.
    JPH::BodyLockRead lock(impl.physics.GetBodyLockInterface(), hit.mBodyID);
    if (!lock.Succeeded()) return out;
    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    out.hit = true;
    out.body.id = hit.mBodyID.GetIndexAndSequenceNumber();
    out.position = to_nf_real(point);
    out.normal = to_nf(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
    out.fraction = std::clamp(hit.mFraction, 0.0f, 1.0f);
    out.distance = out.fraction * max_distance;
    return out;
}

JoltWorld::QueryHit JoltWorld::sphere_cast(Vec3 origin, float radius, Vec3 direction,
                                           float max_distance, JoltBody ignore) const {
    QueryHit out;
    if (!valid() || !(radius > 0.0f) || !(max_distance > 0.0f)) return out;
    const float len = direction.length();
    if (!(len > 1e-6f)) return out;
    direction = direction / len;
    const Impl& impl = *m_impl;
    // The cast borrows the shape rather than owning it, so a stack shape keeps
    // this query allocation-free (and off Jolt's allocator entirely).
    const JPH::SphereShape shape(radius);
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        &shape, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(to_jph_real(origin)),
        to_jph(direction * max_distance));
    JPH::ShapeCastSettings settings;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const SkipSensorsAndBody filter(ignore_id(ignore));
    impl.physics.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {},
                                                {}, filter);
    if (!collector.HadHit()) return out;
    const JPH::ShapeCastResult& hit = collector.mHit;
    JPH::BodyLockRead lock(impl.physics.GetBodyLockInterface(), hit.mBodyID2);
    if (!lock.Succeeded()) return out;
    // Contact point 1 is on the swept sphere, point 2 on the hit body: report
    // the point on the cast shape (what the caller collided with) and the
    // normal out of the body actually hit.
    out.hit = true;
    out.body.id = hit.mBodyID2.GetIndexAndSequenceNumber();
    out.position = to_nf(hit.mContactPointOn1);
    out.normal =
        to_nf(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hit.mContactPointOn2));
    out.fraction = std::clamp(hit.mFraction, 0.0f, 1.0f);
    out.distance = out.fraction * max_distance;
    return out;
}

std::vector<JoltBody> JoltWorld::overlap_sphere(Vec3 center, float radius) const {
    if (!valid() || !(radius > 0.0f)) return {};
    const JPH::SphereShape shape(radius);
    return collect_overlaps(m_impl->physics, &shape, JPH::RMat44::sTranslation(to_jph_real(center)),
                            JPH::BodyID());
}

std::vector<JoltBody> JoltWorld::overlap_box(Vec3 center, Vec3 half_extents) const {
    // A degenerate box has no interior to overlap, so treat it as empty input
    // rather than asking Jolt for a zero-extent shape.
    if (!valid() || !(half_extents.x > 0.0f) || !(half_extents.y > 0.0f) ||
        !(half_extents.z > 0.0f)) {
        return {};
    }
    const JPH::BoxShape shape(to_jph(half_extents));
    return collect_overlaps(m_impl->physics, &shape, JPH::RMat44::sTranslation(to_jph_real(center)),
                            JPH::BodyID());
}

std::vector<JoltBody> JoltWorld::trigger_overlaps(JoltBody trigger) const {
    std::vector<JoltBody> out;
    if (!valid() || !trigger.valid() || !is_alive(trigger)) return out;
    const Impl& impl = *m_impl;
    const JPH::BodyID id(trigger.id);
    // Copy shape + transform out under the lock and release it BEFORE the
    // query: Jolt's query machinery locks bodies itself, and holding a second
    // lock on this body while it walks the broadphase is asking for trouble.
    // This is the same pattern Jolt uses internally.
    JPH::ShapeRefC shape;
    JPH::RMat44 transform;
    {
        JPH::BodyLockRead lock(impl.physics.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return out;
        const JPH::Body& body = lock.GetBody();
        shape = body.GetShape();
        transform = body.GetCenterOfMassTransform();
    }
    if (shape == nullptr) return out;
    // Ignore the queried body itself as well as every sensor: without that, a
    // trigger would always report itself.
    return collect_overlaps(impl.physics, shape, transform, id);
}

void JoltWorld::set_continuous_collision(JoltBody handle, bool enabled) {
    if (!valid() || !handle.valid() || !is_alive(handle)) return;
    m_impl->physics.GetBodyInterface().SetMotionQuality(
        JPH::BodyID(handle.id),
        enabled ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
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

void* JoltWorld::system_handle() {
    if (!valid()) return nullptr;
    return &m_impl->physics;
}

JoltConstraint JoltWorld::add_fixed(JoltBody a, JoltBody b) {
    JoltConstraint out;
    if (!valid() || !is_alive(a) || !is_alive(b)) return out;
    Impl& impl = *m_impl;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    JPH::FixedConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mAutoDetectPoint = true; // freeze the current relative transform
    JPH::Ref<JPH::Constraint> c = settings.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

JoltConstraint JoltWorld::add_hinge(JoltBody a, JoltBody b, Vec3 world_point, Vec3 world_axis) {
    JoltConstraint out;
    if (!valid() || !is_alive(a) || !is_alive(b)) return out;
    float len = world_axis.length();
    if (!(len > 1e-6f)) return out;
    world_axis = world_axis / len;
    Impl& impl = *m_impl;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    // A normal perpendicular to the hinge axis (for the zero-angle frame).
    Vec3 helper = std::abs(world_axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 normal = world_axis.cross(helper);
    normal = normal / normal.length();
    JPH::HingeConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mPoint1 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mPoint2 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mHingeAxis1 = JPH::Vec3(world_axis.x, world_axis.y, world_axis.z);
    settings.mHingeAxis2 = JPH::Vec3(world_axis.x, world_axis.y, world_axis.z);
    settings.mNormalAxis1 = JPH::Vec3(normal.x, normal.y, normal.z);
    settings.mNormalAxis2 = JPH::Vec3(normal.x, normal.y, normal.z);
    JPH::Ref<JPH::Constraint> c = settings.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

JoltConstraint JoltWorld::add_point(JoltBody a, JoltBody b, Vec3 world_point) {
    JoltConstraint out;
    if (!valid() || !is_alive(a) || !is_alive(b)) return out;
    Impl& impl = *m_impl;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    JPH::PointConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mPoint1 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mPoint2 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    JPH::Ref<JPH::Constraint> c = settings.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

void JoltWorld::remove_constraint(JoltConstraint handle) {
    if (!valid() || !handle.valid()) return;
    Impl& impl = *m_impl;
    auto it = impl.constraints.find(handle.id);
    if (it == impl.constraints.end()) return;
    // Vehicle constraints are ALSO step listeners owned via vehicle_* wiring:
    // unregister the listener first, then the constraint, then the tester.
    if (auto* step = dynamic_cast<JPH::PhysicsStepListener*>(it->second.GetPtr()))
        impl.physics.RemoveStepListener(step);
    impl.physics.RemoveConstraint(it->second);
    impl.constraints.erase(it);
    impl.vehicle_testers.erase(handle.id);
}

// --- Vehicle wiring (ALL Jolt vehicle objects are born here, in the TU that
// --- owns the PhysicsSystem — see JoltVehicle.hpp implementation note).
// ---
// --- LIFECYCLE (read before touching): VehicleConstraint stores the chassis
// --- as a RAW Body* plus a VehicleCollisionTester* with no null guard in
// --- OnStep. Consequently:
// ---   1. CreateBody -> SetVehicleCollisionTester -> AddBody ->
// ---      AddConstraint -> AddStepListener, in EXACTLY this order.
// ---      Constraint/StepListener before AddBody corrupts the heap.
// ---   2. Teardown is the exact reverse (see vehicle_destroy +
// ---      remove_constraint above).

JoltWorld::VehicleHandle JoltWorld::vehicle_create(const JoltVehicleConfig& config, Vec3 spawn) {
    VehicleHandle out;
    if (!valid()) return out;
    Impl& impl = *m_impl;

    // Chassis body (world-owned, same pattern as add_body but with explicit
    // vehicle mass override).
    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_box(config.chassis_half_extents);
    desc.position = spawn;
    desc.friction = 0.8f;
    desc.restitution = 0.0f;
    desc.mass = config.chassis_mass > 0.0f ? config.chassis_mass : 1500.0f;
    const JoltBody chassis = add_body(desc);
    if (!chassis.valid()) return out;
    out.chassis = chassis;

    // Wheels: front axle (+z) steers, rear axle drives.
    JPH::VehicleConstraintSettings settings;
    settings.mUp = JPH::Vec3(0, 1, 0);
    settings.mForward = JPH::Vec3(0, 0, 1);
    settings.mMaxPitchRollAngle = 1.2f; // stay rubber-side down-ish
    const float xs[2] = {-config.track_half_width, config.track_half_width};
    const float zs[2] = {config.wheelbase_half_length, -config.wheelbase_half_length};
    for (int axle = 0; axle < 2; ++axle) {
        for (int side = 0; side < 2; ++side) {
            auto* wheel = new JPH::WheelSettingsWV();
            wheel->mPosition = JPH::Vec3(xs[side], config.wheel_y, zs[axle]);
            wheel->mRadius = config.wheel_radius;
            wheel->mWidth = config.wheel_width;
            wheel->mMaxSteerAngle = (axle == 0) ? config.max_steer_deg * 0.0174533f : 0.0f;
            settings.mWheels.push_back(wheel);
        }
    }
    auto* controller = new JPH::WheeledVehicleControllerSettings();
    controller->mEngine.mMaxTorque = config.engine_max_torque;
    // Rear-wheel drive: wheels are axle-major [FL, FR, RL, RR] -> rear is (2,3).
    JPH::VehicleDifferentialSettings& diff = controller->mDifferentials.emplace_back();
    diff.mLeftWheel = 2;
    diff.mRightWheel = 3;
    diff.mLimitedSlipRatio = 1.4f;
    diff.mEngineTorqueRatio = 1.0f;
    settings.mController = controller;

    // Lock the chassis Body* WITHOUT crossing a TU boundary: everything from
    // here on runs inside this TU against impl.physics.
    JPH::BodyLockWrite lock(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(chassis.id));
    if (!lock.Succeeded()) {
        remove_body(chassis);
        out.chassis = JoltBody{};
        return out;
    }
    JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(lock.GetBody(), settings);
    JPH::Ref<JPH::VehicleCollisionTester> tester =
        new JPH::VehicleCollisionTesterCastCylinder(Layers::kMoving);
    constraint->SetVehicleCollisionTester(tester);
    impl.physics.AddConstraint(constraint);
    impl.physics.AddStepListener(constraint);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = constraint;
    impl.vehicle_testers[id] = tester;
    out.constraint_id = id;
    return out;
}

void JoltWorld::vehicle_destroy(VehicleHandle handle) {
    if (!valid() || !handle.chassis.valid()) return;
    if (handle.constraint_id != 0) {
        JoltConstraint c;
        c.id = handle.constraint_id;
        remove_constraint(c); // unregisters listener + constraint + tester
    }
    remove_body(handle.chassis);
}

JoltBodyState JoltWorld::vehicle_chassis_state(VehicleHandle handle) const {
    if (!valid() || !handle.chassis.valid()) return JoltBodyState{};
    return state(handle.chassis);
}

void JoltWorld::vehicle_drive(VehicleHandle handle, float forward, float steer, float brake) {
    if (!valid() || handle.constraint_id == 0) return;
    Impl& impl = *m_impl;
    auto it = impl.constraints.find(handle.constraint_id);
    if (it == impl.constraints.end()) return;
    auto* vehicle = static_cast<JPH::VehicleConstraint*>(it->second.GetPtr());
    auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle->GetController());
    if (!controller) return;
    // Drive input must wake the sleeping chassis: the controller only applies
    // torque in PreCollide/PostCollide while the body is active, and a parked
    // car sleeps after ~2s (AllowSleep). SetDriverInput alone never wakes it.
    impl.physics.GetBodyInterface().ActivateBody(JPH::BodyID(handle.chassis.id));
    controller->SetDriverInput(std::clamp(forward, -1.0f, 1.0f), std::clamp(steer, -1.0f, 1.0f),
                               std::clamp(brake, 0.0f, 1.0f), 0.0f);
}

// --- Ragdoll wiring (moved verbatim from the old JoltRagdoll.cpp TU so all
// --- Jolt allocation stays in this TU — see JoltRagdoll.hpp note).
// --- (Bodies + constraint creation below; queries follow in the next block.)

JoltWorld::RagdollHandle JoltWorld::ragdoll_create(const std::vector<RagdollJointDesc>& joints,
                                                   Vec3 spawn) {
    RagdollHandle out;
    if (!valid() || joints.empty()) return out;
    Impl& impl = *m_impl;

    auto skeleton = new JPH::Skeleton();
    for (const auto& j : joints) skeleton->AddJoint(j.name, j.parent);
    skeleton->CalculateParentJointIndices();

    JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
    settings->mSkeleton = skeleton;
    settings->mParts.resize(joints.size());

    for (usize i = 0; i < joints.size(); ++i) {
        const auto& j = joints[i];
        JPH::RagdollSettings::Part& part = settings->mParts[i];
        part.SetShape(new JPH::SphereShape(j.radius));
        part.mPosition = JPH::RVec3(spawn.x + j.local_offset.x,
                                    spawn.y + j.local_offset.y,
                                    spawn.z + j.local_offset.z);
        part.mRotation = JPH::Quat::sIdentity();
        part.mMotionType = JPH::EMotionType::Dynamic;
        part.mObjectLayer = Layers::kMoving;
        part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        part.mMassPropertiesOverride.mMass = j.mass > 0.0f ? j.mass : 1.0f;
        part.mFriction = 0.7f;
        part.mRestitution = 0.0f;

        if (!j.parent.empty()) {
            auto* st = new JPH::SwingTwistConstraintSettings();
            st->mPosition1 = JPH::RVec3(j.local_offset.x * 0.5f,
                                        j.local_offset.y * 0.5f,
                                        j.local_offset.z * 0.5f);
            st->mPosition2 = JPH::RVec3(0, 0, 0);
            st->mTwistAxis1 = JPH::Vec3(0, 0, 1);
            st->mTwistAxis2 = JPH::Vec3(0, 0, 1);
            st->mPlaneAxis1 = JPH::Vec3(1, 0, 0);
            st->mPlaneAxis2 = JPH::Vec3(1, 0, 0);
            st->mNormalHalfConeAngle = j.swing_limit_deg * 0.0174533f;
            st->mPlaneHalfConeAngle = j.swing_limit_deg * 0.0174533f;
            st->mTwistMinAngle = -j.twist_limit_deg * 0.0174533f;
            st->mTwistMaxAngle = j.twist_limit_deg * 0.0174533f;
            part.mToParent = st;
        }
    }

    settings->Stabilize();
    settings->DisableParentChildCollisions();

    // NOTE: fixed group id is fine while ragdolls are short-lived test
    // objects; production code needs a per-ragdoll unique GroupID.
    JPH::Ref<JPH::Ragdoll> ragdoll = settings->CreateRagdoll(0x12345, 0, &impl.physics);
    if (!ragdoll) return out;
    ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

    Impl::RagdollEntry entry;
    entry.ragdoll = ragdoll;
    const auto& body_ids = ragdoll->GetBodyIDs();
    entry.bodies.reserve(joints.size());
    for (usize i = 0; i < joints.size() && i < body_ids.size(); ++i) {
        JoltBody b;
        b.id = body_ids[i].GetIndexAndSequenceNumber();
        entry.bodies.push_back(b);
    }
    const JPH::uint32 id = impl.next_ragdoll++;
    impl.ragdolls[id] = std::move(entry);
    out.id = id;
    return out;
}

void JoltWorld::ragdoll_destroy(RagdollHandle handle) {
    if (!valid() || !handle.valid()) return;
    auto it = m_impl->ragdolls.find(handle.id);
    if (it == m_impl->ragdolls.end()) return;
    // RemoveFromPhysicsSystem BEFORE the Ref dies: the Ragdoll destructor
    // destroys bodies that must already be out of the broadphase.
    it->second.ragdoll->RemoveFromPhysicsSystem();
    m_impl->ragdolls.erase(it);
}

usize JoltWorld::ragdoll_joint_count(RagdollHandle handle) const {
    if (!valid() || !handle.valid()) return 0;
    auto it = m_impl->ragdolls.find(handle.id);
    if (it == m_impl->ragdolls.end()) return 0;
    return it->second.bodies.size();
}

JoltBody JoltWorld::ragdoll_body_at(RagdollHandle handle, usize joint_index) const {
    if (!valid() || !handle.valid()) return JoltBody{};
    auto it = m_impl->ragdolls.find(handle.id);
    if (it == m_impl->ragdolls.end() || joint_index >= it->second.bodies.size()) return JoltBody{};
    return it->second.bodies[joint_index];
}

std::vector<RagdollJointState> JoltWorld::ragdoll_joint_states(RagdollHandle handle) const {
    std::vector<RagdollJointState> out;
    if (!valid() || !handle.valid()) return out;
    auto it = m_impl->ragdolls.find(handle.id);
    if (it == m_impl->ragdolls.end()) return out;
    out.reserve(it->second.bodies.size());
    JPH::BodyInterface& bi =
        const_cast<JPH::BodyInterface&>(m_impl->physics.GetBodyInterface());
    for (const JoltBody& b : it->second.bodies) {
        RagdollJointState s;
        s.body = b;
        if (is_alive(b)) {
            const JPH::RVec3 p = bi.GetCenterOfMassPosition(JPH::BodyID(b.id));
            const JPH::Vec3 v = bi.GetLinearVelocity(JPH::BodyID(b.id));
            s.position = Vec3{static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                              static_cast<float>(p.GetZ())};
            s.linear_velocity = Vec3{v.GetX(), v.GetY(), v.GetZ()};
        }
        out.push_back(s);
    }
    return out;
}

void JoltWorld::ragdoll_apply_impulse(RagdollHandle handle, usize joint_index, const Vec3& impulse) {
    if (!valid() || !handle.valid()) return;
    auto it = m_impl->ragdolls.find(handle.id);
    if (it == m_impl->ragdolls.end() || joint_index >= it->second.bodies.size()) return;
    m_impl->physics.GetBodyInterface().AddImpulse(JPH::BodyID(it->second.bodies[joint_index].id),
                                                  JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void JoltWorld::ragdoll_activate(RagdollHandle handle) {
    if (!valid() || !handle.valid()) return;
    auto it = m_impl->ragdolls.find(handle.id);
    if (it == m_impl->ragdolls.end()) return;
    it->second.ragdoll->Activate();
}
JoltConstraint JoltWorld::add_slider(JoltBody a, JoltBody b, Vec3 world_point, Vec3 world_axis) {
    JoltConstraint out;
    if (!valid() || !is_alive(a) || !is_alive(b)) return out;
    float len = world_axis.length();
    if (!(len > 1e-6f)) return out;
    world_axis = world_axis / len;
    Impl& impl = *m_impl;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    JPH::SliderConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mPoint1 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mPoint2 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mSliderAxis1 = JPH::Vec3(world_axis.x, world_axis.y, world_axis.z);
    settings.mSliderAxis2 = JPH::Vec3(world_axis.x, world_axis.y, world_axis.z);
    settings.mNormalAxis1 = JPH::Vec3(0, 1, 0);
    settings.mNormalAxis2 = JPH::Vec3(0, 1, 0);
    settings.mLimitsMin = -1.0f;  // default ±1m
    settings.mLimitsMax = 1.0f;
    JPH::Ref<JPH::Constraint> c = settings.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

JoltConstraint JoltWorld::add_distance(JoltBody a, JoltBody b, Vec3 anchor_a, Vec3 anchor_b,
                                       float min_dist, float max_dist) {
    JoltConstraint out;
    if (!valid() || !is_alive(a) || !is_alive(b)) return out;
    if (!(min_dist >= 0.0f) || !(max_dist >= min_dist)) return out;
    Impl& impl = *m_impl;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    JPH::DistanceConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mPoint1 = JPH::RVec3(anchor_a.x, anchor_a.y, anchor_a.z);
    settings.mPoint2 = JPH::RVec3(anchor_b.x, anchor_b.y, anchor_b.z);
    settings.mMinDistance = min_dist;
    settings.mMaxDistance = max_dist;
    JPH::Ref<JPH::Constraint> c = settings.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

JoltConstraint JoltWorld::add_cone(JoltBody a, JoltBody b, Vec3 world_point, Vec3 world_axis,
                                   float max_angle_rad) {
    JoltConstraint out;
    if (!valid() || !is_alive(a) || !is_alive(b)) return out;
    if (!(max_angle_rad > 0.0f)) return out;
    float len = world_axis.length();
    if (!(len > 1e-6f)) return out;
    world_axis = world_axis / len;
    Impl& impl = *m_impl;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    JPH::ConeConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mPoint1 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mPoint2 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mTwistAxis1 = JPH::Vec3(world_axis.x, world_axis.y, world_axis.z);
    settings.mTwistAxis2 = JPH::Vec3(world_axis.x, world_axis.y, world_axis.z);
    settings.mHalfConeAngle = max_angle_rad;
    JPH::Ref<JPH::Constraint> c = settings.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

JoltConstraint JoltWorld::add_sixdof(JoltBody a, JoltBody b, Vec3 world_point,
                                     const Vec3 limit_min[6], const Vec3 limit_max[6]) {
    JoltConstraint out;
    if (!valid() || !is_alive(a) || !is_alive(b)) return out;
    Impl& impl = *m_impl;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    JPH::SixDOFConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mPosition1 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mPosition2 = JPH::RVec3(world_point.x, world_point.y, world_point.z);
    settings.mAxisX1 = JPH::Vec3(1, 0, 0);
    settings.mAxisY1 = JPH::Vec3(0, 1, 0);
    settings.mAxisX2 = JPH::Vec3(1, 0, 0);
    settings.mAxisY2 = JPH::Vec3(0, 1, 0);
    // Map our 6 axes: 0,1,2 = translation X,Y,Z; 3,4,5 = rotation X,Y,Z
    // Jolt EAxis order: TranslationX, TranslationY, TranslationZ, RotationX, RotationY, RotationZ
    for (int i = 0; i < 6; ++i) {
        settings.mLimitMin[i] = limit_min[i].x;
        settings.mLimitMax[i] = limit_max[i].x;
    }
    JPH::Ref<JPH::Constraint> c = settings.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

} // namespace nf::physics
