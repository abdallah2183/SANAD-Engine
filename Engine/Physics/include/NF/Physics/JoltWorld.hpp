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
    /// World-space orientation, unit length. Read back together with the
    /// position on purpose: a pose is one atomic fact, and a second query for
    /// the rotation could return a state from a different step than the
    /// position. Vehicles need it on the wire (a snapshot carrying only
    /// position renders a car sliding sideways) and the renderer needs it on
    /// the transform, so both read it from here.
    Quat rotation = Quat::identity();
    bool active = false;
};

/// Per-wheel read state for a JoltVehicle: where the wheel is and what it is
/// touching. Use this to pose render meshes and play surface effects — the
/// physics side is authoritative, the renderer just reads it back.
struct JoltWheelState {
    /// Wheel center in world space (suspension travel applied).
    Vec3 position{0, 0, 0};
    /// Unit contact normal of the ground under the wheel (up if airborne).
    Vec3 contact_normal{0, 1, 0};
    /// Whether the wheel currently touches ground. A vehicle with all wheels
    /// off the ground is airborne: kill downforce-style logic here.
    bool in_contact = false;
    /// Suspension compression from the rest length, in meters (positive =
    /// compressed, 0 at full extension). 0 when airborne.
    float suspension_compression = 0.0f;
    /// Angular velocity of the wheel around its spin axis (rad/s).
    float angular_velocity = 0.0f;
    /// Steering angle of the wheel around the vertical axis (radians; 0 for
    /// non-steered axles).
    float steer_angle = 0.0f;
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

    // --- Suspension + tire tune. Jolt's WheelSettingsWV defaults are a generic
    // car; these are the knobs that keep a full-throttle run straight and a
    // curb crossing rubber-side down. Defaults match Jolt's own so an unset
    // config behaves bit-identically to the pre-tune vehicle.
    float suspension_min_length = 0.30f; // compressed travel (m)
    float suspension_max_length = 0.50f; // droop travel (m)
    float suspension_frequency_hz = 1.5f; // spring stiffness; softer = less twitchy
    float suspension_damping = 0.5f;      // 0 = undamped bounce, >=1 kills oscillation
    float tire_peak_friction = 1.2f;      // grip at the slip peak (can exceed 1: tire profile)
    float tire_limit_friction = 1.0f;     // grip once the tire is sliding
    float wheel_inertia = 0.9f;           // kg m^2
    float angular_damping = 0.2f;         // wheel spin decay
    float max_brake_torque = 1500.0f;     // Nm per wheel
    float max_handbrake_torque = 4000.0f; // Nm, usually rear wheels only
};

/// Jolt character setup (capsule + gameplay movement). Lives here (not in
/// JoltCharacter.hpp) so JoltWorld::character_create can take it without a
/// circular include.
struct JoltCharacterConfig {
    float radius = 0.35f;              // capsule radius
    float half_height = 0.55f;         // capsule cylinder half-height (total height = 2*(half+radius))
    float crouch_half_height = 0.30f;  // cylinder half-height while crouched
    float max_slope_deg = 50.0f;       // steeper contacts are walls, not ground
    float max_speed = 6.0f;            // horizontal cruise speed
    float acceleration = 40.0f;        // horizontal velocity gain per second
    float air_control = 0.35f;         // acceleration multiplier while airborne
    float jump_speed = 7.0f;           // upward speed applied when jumping while grounded
    float step_offset = 0.4f;          // max obstacle height climbed while walking (WalkStairs)
    float mass = 80.0f;                // for pushing dynamic bodies (character vs body)
    float gravity_scale = 1.0f;        // multiplier on the world gravity
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

