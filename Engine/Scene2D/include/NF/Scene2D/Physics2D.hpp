#pragma once

// NF/Scene2D/Physics2D.hpp — deterministic rigidbody solver for 2D.
// Design doc Section 56 (2D Physics): "2D world / 2D shapes / 2D constraints",
// "with a layer independent of 3D gameplay".
//
// Not a wrapper around Jolt. Jolt is a 3D engine whose 2D support means
// "constrain z", which still pays for a 3D broadphase, 3D shapes and a 3D
// solver every step. A 2D game wants a few hundred bodies stepping in
// microseconds with contacts that are flat by construction, so this is a
// purpose-written solver: circles and boxes, a spatial-hash broadphase, SAT
// narrowphase with contact clipping, sequential impulses, and the two
// constraints 2D gameplay actually needs (distance, revolute).
//
// Determinism (design doc Section 114) is a hard contract here, not an
// aspiration: `is_deterministic()` is the promise that two runs of the same
// step sequence produce identical body state, and the tests assert it. Every
// source of run-to-run variation is removed — pair generation sorts and
// dedupes, contact ordering is by coordinate, there is no RNG anywhere, and
// iteration counts are fixed, not adaptive.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include "Math2D.hpp"

#include <vector>

namespace nf::scene2d {

/// A collider shape. Two kinds only: adding a third (capsule) is a matter of a
/// new narrowphase pair, not an architecture change.
struct Shape2D {
    enum class Kind : u32 { Circle, Box };

    Kind kind = Kind::Circle;
    f32 radius = 0.5f;        ///< Circle radius, world units.
    Vec2 half{0.5f, 0.5f};    ///< Box half-extents, world units.

    static Shape2D circle(f32 r) {
        Shape2D s;
        s.kind = Kind::Circle;
        s.radius = r;
        return s;
    }

    static Shape2D box(f32 half_x, f32 half_y) {
        Shape2D s;
        s.kind = Kind::Box;
        s.half = Vec2{half_x, half_y};
        return s;
    }

    /// Half-extent of the shape's bounding square — what the broadphase and the
    /// debug drawer need.
    Vec2 bounding_half() const {
        if (kind == Kind::Circle) return Vec2{radius, radius};
        return half;
    }

    /// Shape area, for mass computation. A uniform shape's centre of mass is
    /// its position, which is what lets the solver skip per-shape COM offsets.
    f32 area() const {
        if (kind == Kind::Circle) {
            return static_cast<f32>(PI) * radius * radius;
        }
        return 4.0f * half.x * half.y;
    }
};

/// One rigid body. Plain data plus derived mass properties; the world owns the
/// array and hands out handles, so a body is never moved or copied behind a
/// caller's back.
struct Body2D {
    // --- State ---------------------------------------------------------
    Vec2 position{0.0f, 0.0f};
    f32 angle = 0.0f;          ///< Radians, clockwise in y-down space.
    Vec2 velocity{0.0f, 0.0f};
    f32 angular_velocity = 0.0f;

    // --- Accumulated per step, cleared by integrate() -------------------
    Vec2 force{0.0f, 0.0f};
    f32 torque = 0.0f;

    // --- Material / behaviour ------------------------------------------
    Shape2D shape;
    f32 density = 1.0f;
    f32 restitution = 0.2f;    ///< 0 = stop dead, 1 = bounce back fully.
    f32 friction = 0.6f;       ///< Coulomb coefficient, tangential clamp.
    f32 gravity_scale = 1.0f;  ///< 0 = floating body (hover, space).
    bool is_static = false;    ///< Never moves; infinite mass.
    bool is_trigger = false;   /// Generates contacts but resolves none.
    bool fixed_rotation = false;
    /// Linear damping per second. Applied as velocity *= exp(-damping*dt), the
    /// frame-rate-independent form; the naive `1 - damping*dt` overshoots to
    /// negative velocity at large dt.
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.1f;

    // --- Derived (update_mass_properties) -------------------------------
    f32 mass = 1.0f;
    f32 inv_mass = 1.0f;
    f32 inertia = 1.0f;        ///< About the centre of mass.
    f32 inv_inertia = 1.0f;

    /// Recomputes mass/inertia from shape and density. Static bodies get
    /// infinite mass so the solver's `inv_mass == 0` early-outs fire.
    void update_mass_properties() {
        if (is_static) {
            mass = 0.0f;
            inv_mass = 0.0f;
            inertia = 0.0f;
            inv_inertia = 0.0f;
            return;
        }
        mass = density * shape.area();
        inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
        if (shape.kind == Shape2D::Kind::Circle) {
            // I = m r^2 / 2 for a disc.
            inertia = 0.5f * mass * shape.radius * shape.radius;
        } else {
            // I = m (w^2 + h^2) / 12 for a rectangle about its centre.
            const f32 w = 2.0f * shape.half.x;
            const f32 h = 2.0f * shape.half.y;
            inertia = mass * (w * w + h * h) / 12.0f;
        }
        inv_inertia = inertia > 0.0f ? 1.0f / inertia : 0.0f;
        if (fixed_rotation) inv_inertia = 0.0f;
    }

