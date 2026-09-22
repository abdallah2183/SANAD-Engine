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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
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
#include <cfloat>
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

// --- Body creation plumbing -----------------------------------------------
//
// add_body, add_trigger and the complex-shape entry points (capsule, convex
// hull, mesh) all end the same way: shape-independent settings, mass override,
// activation, handle. Keeping that tail in ONE place is what makes a capsule
// or a mesh body indistinguishable from a sphere to the rest of the wrapper
// (remove_body / body_count / is_alive / every query), and what keeps a new
// shape from silently drifting away from that bookkeeping.

/// Rejects NaN/Inf coordinates before they reach Jolt: its shape builders
/// assert on non-finite input, so this is the difference between an invalid
/// handle and a debug-build crash.
bool all_finite(const std::vector<Vec3>& points) {
    for (const Vec3& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    return true;
}

/// Shape-independent tail of every creation path. `shape` must be non-null
/// (callers bail out on a failed Create()). `sensor` is the trigger flag.
/// `force_static` overrides `desc.type`: a STATIC body whatever the caller
/// asked for (a Jolt mesh collider may not move).
JoltBody create_body_from_shape(JPH::PhysicsSystem& physics, const JPH::ShapeRefC& shape,
                                const BodyDesc& desc, bool sensor, bool force_static) {
    JoltBody out;
    if (shape == nullptr) return out;
    // Kinematic maps to Static here (as in add_body): the wrapper's motion
    // vocabulary is "static or dynamic", and a body that only moves through
    // set_linear_velocity() must not be integrated by the solver.
    const bool dynamic = !force_static && desc.type == BodyType::Dynamic;
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
        physics.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid()) return out;
    out.id = id.GetIndexAndSequenceNumber();
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
    // Characters (design §39): one JPH::CharacterVirtual each plus the two
    // capsule shapes a crouch swaps between. These are NOT bodies — the system
    // knows nothing about them, so they never show up in body_count() and no
    // other subsystem can reach them.
    struct CharacterEntry {
        JPH::Ref<JPH::CharacterVirtual> character;
        JPH::ShapeRefC standing_shape;
        JPH::ShapeRefC crouch_shape;
        // Local offset that puts each capsule's BOTTOM at the character
        // position (see character_create), so the feet stay planted across a
        // stance change.
        JPH::Vec3 standing_offset = JPH::Vec3::sZero();
        JPH::Vec3 crouch_offset = JPH::Vec3::sZero();
        JoltCharacterConfig config;
        bool crouched = false;
        bool climbing = false;
    };
    std::map<JPH::uint32, CharacterEntry> characters;
    JPH::uint32 next_character = 1;
    bool ok = false;

    ~Impl() {
        constraints.clear(); // released before the system dies
        vehicle_testers.clear();
        ragdolls.clear();
        characters.clear();
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
            // Unsupported shape (the closed first-party vocabulary has no
            // capsule/cylinder/...): fall back to a bounding sphere so the body
            // still simulates instead of vanishing. Complex colliders are a
            // separate, explicit decision — see add_capsule_body /
            // add_convex_hull_body / add_mesh_body, which never guess.
            shape = new JPH::SphereShape(0.5f);
            break;
    }

    return create_body_from_shape(impl.physics, shape, desc, sensor, /*force_static=*/false);
}

JoltBody JoltWorld::add_capsule_body(const BodyDesc& base, float radius, float half_height) {
    JoltBody out;
    if (!valid()) return out;
    // `!(x > 0)` rather than `x <= 0`: a NaN radius is rejected here too.
    // half_height == 0 is legal and Jolt turns it into a sphere.
    if (!(radius > 0.0f) || !(half_height >= 0.0f)) return out;
    // Settings rather than the JPH::CapsuleShape constructor on purpose: that
    // constructor asserts half_height > 0, while CapsuleShapeSettings accepts
    // 0 and builds a SphereShape (the degenerate capsule).
    const JPH::CapsuleShapeSettings settings(half_height, radius);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return out;
    return create_body_from_shape(m_impl->physics, result.Get(), base, /*sensor=*/false,
                                  /*force_static=*/false);
}

JoltBody JoltWorld::add_cylinder_body(const BodyDesc& base, float radius, float half_height) {
    JoltBody out;
    if (!valid()) return out;
    // `!(x > 0)` rather than `x <= 0`: a NaN radius or half_height is rejected
    // here too. Jolt's CylinderShape only asserts non-negativity and never
    // clamps the radius the way SphereShape does, so this is the only thing
    // standing between a caller's -1 and a zero-volume collider.
    if (!(radius > 0.0f) || !(half_height > 0.0f)) return out;
    const JPH::CylinderShapeSettings settings(half_height, radius);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return out;
    return create_body_from_shape(m_impl->physics, result.Get(), base, /*sensor=*/false,
                                  /*force_static=*/false);
}