    // --- Complex colliders (design §38; additive, Jolt-only) --------------
    //
    // The shared Shape/ShapeType vocabulary stays closed (sphere/box/plane,
    // see Shapes.hpp): these three entry points are the only way to get a
    // Jolt-native collider that the first-party solver has no opinion about.
    // They are not a separate species of body. Each one returns a NORMAL
    // JoltBody, so everything downstream keeps working unchanged — is_alive /
    // state / set_linear_velocity / remove_body / body_count, every query
    // (ray_cast, sphere_cast, overlap_sphere, overlap_box, trigger_overlaps),
    // set_continuous_collision, constraints, vehicles and ragdolls all treat a
    // capsule/hull/mesh body exactly like a sphere.
    //
    // Invalid input (bad shape parameters, a degenerate cloud, a malformed
    // index buffer) yields an invalid handle and adds no body — never a crash
    // and never a silent fallback to a different shape: a capsule that
    // quietly becomes a sphere is a wrong collider, which is worse than a
    // missing one. Inherited from add_body(): the BodyDesc's initial
    // *velocity* fields are not applied at creation — use
    // set_linear_velocity() (velocity lives on the body, not the settings).

    /// Creates a body whose collider is a capsule (Y-axis, like Jolt's own
    /// capsule): `half_height` is the cylinder half-height, `radius` the cap
    /// radius. `base` supplies type/position/orientation/velocity/mass/friction/
    /// restitution/damping/sleep — its `shape` field is ignored (the capsule
    /// parameters replace it). Returns an invalid handle for a non-positive
    /// radius, a negative half_height, an invalid world, or a failed creation.
    JoltBody add_capsule_body(const BodyDesc& base, float radius, float half_height);

    /// Creates a body whose collider is a cylinder (Y-axis, like Jolt's own
    /// cylinder): `half_height` is half the straight-walled length, `radius` the
    /// wall radius, so the collider spans local y = -half_height .. +half_height.
    /// Convex, so unlike a mesh or a height field it may be Dynamic: a barrel, a
    /// pipe, a wheel without the vehicle system. `base` supplies type/position/
    /// orientation/velocity/mass/friction/restitution/damping/sleep — its
    /// `shape` field is ignored (the cylinder parameters replace it). Returns an
    /// invalid handle for a non-positive radius, a non-positive half_height, an
    /// invalid world, or a failed creation.
    ///
    /// A zero half_height is rejected on purpose even though Jolt would accept
    /// it: the degenerate cylinder is a zero-thickness disc, a collider that
    /// touches everything and holds nothing. The degenerate capsule has a
    /// meaning (it becomes a sphere); the degenerate cylinder does not.
    JoltBody add_cylinder_body(const BodyDesc& base, float radius, float half_height);

    /// Creates a body whose collider is the convex hull of `local_points`
    /// (body-local space). Fewer than 4 points, or a degenerate/coplanar cloud
    /// (Jolt builds a zero-volume hull from those instead of failing, which is
    /// rejected here as well), yields an invalid handle instead of a crash.
    JoltBody add_convex_hull_body(const BodyDesc& base, const std::vector<Vec3>& local_points);

    /// Creates a STATIC body whose collider is a triangle mesh built from
    /// `vertices` (body-local) and `indices` (3 per triangle). Mesh colliders may
    /// not move in Jolt, so the body is forced to Static regardless of
    /// `base.type` (documented); an empty or malformed index list yields an
    /// invalid handle.
    JoltBody add_mesh_body(const BodyDesc& base, const std::vector<Vec3>& vertices,
                           const std::vector<u32>& indices);

    /// Creates a STATIC body whose collider is a height field: a regular
    /// `sample_count`-by-`sample_count` grid of `heights`, sampled row-major so
    /// that the column/row indices a terrain renderer uses address the same
    /// vertex. Vertex (x, y) sits at
    ///
    ///     offset + scale * (x, heights[y * sample_count + x], y)
    ///
    /// which is the engine's terrain convention (grid in XZ, +Y up) one-to-one:
    /// the grid's X and its row index Y map to world X and Z, the sample is the
    /// height along world Y. A height field covers exactly the gap a mesh
    /// collider leaves — the terrain renderer (`Rendering/Terrain`) builds the
    /// same grid, and without this collider it renders ground bodies cannot
    /// stand on.
    ///
    /// Height fields may not move in Jolt (`MustBeStatic`), so the body is
    /// forced to Static regardless of `base.type`, as add_mesh_body does.
    /// `sample_count` must be at least 4 (Jolt requires `sample_count /
    /// block_size >= 2` with its default block size of 2), `heights` must hold
    /// exactly `sample_count` squared finite values, and the X and Z scale
    /// components must be non-zero — otherwise the grid collapses to a line and
    /// there is no field to collide with. `scale.y` may be zero: that is a flat
    /// field, which is a legal (if uninteresting) plane. Any violation yields an
    /// invalid handle and adds no body.
    JoltBody add_heightfield_body(const BodyDesc& base, const std::vector<float>& heights,
                                  u32 sample_count, Vec3 offset, Vec3 scale);