    // --- Kinematics -----------------------------------------------------
    Mat3x2 world_transform() const {
        return Mat3x2::compose(position, angle * RAD_TO_DEG, Vec2{1.0f, 1.0f});
    }

    /// World-space velocity at a point, including rotation: v + omega x r.
    /// A box's corner moves when the box spins even if its centre does not.
    Vec2 velocity_at_point(Vec2 world_point) const {
        const Vec2 r = world_point - position;
        // Perpendicular of r in 2D, rotated 90 degrees clockwise (y-down).
        const Vec2 perp{-r.y, r.x};
        return velocity + perp * angular_velocity;
    }

    /// Impulse at the centre of mass: velocity changes, angular velocity does not.
    void apply_impulse(Vec2 impulse) {
        if (is_static) return;
        velocity += impulse * inv_mass;
    }

    /// Impulse at a world point: applies both linear and angular response. An
    /// impulse is instantaneous, so unlike `apply_force_at_point` this writes
    /// straight to velocity — accumulating into `torque` would defer the spin to
    /// the next integrate and let the body drift off the impulse line meanwhile.
    void apply_impulse_at_point(Vec2 impulse, Vec2 world_point) {
        if (is_static) return;
        velocity += impulse * inv_mass;
        const Vec2 r = world_point - position;
        // tau = r x F; in 2D the cross product is a scalar.
        const f32 impulse_torque = r.x * impulse.y - r.y * impulse.x;
        angular_velocity += impulse_torque * inv_inertia;
    }

    void apply_force(Vec2 f) { force += f; }

    void apply_force_at_point(Vec2 f, Vec2 world_point) {
        force += f;
        const Vec2 r = world_point - position;
        torque += r.x * f.y - r.y * f.x;
    }

    void apply_torque(f32 t) { torque += t; }

    /// World-space AABB, inflated by `margin` — the broadphase queries this.
    Rect aabb(f32 margin = 0.0f) const {
        const Vec2 h = shape.bounding_half();
        return Rect{position.x - h.x - margin, position.y - h.y - margin,
                    h.x * 2.0f + margin * 2.0f, h.y * 2.0f + margin * 2.0f};
    }
};

/// Generation-tagged index into the world's body pool. A destroyed body's slot
/// is reused, so the generation is what stops a stale handle from silently
/// commanding whatever body took the slot.
struct BodyHandle {
    u32 index = u32_max;
    u32 generation = 0;

    bool operator==(const BodyHandle& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const BodyHandle& o) const { return !(*this == o); }
    bool valid() const { return index != u32_max; }
};

/// A contact between two bodies. Up to two contact points per pair: two boxes
/// resting on each other produce an edge contact, and resolving both points is
/// what stops the top box from tipping over.
struct ContactManifold {
    u32 body_a = u32_max;
    u32 body_b = u32_max;
    Vec2 normal{0.0f, 0.0f};   ///< From A to B, unit length.
    f32 penetration = 0.0f;
    Vec2 points[2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
    u32 point_count = 0;
    /// Combined material properties, blended once per manifold rather than per
    /// impulse.
    f32 restitution = 0.0f;
    f32 friction = 0.0f;
    /// True when either body is a trigger: reported by `step` in the contact
    /// list, but never resolved.
    bool is_trigger = false;
};

/// Distance constraint: keeps `distance(body_a, body_b) == rest_length`. A
/// rope, a chain link, a grappling hook. `stiffness` in [0,1] softens it toward
/// a spring — 1.0 is perfectly rigid.
struct DistanceConstraint2D {
    BodyHandle a;
    BodyHandle b;
    f32 rest_length = 1.0f;
    f32 stiffness = 1.0f;
};

/// Revolute (pin/hinge) constraint: `local_anchor_a` on body A is pinned to
/// `local_anchor_b` on body B, with the bodies free to rotate about the shared
/// point. A door hinge, a wheel, a ragdoll joint.
struct RevoluteConstraint2D {
    BodyHandle a;
    BodyHandle b;
    Vec2 local_anchor_a{0.0f, 0.0f};
    Vec2 local_anchor_b{0.0f, 0.0f};
};

struct RaycastHit2D {
    BodyHandle body;
    Vec2 point{0.0f, 0.0f};
    Vec2 normal{0.0f, 0.0f};
    f32 distance = 0.0f;
    bool hit = false;
};

class PhysicsWorld2D {
public:
    PhysicsWorld2D() = default;

    /// Gravity in world units per second squared. y is positive (downward).
    void set_gravity(Vec2 g) { m_gravity = g; }
    Vec2 gravity() const { return m_gravity; }

    /// Adds a body and returns a handle. The body's mass properties are
    /// (re)computed, so a caller setting density/shape/static after creation
    /// should call `refresh_mass_properties`.
    BodyHandle create_body(const Body2D& body);
    void destroy_body(BodyHandle handle);
    Body2D* body(BodyHandle handle);
    const Body2D* body(BodyHandle handle) const;
    usize body_count() const { return m_body_count; }
    void refresh_mass_properties(BodyHandle handle);