JoltBody JoltWorld::add_convex_hull_body(const BodyDesc& base,
                                         const std::vector<Vec3>& local_points) {
    JoltBody out;
    if (!valid()) return out;
    // The documented floor: a hull is at least a tetrahedron. (Jolt would
    // accept 3 non-collinear points, but a triangle collider is a mesh's job,
    // and the doc promises an invalid handle here.)
    if (local_points.size() < 4 || !all_finite(local_points)) return out;
    JPH::Array<JPH::Vec3> points;
    points.reserve(local_points.size());
    for (const Vec3& p : local_points) points.push_back(to_jph(p));
    // The first Jolt call that can fail on its own: a collinear or coplanar
    // cloud has no hull, and ConvexHullBuilder reports that as an error string
    // rather than building a degenerate shape.
    const JPH::ConvexHullShapeSettings settings(points);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return out;
    // Measured on v5.6: an exactly coplanar cloud does NOT take that error
    // path — the builder accepts it and produces a hull whose volume is 0, and
    // the convex radius then inflates it into a thin slab. Such a shape has no
    // meaningful mass properties (CalculateInertia scales a zero volume, so
    // the body ends up with zero inertia and can never rotate): reject it
    // instead of handing back a silent almost-collider.
    if (!(result.Get()->GetVolume() > 0.0f)) return out;
    return create_body_from_shape(m_impl->physics, result.Get(), base, /*sensor=*/false,
                                  /*force_static=*/false);
}

JoltBody JoltWorld::add_mesh_body(const BodyDesc& base, const std::vector<Vec3>& vertices,
                                  const std::vector<u32>& indices) {
    JoltBody out;
    if (!valid()) return out;
    // Validate before handing anything to Jolt: MeshShapeSettings::Sanitize()
    // runs from its constructor and indexes the vertex list with the raw
    // triangle indices, so an out-of-range index is an out-of-bounds read
    // there — not a reportable error. Same for a ragged index list.
    if (vertices.size() < 3 || indices.empty() || indices.size() % 3 != 0) return out;
    if (!all_finite(vertices)) return out;
    for (u32 index : indices) {
        if (index >= vertices.size()) return out;
    }

    JPH::VertexList jph_vertices;
    jph_vertices.reserve(vertices.size());
    for (const Vec3& v : vertices) jph_vertices.push_back(JPH::Float3(v.x, v.y, v.z));
    JPH::IndexedTriangleList jph_triangles;
    jph_triangles.reserve(indices.size() / 3);
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        jph_triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2],
                                                     /*material=*/0));
    }
    const JPH::MeshShapeSettings settings(std::move(jph_vertices), std::move(jph_triangles));
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return out; // e.g. every triangle was degenerate
    // Mesh colliders may not move in Jolt (Shape::MustBeStatic), so the body
    // is forced Static: a Dynamic mesh would either be rejected or fall apart,
    // and neither is what a caller asking for a wall wants.
    return create_body_from_shape(m_impl->physics, result.Get(), base, /*sensor=*/false,
                                  /*force_static=*/true);
}

JoltBody JoltWorld::add_heightfield_body(const BodyDesc& base, const std::vector<float>& heights,
                                         u32 sample_count, Vec3 offset, Vec3 scale) {
    JoltBody out;
    if (!valid()) return out;
    // Validate up front for the same reason add_mesh_body does: Jolt's settings
    // object indexes the sample buffer from its constructor, so a buffer that
    // does not match the declared grid size is an out-of-bounds read over there
    // rather than a reportable error here.
    // `sample_count / block_size >= 2` with Jolt's default block size of 2, so
    // 4 is the smallest field the builder accepts.
    if (sample_count < 4) return out;
    const usize expected = static_cast<usize>(sample_count) * static_cast<usize>(sample_count);
    if (heights.size() != expected) return out;
    for (const float h : heights) {
        if (!std::isfinite(h)) return out;
    }
    // A zero X or Z scale collapses every row or column onto one line, leaving
    // no surface to collide with. A zero Y scale is a flat field, which is a
    // legal plane and is left alone.
    if (scale.x == 0.0f || scale.z == 0.0f) return out;

    JPH::HeightFieldShapeSettings settings(heights.data(), to_jph(offset), to_jph(scale),
                                           sample_count);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return out; // e.g. every sample at the "no collision" sentinel

    // Height fields may not move in Jolt (HeightFieldShape::MustBeStatic), same
    // rule and same reasoning as the mesh collider above.
    return create_body_from_shape(m_impl->physics, result.Get(), base, /*sensor=*/false,
                                  /*force_static=*/true);
}