    /// The shape a compound part may take. Deliberately its own small
    /// vocabulary rather than a reuse of the first-party `Shape`: `Shapes.hpp`
    /// keeps that one closed at sphere/box/plane on purpose (the narrowphase
    /// switches on it and the broadphase reads its extents), while a compound
    /// part should also be free to be a cylinder or a capsule — the same complex
    /// colliders the entry points above build. Widening `Shape` to carry them
    /// would drag shapes the solver cannot solve through every part of the
    /// engine; a part never reaches the solver, it only reaches Jolt.
    struct CompoundShape {
        enum class Type : u8 {
            Sphere,
            Box,
            Cylinder, // Y axis, per add_cylinder_body
            Capsule,  // Y axis, per add_capsule_body
        };
        Type type = Type::Box;
        float radius = 0.5f;                 // sphere, cylinder, capsule
        float half_height = 0.0f;            // cylinder: half the straight length;
                                             // capsule: half the cylinder length
        Vec3 half_extents{0.5f, 0.5f, 0.5f}; // box

        static CompoundShape make_sphere(float radius);
        static CompoundShape make_box(const Vec3& half_extents);
        static CompoundShape make_cylinder(float radius, float half_height);
        static CompoundShape make_capsule(float radius, float half_height);
    };

    /// One part of a compound collider: a `CompoundShape` placed at a local
    /// `position` and `orientation` within the body. Parts may overlap; Jolt
    /// only asks that the union be a sensible collider, and it is what the
    /// caller makes it.
    struct CompoundPart {
        CompoundShape shape;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Quat orientation = Quat::identity();
    };

    /// Creates a body whose collider is the union of `parts`, each placed in
    /// body-local space. This is the shape the vocabulary above cannot express:
    /// a box hull CANNOT be concave (its builder convexifies whatever points it
    /// is given), and a static mesh may not move. A compound of boxes is both —
    /// an L-bracket, a table with legs, a stair step all simulate as ONE dynamic
    /// body instead of a constraint-welded cluster that costs solver iterations
    /// and never quite stops jittering.
    ///
    /// `base` supplies type/position/orientation/friction/restitution/damping/
    /// sleep; its `shape` field is ignored (the parts replace it). The body may
    /// be Dynamic: unlike the mesh and height field, no part forces it static, so
    /// a compound of spheres, boxes, cylinders and capsules is freely movable.
    /// `state().position` round-trips the part-space origin — place a part at
    /// (0, 1, 0) and it sits one metre above whatever `state()` reports, same
    /// convention as every other collider here.
    ///
    /// Fewer than 2 parts, a non-positive sphere/cylinder/capsule radius or box
    /// half-extent, a non-positive cylinder half_height (a zero-height cylinder
    /// is a zero-thickness disc, a collider that touches everything and holds
    /// nothing — the degenerate capsule is different, a zero half_height turns
    /// it into a sphere, which is legal), or a failed creation yields an invalid
    /// handle and adds no body. A part's degenerate geometry is REJECTED rather
    /// than clamped: unlike the first-party `Shape` (whose factories clamp so the
    /// solver may assume a positive radius) a part is consumed by Jolt alone, and
    /// Jolt's cylinder and capsule builders do not normalise a negative radius.
    JoltBody add_compound_body(const BodyDesc& base, const std::vector<CompoundPart>& parts);

    // --- Scene queries, sensors (triggers) and continuous collision -------
    //
    // Design rules, shared by every entry point below:
    //   * `direction` is normalized internally and must NOT be normalized by
    //     the caller; a zero-length direction is a miss, not a division by
    //     zero. A non-positive `max_distance` is a miss too.
    //   * SENSORS ARE NEVER REPORTED. A trigger volume is invisible to every
    //     query here — otherwise a ray fired from inside a trigger would hit
    //     the trigger instead of the geometry behind it. Use
    //     trigger_overlaps() to ask a trigger what is inside it.
    //   * Invalid handles, dead handles and an invalid world return a miss /
    //     an empty vector — queries never crash.
    //   * Queries see dynamic AND static bodies (a ray must hit the floor),
    //     deduped per body; the order is unspecified.