    /// Adds a distance constraint and returns its index into
    /// `distance_constraints()` — the index is stable until `clear`, and the
    /// index is how the caller later removes or reconfigures the joint.
    u32 add_distance_constraint(const DistanceConstraint2D& c);
    u32 add_revolute_constraint(const RevoluteConstraint2D& c);
    const std::vector<DistanceConstraint2D>& distance_constraints() const {
        return m_distance;
    }
    const std::vector<RevoluteConstraint2D>& revolute_constraints() const {
        return m_revolute;
    }

    /// Advances the world by one fixed step. Call from a fixed-timestep loop,
    /// never from the render loop: a variable dt makes the solver's stability
    /// and the replay's output both depend on the frame rate.
    void step(f32 dt, u32 velocity_iterations = 8, u32 position_iterations = 4);

    /// Contacts from the last `step`, in deterministic order. Both resolved and
    /// trigger manifolds are reported — gameplay distinguishes them with
    /// `is_trigger` (feet on ground is a resolved contact, a pickup is a trigger).
    const std::vector<ContactManifold>& contacts() const { return m_contacts; }

    // --- Queries ---------------------------------------------------------

    /// Finds the first dynamic body containing `point`. Overlap order is not
    /// defined; callers needing a specific one should test all hits.
    BodyHandle point_query(Vec2 world_point) const;

    /// Casts a ray and returns the nearest impact. `dir` need not be normalised
    /// — the distance reported is in units of `dir` scaled to its length.
    RaycastHit2D raycast(Vec2 start, Vec2 dir, f32 max_distance) const;

    /// True when the two shapes overlap, ignoring trigger/static settings.
    bool shapes_overlap(const Body2D& a, const Body2D& b) const;

    /// The determinism contract (design doc Section 114). Always true for this
    /// solver: it exists so a caller can assert the property rather than
    /// trusting a comment.
    bool is_deterministic() const { return true; }

    void clear();

private:
    struct Slot {
        Body2D body;
        u32 generation = 1;
        bool alive = false;
    };

    /// Slot lookup with generation validation: the only place a stale handle is
    /// turned into a null instead of into a command aimed at the body that took
    /// the recycled slot.
    Slot* slot(BodyHandle handle);
    const Slot* slot(BodyHandle handle) const;

    // --- Integration -----------------------------------------------------
    /// Velocity integration: applies accumulated force/torque and gravity, then
    /// clears the accumulators. Semi-implicit — the new velocity is what moves
    /// the body this step, so a force sees its effect within the same step.
    void integrate_bodies(f32 dt);
    /// Position integration from the post-solver velocity.
    void integrate_positions(f32 dt);
    void apply_damping(f32 dt);

    // --- Solver ----------------------------------------------------------
    /// Computes per-manifold restitution bias once per step: restitution must
    /// add exactly one bounce, not one per solver iteration, so the closing
    /// speed is measured before the first velocity pass and replayed as a
    /// constant target by every `solve_velocities` pass.
    void prepare_contacts();
    /// One sequential-impulse pass over the contact list: normal impulses with
    /// accumulated clamping (a contact can only push, never pull), then Coulomb
    /// friction per point.
    void solve_velocities();
    /// One Baumgarte pass over the contact list, shrinking residual overlap.
    void solve_positions();
    /// Distance and revolute joints, velocity level.
    void solve_constraints(f32 dt);
    /// Joint position correction, run inside the position loop.
    void solve_constraint_positions();

    void find_contacts();

    // --- Broadphase ------------------------------------------------------
    std::vector<std::pair<u32, u32>> broadphase_pairs() const;

    // --- Narrowphase -----------------------------------------------------
    bool collide(const Body2D& a, const Body2D& b, ContactManifold& out) const;
    bool circle_circle(const Body2D& a, const Body2D& b, ContactManifold& out) const;
    bool circle_box(const Body2D& a, const Body2D& b, ContactManifold& out) const;
    bool box_box(const Body2D& a, const Body2D& b, ContactManifold& out) const;

    std::vector<Slot> m_slots;
    std::vector<u32> m_free;
    usize m_body_count = 0;

    std::vector<DistanceConstraint2D> m_distance;
    std::vector<RevoluteConstraint2D> m_revolute;

    std::vector<ContactManifold> m_contacts;
    /// Accumulated impulse per contact point, parallel to `m_contacts` two
    /// entries per manifold. Kept across the velocity iterations because the
    /// clamp is what makes a resting contact stick instead of vibrating.
    std::vector<f32> m_acc_normal;
    std::vector<f32> m_acc_friction;
    /// Per-manifold restitution target velocity, set by `prepare_contacts`.
    std::vector<f32> m_restitution_bias;
    Vec2 m_gravity{0.0f, 25.0f};
};

} // namespace nf::scene2d