JoltWorld::CompoundShape JoltWorld::CompoundShape::make_sphere(float radius) {
    CompoundShape out;
    out.type = Type::Sphere;
    out.radius = radius;
    return out;
}

JoltWorld::CompoundShape JoltWorld::CompoundShape::make_box(const Vec3& half_extents) {
    CompoundShape out;
    out.type = Type::Box;
    out.half_extents = half_extents;
    return out;
}

JoltWorld::CompoundShape JoltWorld::CompoundShape::make_cylinder(float radius, float half_height) {
    CompoundShape out;
    out.type = Type::Cylinder;
    out.radius = radius;
    out.half_height = half_height;
    return out;
}

JoltWorld::CompoundShape JoltWorld::CompoundShape::make_capsule(float radius, float half_height) {
    CompoundShape out;
    out.type = Type::Capsule;
    out.radius = radius;
    out.half_height = half_height;
    return out;
}

JoltBody JoltWorld::add_compound_body(const BodyDesc& base, const std::vector<CompoundPart>& parts) {
    JoltBody out;
    if (!valid()) return out;
    // Jolt requires at least 2 sub shapes: a single part is not a compound, and
    // its builder returns an error rather than a RotatedTranslatedShape.
    if (parts.size() < 2) return out;

    JPH::StaticCompoundShapeSettings settings;
    for (const CompoundPart& part : parts) {
        // A degenerate part is rejected, not clamped: the first-party Shape
        // factories clamp degenerate geometry so the solver can assume a
        // positive radius, but a part reaches Jolt alone, and Jolt's cylinder
        // and capsule builders pass a negative radius through unnormalised. The
        // per-shape floors mirror the standalone entry points — see
        // add_cylinder_body for why a zero-height cylinder is refused while a
        // zero-height capsule is a sphere.
        JPH::ShapeRefC sub;
        switch (part.shape.type) {
            case CompoundShape::Type::Sphere:
                if (!(part.shape.radius > 0.0f)) return out;
                sub = new JPH::SphereShape(part.shape.radius);
                break;
            case CompoundShape::Type::Box: {
                const Vec3& h = part.shape.half_extents;
                if (!(h.x > 0.0f) || !(h.y > 0.0f) || !(h.z > 0.0f)) return out;
                sub = new JPH::BoxShape(JPH::Vec3(h.x, h.y, h.z));
                break;
            }
            case CompoundShape::Type::Cylinder: {
                if (!(part.shape.radius > 0.0f) || !(part.shape.half_height > 0.0f)) return out;
                // A settings object is not a shape: it has to be cooked first,
                // the same as the standalone add_cylinder_body entry point.
                const JPH::CylinderShapeSettings cyl(part.shape.half_height, part.shape.radius);
                const JPH::ShapeSettings::ShapeResult cooked = cyl.Create();
                if (cooked.HasError()) return out;
                sub = cooked.Get();
                break;
            }
            case CompoundShape::Type::Capsule: {
                if (!(part.shape.radius > 0.0f) || !(part.shape.half_height >= 0.0f)) return out;
                const JPH::CapsuleShapeSettings cap(part.shape.half_height, part.shape.radius);
                const JPH::ShapeSettings::ShapeResult cooked = cap.Create();
                if (cooked.HasError()) return out;
                sub = cooked.Get();
                break;
            }
        }
        // A shape type the switch does not build leaves `sub` unset; Jolt
        // reports a null sub shape as a creation error, so the failure is still
        // a clean invalid handle rather than a crash.
        settings.AddShape(JPH::Vec3(part.position.x, part.position.y, part.position.z),
                          JPH::Quat(part.orientation.x, part.orientation.y, part.orientation.z,
                                    part.orientation.w),
                          sub.GetPtr());
    }

    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return out;

    // No part forces this shape static (none of the four ever do), so a Dynamic
    // request is honoured — this is the point of the compound, and the
    // difference from the mesh and height field above.
    return create_body_from_shape(m_impl->physics, result.Get(), base, /*sensor=*/false,
                                  /*force_static=*/false);
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
    // GetPosition(), not GetCenterOfMassPosition(): the body's *shape-local
    // origin* in world space, which is what BodyDesc.position was set to and
    // what every caller writes into a scene Transform the mesh is drawn from.
    // Jolt stores the centre-of-mass position in mPosition and derives the
    // origin from it, so reporting the COM would round-trip a hull whose local
    // COM is off-centre (every fracture shard) back offset by that COM — the
    // shard would render displaced and state() would disagree with the pose it
    // was spawned with. Boxes, spheres, capsules and ragdoll parts are all
    // centred, so this is a no-op for every body that existed before hull
    // debris; it makes the round trip exact for the one shape that isn't.
    const JPH::RVec3 p =
        const_cast<JPH::BodyInterface&>(impl.physics.GetBodyInterface()).GetPosition(id);
    const JPH::Vec3 v =
        const_cast<JPH::BodyInterface&>(impl.physics.GetBodyInterface()).GetLinearVelocity(id);
    const JPH::Quat r =
        const_cast<JPH::BodyInterface&>(impl.physics.GetBodyInterface()).GetRotation(id);
    out.position = Vec3{static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                        static_cast<float>(p.GetZ())};
    out.linear_velocity = Vec3{v.GetX(), v.GetY(), v.GetZ()};
    // Jolt stores (x, y, z, w) in the same order as nf::Quat, and keeps body
    // rotations normalized, so the components copy straight across.
    out.rotation = Quat{r.GetX(), r.GetY(), r.GetZ(), r.GetW()};
    return out;
}