    /// Result of a ray or shape cast. `hit == false` means nothing was hit.
    struct QueryHit {
        bool hit = false;
        JoltBody body;            // invalid when hit == false
        Vec3 position{0, 0, 0};   // world-space contact point
        Vec3 normal{0, 0, 0};     // world-space surface normal (out of the hit body)
        float distance = 0.0f;    // distance travelled along the cast direction
        float fraction = 0.0f;    // distance / max_distance in [0,1]
    };

    /// Casts a ray. Sensors are never reported (documented above). `ignore`
    /// skips one body; an invalid or already-removed handle is a no-op, so the
    /// ignore parameter is safe to pass unconditionally. Returns the CLOSEST
    /// hit. `distance` is `fraction * max_distance`.
    QueryHit ray_cast(Vec3 origin, Vec3 direction, float max_distance,
                      JoltBody ignore = JoltBody{}) const;

    /// Sweeps a sphere of `radius` along `direction`. Sensors never reported.
    /// `distance`/`fraction` describe the swept sphere's CENTRE travel, while
    /// `position` is a world-space contact point on the swept sphere's own
    /// surface (so it sits ~`radius` ahead of the centre at the hit). A
    /// non-positive `radius` is a miss.
    QueryHit sphere_cast(Vec3 origin, float radius, Vec3 direction, float max_distance,
                         JoltBody ignore = JoltBody{}) const;

    /// All non-sensor bodies whose shape overlaps the given primitive (order
    /// unspecified). May include static bodies, so a query that reaches down
    /// to the ground plane reports it. A non-positive radius / any zero
    /// half-extent yields an empty vector.
    std::vector<JoltBody> overlap_sphere(Vec3 center, float radius) const;
    std::vector<JoltBody> overlap_box(Vec3 center, Vec3 half_extents) const;

    /// Creates a sensor (trigger) body: it detects overlaps but never blocks
    /// motion (sensors generate no contacts, so dynamics pass straight
    /// through). Same bookkeeping as add_body: the returned handle is a normal
    /// JoltBody, the body counts in body_count() and is removable with
    /// remove_body(). `desc.type` is honored — prefer BodyType::Static for a
    /// fixed volume (a Dynamic trigger never collides, so it free-falls);
    /// a Kinematic trigger is the script-driven moving volume.
    JoltBody add_trigger(const BodyDesc& desc);

    /// Bodies currently overlapping a trigger created by add_trigger
    /// (non-sensor bodies only, static scenery included, the queried body
    /// itself always excluded). Any live body works as the query shape, but
    /// only a sensor actually behaves as a trigger. Invalid/dead handles
    /// return an empty vector.
    std::vector<JoltBody> trigger_overlaps(JoltBody trigger) const;

    /// Enables/disables continuous collision detection (linear cast) for a
    /// body. CCD stops a fast, small body at the first contact instead of
    /// tunnelling through thin geometry, at the cost of one shape cast per
    /// step — enable it for fast-moving bodies only. Dead handles are a no-op.
    void set_continuous_collision(JoltBody handle, bool enabled);

    /// The pose and velocity of one body. `position` is the body's shape-local
    /// origin in world space — the same point BodyDesc.position placed, so
    /// state() of a freshly added body returns what was asked for regardless of
    /// shape, and writing it into a scene Transform draws the mesh where the
    /// shape is. It is deliberately not Jolt's centre-of-mass position, which
    /// differs from this by the shape's local COM for any hull that is not
    /// centred on its own origin (every fracture shard). Dead or invalid handles
    /// return a zeroed state.
    JoltBodyState state(JoltBody handle) const;
    void set_linear_velocity(JoltBody handle, const Vec3& velocity);

    /// Sets the angular velocity directly (radians/second, world space). This
    /// exists for the same reason as the linear setter: a BodyDesc's velocity
    /// fields are documented as NOT applied at creation, so anything that
    /// spawns a body already moving must set both afterwards. Debris is the
    /// case that needs it — a shard's spin comes from the blast impulse landing
    /// off-centre, and a shard whose angular velocity is dropped lands flat.
    /// Dead or invalid handles are a no-op.
    void set_angular_velocity(JoltBody handle, const Vec3& velocity);

    /// Applies an impulse at the body's centre of mass: the linear velocity
    /// changes by impulse / mass and the body does not spin. Units are N*s, so
    /// `impulse = mass * desired_velocity` needs no frame rate — unlike a force,
    /// an impulse is felt without a step. A sleeping body wakes up; a static or
    /// kinematic body (and a sensor) is silently unaffected, because a push on a
    /// body that cannot move is a no-op rather than an error. Dead or invalid
    /// handles are a no-op too.
    void apply_impulse(JoltBody handle, const Vec3& impulse);

    /// Applies an impulse at a world-space `point`. The linear part is exactly
    /// apply_impulse's; the off-centre arm `point - centre_of_mass` additionally
    /// spins the body, so the same impulse at a wheel's rim makes it roll and at
    /// its hub does not. This is the call a blast, a weapon's recoil or a jump
    /// pad wants, and the one that set_angular_velocity forces the caller to
    /// solve by hand — torque and inertia differ per shape, and the point is
    /// measured against the body's *centre of mass*, which for a hull not
    /// centred on its own origin is NOT state().position (see the state() note
    /// above). A point inside the body is legal; the arm simply points the other
    /// way and the spin reverses. No-op on static/kinematic bodies and dead
    /// handles.
    void apply_impulse_at_point(JoltBody handle, const Vec3& impulse, const Vec3& world_point);

    /// Adds a force to the body's accumulator, which the next step() integrates
    /// and then clears. Two calls before one step apply twice the force; a call
    /// after the body's last step is never felt, so a body about to be removed
    /// wants an impulse. The body must stay alive until the step. Wakes a
    /// sleeping body, no-ops on static/kinematic bodies and dead handles.
    void add_force(JoltBody handle, const Vec3& force);
    /// As add_force, but applied off-centre — force at a point also torques the
    /// body about `point - centre_of_mass`.
    void add_force_at_point(JoltBody handle, const Vec3& force, const Vec3& world_point);
    /// Adds a pure torque (N*m, world space) to the accumulator — for a spin with
    /// no net push, e.g. a motor or a drag-free turn. Same accumulator lifetime
    /// and the same no-op rules as add_force.
    void add_torque(JoltBody handle, const Vec3& torque);

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
    void vehicle_drive(VehicleHandle handle, float forward, float steer, float brake,
                       float handbrake = 0.0f);
    /// Per-wheel state (position, contact, suspension, spin, steer), one entry
    /// per wheel in creation order: [FL, FR, RL, RR] for the default config.
    /// An invalid handle yields an empty vector.
    std::vector<JoltWheelState> vehicle_wheel_states(VehicleHandle handle) const;
    /// Teleports the chassis to `position` and zeroes its velocity, keeping the
    /// vehicle's constraint and wheels intact — the respawn/flip-recovery path
    /// (a rolled car resets upright without rebuilding the whole vehicle).
    /// A dead/invalid handle is a no-op.
    void vehicle_reset(VehicleHandle handle, Vec3 position);

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

    // --- Character controller wiring (design §39; same single-TU rule as
    // --- vehicles and ragdolls: the JPH::CharacterVirtual object is born and
    // --- driven in JoltWorld.cpp).
    //
    // The character is a KINEMATIC-STYLE volume (Jolt's CharacterVirtual with
    // no inner rigid body), not a rigid body: it does NOT appear in
    // body_count(), the world step never integrates it (character_move()
    // moves it, once per fixed tick), and only its own contacts can push it
    // around. Consequences worth knowing before wiring it into a game or a
    // netcode layer:
    //   * The tick is SELF-CONTAINED, unlike the first-party
    //     CharacterController's move/step/post_step protocol: CharacterVirtual
    //     integrates itself inside that single call, so there is no separate
    //     step() to pair it with. The game still calls world.step() for the
    //     rest of the world (and for the bodies the character pushes).
    //   * It is DETERMINISTIC given identical inputs and dt — the same
    //     sequence of character_move() calls from the same state produces the
    //     same trajectory, which is what a network-prediction hook needs
    //     (client and server replay the same inputs; the server's authority
    //     is the same function of the same bytes).
    //   * Gravity comes from world.settings().gravity, scaled by the config's
    //     gravity_scale. A character standing on ground is rebuilt from the
    //     ground's velocity each tick instead of accumulating gravity (an
    //     accumulated falling speed would never be cancelled: CharacterVirtual
    //     stores the velocity it is given, not the one the contacts solved).