void JoltWorld::set_linear_velocity(JoltBody handle, const Vec3& velocity) {
    if (!is_alive(handle)) return;
    Impl& impl = *m_impl;
    impl.physics.GetBodyInterface().SetLinearVelocity(
        JPH::BodyID(handle.id), JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

void JoltWorld::set_angular_velocity(JoltBody handle, const Vec3& velocity) {
    if (!is_alive(handle)) return;
    Impl& impl = *m_impl;
    impl.physics.GetBodyInterface().SetAngularVelocity(
        JPH::BodyID(handle.id), JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

void JoltWorld::apply_impulse(JoltBody handle, const Vec3& impulse) {
    if (!is_alive(handle)) return;
    // BodyInterface::AddImpulse returns without a scratch for a static or
    // kinematic body, so nothing here has to check the body type — a push on a
    // body that cannot move is a no-op, and the caller is allowed not to know.
    m_impl->physics.GetBodyInterface().AddImpulse(
        JPH::BodyID(handle.id), JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void JoltWorld::apply_impulse_at_point(JoltBody handle, const Vec3& impulse,
                                       const Vec3& world_point) {
    if (!is_alive(handle)) return;
    // The point is compared against the body's centre of mass, not its
    // shape-local origin — Jolt's mPosition IS the COM (see state()). For a hull
    // that is not centred on its own origin the two differ, and the torque arm
    // is only physical about the COM.
    m_impl->physics.GetBodyInterface().AddImpulse(
        JPH::BodyID(handle.id), JPH::Vec3(impulse.x, impulse.y, impulse.z),
        JPH::RVec3(world_point.x, world_point.y, world_point.z));
}

void JoltWorld::add_force(JoltBody handle, const Vec3& force) {
    if (!is_alive(handle)) return;
    m_impl->physics.GetBodyInterface().AddForce(
        JPH::BodyID(handle.id), JPH::Vec3(force.x, force.y, force.z));
}

void JoltWorld::add_force_at_point(JoltBody handle, const Vec3& force,
                                   const Vec3& world_point) {
    if (!is_alive(handle)) return;
    m_impl->physics.GetBodyInterface().AddForce(
        JPH::BodyID(handle.id), JPH::Vec3(force.x, force.y, force.z),
        JPH::RVec3(world_point.x, world_point.y, world_point.z));
}

void JoltWorld::add_torque(JoltBody handle, const Vec3& torque) {
    if (!is_alive(handle)) return;
    m_impl->physics.GetBodyInterface().AddTorque(
        JPH::BodyID(handle.id), JPH::Vec3(torque.x, torque.y, torque.z));
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
            // Suspension travel + spring. FrequencyAndDamping keeps the tune
            // mass-independent, so the same Hz works for a 1500 kg car and a
            // 300 kg kart without re-deriving k = m * omega^2.
            wheel->mSuspensionMinLength = std::max(0.0f, config.suspension_min_length);
            wheel->mSuspensionMaxLength = std::max(wheel->mSuspensionMinLength,
                                                   config.suspension_max_length);
            wheel->mSuspensionSpring = JPH::SpringSettings(
                JPH::ESpringMode::FrequencyAndDamping,
                std::max(0.01f, config.suspension_frequency_hz),
                std::max(0.0f, config.suspension_damping));
            wheel->mInertia = std::max(0.01f, config.wheel_inertia);
            wheel->mAngularDamping = std::max(0.0f, config.angular_damping);
            wheel->mMaxBrakeTorque = std::max(0.0f, config.max_brake_torque);
            wheel->mMaxHandBrakeTorque = std::max(0.0f, config.max_handbrake_torque);
            // Friction curves: Jolt's default shape (0 -> peak -> limit) with
            // the peak/limit scaled by the config. Clamped to a positive floor
            // so a mis-tuned config cannot make the wheels frictionless.
            const float peak = std::max(0.05f, config.tire_peak_friction);
            const float limit = std::max(0.05f, config.tire_limit_friction);
            wheel->mLongitudinalFriction.Clear();
            wheel->mLongitudinalFriction.AddPoint(0.0f, 0.0f);
            wheel->mLongitudinalFriction.AddPoint(0.06f, peak);
            wheel->mLongitudinalFriction.AddPoint(0.2f, limit);
            wheel->mLateralFriction.Clear();
            wheel->mLateralFriction.AddPoint(0.0f, 0.0f);
            wheel->mLateralFriction.AddPoint(3.0f, peak);
            wheel->mLateralFriction.AddPoint(20.0f, limit);
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

void JoltWorld::vehicle_drive(VehicleHandle handle, float forward, float steer, float brake,
                              float handbrake) {
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
                               std::clamp(brake, 0.0f, 1.0f),
                               std::clamp(handbrake, 0.0f, 1.0f));
}

std::vector<JoltWheelState> JoltWorld::vehicle_wheel_states(VehicleHandle handle) const {
    std::vector<JoltWheelState> out;
    if (!valid() || handle.constraint_id == 0) return out;
    const Impl& impl = *m_impl;
    const auto it = impl.constraints.find(handle.constraint_id);
    if (it == impl.constraints.end() || it->second == nullptr) return out;
    const auto* vehicle = static_cast<JPH::VehicleConstraint*>(it->second.GetPtr());
    // Wheel world positions come from the chassis body transform, so a read
    // lock on the chassis is enough; the wheel states themselves are plain
    // values the step listener writes between steps (no lock needed to read).
    JPH::BodyLockRead lock(impl.physics.GetBodyLockInterfaceNoLock(),
                           JPH::BodyID(handle.chassis.id));
    if (!lock.Succeeded()) return out;
    const JPH::Body& chassis = lock.GetBody();
    const JPH::RMat44 chassis_tm = chassis.GetCenterOfMassTransform();
    const JPH::Vec3 world_up =
        chassis_tm.GetRotation().Multiply3x3(vehicle->GetLocalUp()).Normalized();
    for (JPH::uint i = 0; i < vehicle->GetWheels().size(); ++i) {
        const JPH::Wheel* w = vehicle->GetWheel(i);
        if (w == nullptr) continue;
        JoltWheelState ws;
        ws.in_contact = w->HasContact();
        ws.suspension_compression =
            std::max(0.0f, w->GetSettings()->mSuspensionMaxLength - w->GetSuspensionLength());
        ws.angular_velocity = w->GetAngularVelocity();
        ws.steer_angle = w->GetSteerAngle();
        if (ws.in_contact) {
            ws.contact_normal = Vec3{w->GetContactNormal().GetX(), w->GetContactNormal().GetY(),
                                     w->GetContactNormal().GetZ()};
        } else {
            ws.contact_normal = Vec3{world_up.GetX(), world_up.GetY(), world_up.GetZ()};
        }
        // Wheel center: start at the suspension hard point, travel down the
        // current suspension length along the world-space suspension axis.
        const JPH::Vec3 local_attach = w->GetSettings()->mPosition;
        const JPH::Vec3 local_dir =
            chassis_tm.GetRotation().Multiply3x3(w->GetSettings()->mSuspensionDirection);
        const JPH::RVec3 world_attach = chassis_tm * JPH::RVec3(local_attach);
        const JPH::RVec3 center = world_attach + JPH::RVec3(local_dir * w->GetSuspensionLength());
        ws.position = Vec3{static_cast<float>(center.GetX()), static_cast<float>(center.GetY()),
                           static_cast<float>(center.GetZ())};
        out.push_back(ws);
    }
    return out;
}

void JoltWorld::vehicle_reset(VehicleHandle handle, Vec3 position) {
    if (!valid() || !handle.chassis.valid()) return;
    Impl& impl = *m_impl;
    JPH::BodyInterface& bi = impl.physics.GetBodyInterface();
    if (!bi.IsAdded(JPH::BodyID(handle.chassis.id))) return;
    // Move the chassis and wipe its velocity in one interface call so the
    // constraint's cached body transform stays consistent with the new pose.
    bi.SetPosition(JPH::BodyID(handle.chassis.id),
                   JPH::RVec3(position.x, position.y, position.z),
                   JPH::EActivation::Activate);
    // Upright is part of the documented contract ("a rolled car resets
    // upright"): a respawn that only moved the chassis would drop a flipped
    // car back onto its roof, which is the one thing the caller asked against.
    bi.SetRotation(JPH::BodyID(handle.chassis.id), JPH::Quat::sIdentity(),
                   JPH::EActivation::Activate);
    bi.SetLinearVelocity(JPH::BodyID(handle.chassis.id), JPH::Vec3::sZero());
    bi.SetAngularVelocity(JPH::BodyID(handle.chassis.id), JPH::Vec3::sZero());
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
            // GetPosition() to match state(): shape-local origin in world, not
            // the COM. Ragdoll parts are spheres, so the two coincide here
            // anyway; the call stays consistent with the rest of the API.
            const JPH::RVec3 p = bi.GetPosition(JPH::BodyID(b.id));
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

// --- Character controller wiring (design §39: step offset, slopes, moving
// --- platforms, crouch, climb hooks, network-prediction hooks).
// ---
// --- ALL CharacterVirtual state is born and driven here: the object is never
// --- handed out of this TU (the header only exposes an opaque id), exactly
// --- like vehicles and ragdolls.
// ---
// --- ANCHORING: Jolt places a character's shape with its centre of mass at
// --- `position + mShapeOffset` (+ character padding), and a CapsuleShape is
// --- centred on its own origin. The shape offset below therefore lifts each
// --- capsule by its own half height, which makes the character position the
// --- capsule's BOTTOM (the feet): the natural gameplay anchor, and the reason
// --- a crouch can swap the capsule without the feet moving.
// ---
// --- VERTICAL SPEED: CharacterVirtual::GetLinearVelocity() returns what the
// --- caller last set (the contact solver reports the solved *displacement*,
// --- not a solved velocity). Adding gravity to the stored velocity every tick
// --- while standing on the floor would therefore accumulate an unbounded
// --- falling speed, so a supported character rebuilds its vertical speed from
// --- the ground's velocity each tick instead — the recipe documented on
// --- CharacterVirtual::ExtendedUpdate.

JoltWorld::CharacterHandle JoltWorld::character_create(const JoltCharacterConfig& config,
                                                       Vec3 spawn) {
    CharacterHandle out;
    if (!valid()) return out;
    Impl& impl = *m_impl;

    // Sanitize: Jolt asserts on a non-positive radius / half height, and a bad
    // config must not take the process down (same defensive style as the rest
    // of the wrapper).
    JoltCharacterConfig cfg = config;
    if (!(cfg.radius > 0.0f)) cfg.radius = 0.35f;
    if (!(cfg.half_height > 0.0f)) cfg.half_height = 0.55f;
    if (!(cfg.crouch_half_height > 0.0f)) cfg.crouch_half_height = cfg.half_height * 0.5f;
    if (!(cfg.max_slope_deg > 0.0f)) cfg.max_slope_deg = 50.0f;

    const float radius = cfg.radius;
    const JPH::Vec3 standing_offset(0.0f, cfg.half_height + radius, 0.0f);
    const JPH::Vec3 crouch_offset(0.0f, cfg.crouch_half_height + radius, 0.0f);
    JPH::ShapeRefC standing_shape = new JPH::CapsuleShape(cfg.half_height, radius);
    JPH::ShapeRefC crouch_shape = new JPH::CapsuleShape(cfg.crouch_half_height, radius);

    JPH::CharacterVirtualSettings settings;
    settings.mShape = standing_shape;
    settings.mShapeOffset = standing_offset;
    settings.mUp = JPH::Vec3(0, 1, 0);
    // Contacts behind this plane support the character, contacts in front only
    // collide with it: at -radius that is "the lower sphere of the capsule can
    // carry the character; its sides and top cannot". The plane lives in the
    // shape's own space, so it is independent of the shape offset above.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
    settings.mMaxSlopeAngle = cfg.max_slope_deg * 0.017453292f; // radians
    settings.mMass = cfg.mass;
    // SDK default, pinned explicitly: 0 makes characters stick to geometry
    // (no sliding direction can be calculated), too large causes ghost
    // collisions.
    settings.mPredictiveContactDistance = 0.1f;
    // Both faces of a thin wall collide: a character must not walk through the
    // back face of a one-sided surface (SDK default, stated for the record).
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;

    Impl::CharacterEntry entry;
    entry.character = new JPH::CharacterVirtual(
        &settings, JPH::RVec3(spawn.x, spawn.y, spawn.z), JPH::Quat::sIdentity(), &impl.physics);
    entry.standing_shape = standing_shape;
    entry.crouch_shape = crouch_shape;
    entry.standing_offset = standing_offset;
    entry.crouch_offset = crouch_offset;
    entry.config = cfg;
    const JPH::uint32 id = impl.next_character++;
    impl.characters[id] = std::move(entry);
    out.id = id;
    return out;
}

void JoltWorld::character_destroy(CharacterHandle handle) {
    if (!valid() || !handle.valid()) return;
    // A CharacterVirtual is not registered with the PhysicsSystem (no inner
    // rigid body is configured here), so dropping the Ref — along with the two
    // shape Refs the entry owns — is the entire teardown. Nothing is left in
    // the broadphase to remove.
    m_impl->characters.erase(handle.id);
}

void JoltWorld::character_move(CharacterHandle handle, Vec3 wish_dir, bool jump, float dt) {
    if (!valid() || !handle.valid() || !(dt > 0.0f)) return;
    Impl& impl = *m_impl;
    auto it = impl.characters.find(handle.id);
    if (it == impl.characters.end()) return;
    Impl::CharacterEntry& entry = it->second;
    JPH::CharacterVirtual* character = entry.character.GetPtr();
    if (character == nullptr) return;
    const JoltCharacterConfig& cfg = entry.config;

    const JPH::Vec3 gravity =
        JPH::Vec3(m_settings.gravity.x, m_settings.gravity.y, m_settings.gravity.z) *
        cfg.gravity_scale;
    // OnSteepGround is deliberately NOT "grounded": a character standing on a
    // slope too steep to walk up must not jump or inherit the slope's slide.
    const bool grounded =
        character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    const JPH::Vec3 current = character->GetLinearVelocity();
    const JPH::Vec3 ground_velocity =
        grounded ? character->GetGroundVelocity() : JPH::Vec3::sZero();

    // Horizontal: accelerate toward wish_dir * max_speed (a wish of (0,0,1)
    // asks for the full cruise speed, (0,0,0.5) for half of it). The
    // acceleration acts on the velocity RELATIVE to the ground: adding the
    // ground velocity on top of a velocity that already contains it would
    // compound it every tick and rocket a character off its moving platform.
    JPH::Vec3 relative(current.GetX() - ground_velocity.GetX(), 0.0f,
                       current.GetZ() - ground_velocity.GetZ());
    const float accel = cfg.acceleration * (grounded ? 1.0f : cfg.air_control);
    const float max_delta = (accel > 0.0f ? accel : 0.0f) * dt;
    const JPH::Vec3 target(wish_dir.x * cfg.max_speed, 0.0f, wish_dir.z * cfg.max_speed);
    JPH::Vec3 delta = target - relative;
    if (delta.Length() > max_delta) delta = delta.Normalized() * max_delta;
    relative += delta;
    const JPH::Vec3 horizontal(relative.GetX() + ground_velocity.GetX(), 0.0f,
                               relative.GetZ() + ground_velocity.GetZ());

    // Vertical: the climb hook replaces gravity with the wish's own vertical
    // component; otherwise a supported character starts from its ground's
    // vertical velocity (0 on a static floor) and an airborne one from its
    // current velocity, which is what makes the jump a real ballistic arc.
    float vertical;
    if (entry.climbing) {
        vertical = wish_dir.y * cfg.max_speed;
    } else {
        vertical = (grounded ? ground_velocity.GetY() : current.GetY()) + gravity.GetY() * dt;
        if (jump && grounded) vertical = cfg.jump_speed;
    }
    character->SetLinearVelocity(JPH::Vec3(horizontal.GetX(), vertical, horizontal.GetZ()));

    // One call runs the whole movement: collide-and-slide, then the stairs and
    // floor-stick passes that give the character its step offset. Their
    // distances are derived from step_offset (Jolt's defaults are 0.4 up /
    // 0.02 minimum forward / 0.15 forward test, i.e. exactly 100% / 5% / 50%
    // of a 0.4m step). Climbing disables both: on a ladder the character must
    // not be snapped back onto the floor it just left.
    JPH::CharacterVirtual::ExtendedUpdateSettings update;
    if (entry.climbing) {
        update.mWalkStairsStepUp = JPH::Vec3::sZero();
        update.mStickToFloorStepDown = JPH::Vec3::sZero();
    } else {
        const float step = cfg.step_offset > 0.0f ? cfg.step_offset : 0.0f;
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, step, 0.0f);
        update.mStickToFloorStepDown = JPH::Vec3(0.0f, -step, 0.0f);
        update.mWalkStairsMinStepForward = 0.05f * step;
        update.mWalkStairsStepForwardTest = 0.5f * step;
    }
    character->ExtendedUpdate(dt, gravity, update, JPH::BroadPhaseLayerFilter(),
                              JPH::ObjectLayerFilter(), JPH::BodyFilter(), JPH::ShapeFilter(),
                              *impl.temp);
}

void JoltWorld::character_set_climbing(CharacterHandle handle, bool enabled) {
    if (!valid() || !handle.valid()) return;
    auto it = m_impl->characters.find(handle.id);
    if (it == m_impl->characters.end()) return;
    it->second.climbing = enabled;
}

void JoltWorld::character_set_crouch(CharacterHandle handle, bool crouched) {
    if (!valid() || !handle.valid()) return;
    auto it = m_impl->characters.find(handle.id);
    if (it == m_impl->characters.end()) return;
    Impl::CharacterEntry& entry = it->second;
    if (entry.crouched == crouched) return;
    JPH::CharacterVirtual* character = entry.character.GetPtr();
    if (character == nullptr) return;

    // Swap the capsule for the other stance, then its offset, so both shapes
    // keep their bottom at the character position (feet planted). FLT_MAX
    // accepts the switch unconditionally: whether a stand-up fits under a
    // ceiling is the game's decision (it owns the crouch state), not a silent
    // veto from the solver that would desynchronise the two.
    const JPH::Shape* shape =
        crouched ? entry.crouch_shape.GetPtr() : entry.standing_shape.GetPtr();
    if (!character->SetShape(shape, FLT_MAX, JPH::BroadPhaseLayerFilter(),
                             JPH::ObjectLayerFilter(), JPH::BodyFilter(), JPH::ShapeFilter(),
                             *m_impl->temp)) {
        return;
    }
    character->SetShapeOffset(crouched ? entry.crouch_offset : entry.standing_offset);
    entry.crouched = crouched;
}

bool JoltWorld::character_is_crouched(CharacterHandle handle) const {
    if (!valid() || !handle.valid()) return false;
    auto it = m_impl->characters.find(handle.id);
    return it != m_impl->characters.end() && it->second.crouched;
}

bool JoltWorld::character_is_grounded(CharacterHandle handle) const {
    if (!valid() || !handle.valid()) return false;
    auto it = m_impl->characters.find(handle.id);
    if (it == m_impl->characters.end()) return false;
    const JPH::CharacterVirtual* character = it->second.character.GetPtr();
    return character != nullptr &&
           character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

Vec3 JoltWorld::character_position(CharacterHandle handle) const {
    if (!valid() || !handle.valid()) return Vec3{0, 0, 0};
    auto it = m_impl->characters.find(handle.id);
    if (it == m_impl->characters.end()) return Vec3{0, 0, 0};
    const JPH::CharacterVirtual* character = it->second.character.GetPtr();
    if (character == nullptr) return Vec3{0, 0, 0};
    return to_nf_real(character->GetPosition());
}

Vec3 JoltWorld::character_velocity(CharacterHandle handle) const {
    if (!valid() || !handle.valid()) return Vec3{0, 0, 0};
    auto it = m_impl->characters.find(handle.id);
    if (it == m_impl->characters.end()) return Vec3{0, 0, 0};
    const JPH::CharacterVirtual* character = it->second.character.GetPtr();
    if (character == nullptr) return Vec3{0, 0, 0};
    return to_nf(character->GetLinearVelocity());
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

JoltConstraint JoltWorld::clone_constraint(JoltConstraint source, JoltBody new_a, JoltBody new_b) {
    JoltConstraint out;
    if (!valid() || !source.valid() || !is_alive(new_a) || !is_alive(new_b)) return out;
    if (new_a == new_b) return out; // a two-body constraint needs two bodies
    Impl& impl = *m_impl;
    const auto src_it = impl.constraints.find(source.id);
    if (src_it == impl.constraints.end() || src_it->second == nullptr) return out;
    JPH::Constraint* src = src_it->second.GetPtr();
    // Only two-body constraints carry a portable settings object: vehicles
    // (and ragdolls) own extra state the settings alone cannot rebuild
    // (tester, step listener, chassis wiring), so they are refused here and
    // cloned through their own dedicated APIs instead.
    if (src->GetType() != JPH::EConstraintType::TwoBodyConstraint) return out;
    JPH::Ref<JPH::ConstraintSettings> settings = src->GetConstraintSettings();
    if (settings == nullptr) return out;

    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(new_a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(), JPH::BodyID(new_b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    // Re-home the settings to the new pair, then rebuild. TwoBodyConstraintSettings
    // is polymorphic, so Create() dispatches to the original constraint's type.
    JPH::TwoBodyConstraintSettings& two =
        static_cast<JPH::TwoBodyConstraintSettings&>(*settings);
    JPH::Ref<JPH::Constraint> c = two.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}

} // namespace nf::physics