    /// Opaque character handle. 0 = invalid.
    struct CharacterHandle {
        u32 id = 0; // index into m_impl->characters (0 = none)
        bool valid() const { return id != 0; }
    };
    CharacterHandle character_create(const JoltCharacterConfig& config, Vec3 spawn);
    void character_destroy(CharacterHandle handle);
    /// One gameplay tick: accelerates the horizontal velocity toward
    /// wish_dir * max_speed (wish_dir length scales the target speed; pass a zero
    /// vector to stop), applies gravity (scaled) on the vertical axis, jumps when
    /// `jump` is set and the character is grounded, walks stairs up to
    /// `step_offset`, sticks to the floor, then moves the character by itself for
    /// `dt`. Call once per fixed step (dt <= 0 is a no-op).
    ///
    /// A supported character is CARRIED by its ground (the ground velocity is
    /// added to the desired velocity, and the horizontal acceleration acts on
    /// the velocity *relative* to the ground so it is never compounded) — that
    /// is what makes moving platforms work. `character_position()` is the
    /// character's FEET; `character_velocity()` is the velocity that was last
    /// handed to it (CharacterVirtual stores the velocity it is given, not a
    /// contact-solved one).
    void character_move(CharacterHandle handle, Vec3 wish_dir, bool jump, float dt);
    /// Enables/disables the climb hook: while enabled, a non-zero `wish_dir.y`
    /// (interpreted through the character's up axis) drives vertical movement —
    /// the game turns it on while the character is on a ladder and off otherwise.
    void character_set_climbing(CharacterHandle handle, bool enabled);
    /// Crouch state: changes the capsule height (crouch_half_height while crouched).
    void character_set_crouch(CharacterHandle handle, bool crouched);
    bool character_is_crouched(CharacterHandle handle) const;
    bool character_is_grounded(CharacterHandle handle) const;
    Vec3 character_position(CharacterHandle handle) const;
    Vec3 character_velocity(CharacterHandle handle) const;

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

    /// Copies a two-body constraint onto a DIFFERENT pair of bodies. The clone
    /// keeps the original's type, limits, axis and anchor configuration, and
    /// is created between `new_a` and `new_b` in their CURRENT relative pose.
    ///
    /// This is the "instance the same joint many times" primitive: build one
    /// hinge/distance/sixdof on a template pair, then stamp copies of it
    /// between spawned bodies (a chain, a ragdoll limb pair, a row of doors)
    /// without re-specifying the geometry. Constraints created this way are
    /// ordinary constraints: remove_constraint() frees them, and their
    /// world-space anchors are recomputed relative to the new bodies.
    ///
    /// Returns invalid for a source constraint that is gone, a Vehicle or
    /// other non-two-body constraint (which carry their own dedicated wiring),
    /// or bodies that are not alive. The source constraint itself is left
    /// untouched — cloning never destroys the template.
    JoltConstraint clone_constraint(JoltConstraint source, JoltBody new_a, JoltBody new_b);

    void remove_constraint(JoltConstraint handle);

private:
    /// Single creation path shared by add_body and add_trigger: identical
    /// settings (shape mapping, layers, mass override, activation) with the
    /// sensor flag as the only difference, so triggers cannot drift away from
    /// the bookkeeping remove_body/is_alive/body_count already rely on.
    JoltBody add_body_internal(const BodyDesc& desc, bool sensor);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    PhysicsSettings m_settings;
};

} // namespace nf::physics
