#include <NF/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace nf::physics {

namespace {

/// Two unit vectors orthogonal to `n`, for the friction directions.
void build_tangent_basis(const Vec3& n, Vec3& t0, Vec3& t1) {
    // Cross with whichever axis is least aligned with n, so the result is never
    // near-degenerate.
    const Vec3 helper = (std::fabs(n.x) < 0.9f) ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    t0 = n.cross(helper).normalized();
    t1 = n.cross(t0).normalized();
}

void clamp_magnitude(Vec3& v, f32 max_length) {
    const f32 len2 = v.length_sq();
    if (len2 > max_length * max_length && len2 > 0.0f) {
        v = v * (max_length / std::sqrt(len2));
    }
}

/// The previous manifold's point nearest to `position`, within `max_distance`.
///
/// Matched by POSITION rather than by feature id. A box-box face contact is
/// produced by clipping a polygon, and the clip can reorder or change the vertex
/// count between frames, so a vertex index is not a stable feature: matching on
/// it inherits the impulse accumulated for a different corner, which injects
/// energy and leaves a stack jittering instead of settling.
const ContactPoint* find_previous_point(const Manifold& prev, const Vec3& position,
                                        f32 max_distance) {
    const ContactPoint* best = nullptr;
    f32 best_distance_sq = max_distance * max_distance;
    for (u32 i = 0; i < prev.point_count; ++i) {
        const f32 d2 = (prev.points[i].position - position).length_sq();
        if (d2 < best_distance_sq) {
            best_distance_sq = d2;
            best = &prev.points[i];
        }
    }
    return best;
}

/// How far a contact point may move between frames and still be considered the
/// same contact. Generous enough for a resting stack, tight enough that two
/// distinct corners are never confused.
constexpr f32 kContactMatchDistance = 0.05f;

} // namespace

PhysicsWorld::PhysicsWorld(const PhysicsSettings& settings) : m_settings(settings), m_grid(2.0f) {}

// --- Bodies -----------------------------------------------------------------

BodyHandle PhysicsWorld::add_body(const BodyDesc& desc) {
    RigidBody body;
    body.type = desc.type;
    body.shape = desc.shape;
    body.position = desc.position;
    body.orientation = desc.orientation.normalized();
    body.linear_velocity = desc.linear_velocity;
    body.angular_velocity = desc.angular_velocity;
    body.friction = std::clamp(desc.friction, 0.0f, 10.0f);
    body.restitution = std::clamp(desc.restitution, 0.0f, 1.0f);
    body.linear_damping = std::clamp(desc.linear_damping, 0.0f, 1.0f);
    body.angular_damping = std::clamp(desc.angular_damping, 0.0f, 1.0f);
    body.allow_sleep = desc.allow_sleep;
    body.alive = true;

    // Static and kinematic bodies are immovable by definition: zero inverse mass
    // and zero inverse inertia is what tells the solver to leave them alone,
    // rather than a special case at every use site.
    if (desc.type == BodyType::Dynamic) {
        const f32 mass = (desc.mass > 1e-4f) ? desc.mass : 1e-4f;
        body.inv_mass = 1.0f / mass;
        const Vec3 inertia = desc.shape.inertia_diagonal(mass);
        body.inv_inertia = Vec3{
            inertia.x > 1e-8f ? 1.0f / inertia.x : 0.0f,
            inertia.y > 1e-8f ? 1.0f / inertia.y : 0.0f,
            inertia.z > 1e-8f ? 1.0f / inertia.z : 0.0f,
        };
    } else {
        body.inv_mass = 0.0f;
        body.inv_inertia = Vec3::zero;
    }

    u32 index = 0;
    if (!m_free_slots.empty()) {
        index = m_free_slots.back();
        m_free_slots.pop_back();
        body.generation = m_bodies[index].generation + 1; // bump on reuse
        m_bodies[index] = body;
    } else {
        index = static_cast<u32>(m_bodies.size());
        body.generation = 1;
        m_bodies.push_back(body);
    }
    return BodyHandle{index, body.generation};
}

void PhysicsWorld::remove_body(BodyHandle handle) {
    if (!is_alive(handle)) {
        return;
    }
    m_bodies[handle.index].alive = false;
    m_bodies[handle.index].generation = handle.generation + 1; // invalidate stale handles
    m_free_slots.push_back(handle.index);
}

bool PhysicsWorld::is_alive(BodyHandle handle) const {
    return handle.valid() && handle.index < m_bodies.size() && m_bodies[handle.index].alive &&
           m_bodies[handle.index].generation == handle.generation;
}

BodyState PhysicsWorld::state(BodyHandle handle) const {
    if (!is_alive(handle)) {
        return BodyState{};
    }
    const RigidBody& b = m_bodies[handle.index];
    return BodyState{b.position, b.orientation, b.linear_velocity, b.angular_velocity, b.asleep};
}

void PhysicsWorld::set_state(BodyHandle handle, const BodyState& state) {
    if (!is_alive(handle)) {
        return;
    }
    RigidBody& b = m_bodies[handle.index];
    b.position = state.position;
    b.orientation = state.orientation.normalized();
    b.linear_velocity = state.linear_velocity;
    b.angular_velocity = state.angular_velocity;
    // An explicit state change is a wake-up: something moved it on purpose.
    b.asleep = state.asleep;
    b.sleep_timer = 0.0f;
}

void PhysicsWorld::set_velocity(BodyHandle handle, const Vec3& linear, const Vec3& angular) {
    if (!is_alive(handle)) {
        return;
    }
    RigidBody& b = m_bodies[handle.index];
    b.linear_velocity = linear;
    b.angular_velocity = angular;
    b.asleep = false;
    b.sleep_timer = 0.0f;
}

void PhysicsWorld::apply_impulse(BodyHandle handle, const Vec3& impulse, const Vec3& world_offset) {
    if (!is_alive(handle)) {
        return;
    }
    RigidBody& b = m_bodies[handle.index];
    if (b.inv_mass <= 0.0f) {
        return; // static or kinematic: an impulse does nothing
    }
    b.linear_velocity += impulse * b.inv_mass;
    b.angular_velocity += world_inv_inertia_mul(b, world_offset.cross(impulse));
    b.asleep = false;
    b.sleep_timer = 0.0f;
}

Vec3 PhysicsWorld::world_inv_inertia_mul(const RigidBody& body, const Vec3& v) const {
    const Vec3 local = body.orientation.conjugate().rotate(v);
    const Vec3 scaled{
        local.x * body.inv_inertia.x,
        local.y * body.inv_inertia.y,
        local.z * body.inv_inertia.z,
    };
    return body.orientation.rotate(scaled);
}

size_t PhysicsWorld::awake_count() const {
    size_t n = 0;
    for (const RigidBody& b : m_bodies) {
        if (b.alive && !b.asleep) ++n;
    }
    return n;
}

// --- Step -------------------------------------------------------------------

void PhysicsWorld::step(f32 dt) {
    if (!(dt > 0.0f)) {
        return;
    }
    integrate_velocities(dt);
    generate_contacts();
    prepare_constraints(dt);
    warm_start();
    solve_contacts();
    solve_position_correction();
    integrate_positions(dt);
    update_sleep(dt);
}

void PhysicsWorld::integrate_velocities(f32 dt) {
    for (RigidBody& b : m_bodies) {
        if (!b.alive || b.type != BodyType::Dynamic || b.asleep) {
            continue;
        }
        b.linear_velocity += m_settings.gravity * dt;
        b.linear_velocity *= std::max(0.0f, 1.0f - b.linear_damping * dt);
        b.angular_velocity *= std::max(0.0f, 1.0f - b.angular_damping * dt);
        // Caps: a single bad contact can otherwise launch a body far enough that
        // the next step overflows and every value becomes NaN.
        clamp_magnitude(b.linear_velocity, m_settings.max_linear_velocity);
        clamp_magnitude(b.angular_velocity, m_settings.max_angular_velocity);
    }
}

void PhysicsWorld::generate_contacts() {
    m_grid.clear();
    m_aabbs.assign(m_bodies.size(), Aabb{});

    for (u32 i = 0; i < m_bodies.size(); ++i) {
        const RigidBody& b = m_bodies[i];
        if (!b.alive) {
            continue;
        }
        Vec3 mn, mx;
        b.shape.compute_aabb(b.position, b.orientation, mn, mx);
        m_aabbs[i] = Aabb{mn, mx};
        if (b.shape.is_unbounded()) {
            m_grid.add_unbounded(i);
        } else {
            m_grid.insert(i, m_aabbs[i]);
        }
    }

    m_grid.pairs(m_pairs);

    m_prev_manifolds = std::move(m_manifolds);
    m_manifolds.clear();

    for (const BroadphasePair& pair : m_pairs) {
        const RigidBody& a = m_bodies[pair.a];
        const RigidBody& b = m_bodies[pair.b];
        if (!a.alive || !b.alive) {
            continue;
        }
        // Two immovable bodies cannot resolve anything.
        if (a.inv_mass <= 0.0f && b.inv_mass <= 0.0f) {
            continue;
        }
        // Two sleeping bodies need no contacts until something wakes one.
        if (a.asleep && b.asleep) {
            continue;
        }
        // Exact AABB rejection: the broadphase's false positives are free to
        // discard here, and the narrowphase is the expensive part.
        if (!m_aabbs[pair.a].overlaps(m_aabbs[pair.b])) {
            continue;
        }

        Manifold manifold;
        manifold.body_a = pair.a;
        manifold.body_b = pair.b;
        if (collide(a.shape, a.position, a.orientation, b.shape, b.position, b.orientation,
                    manifold)) {
            m_manifolds.push_back(manifold);
        }
    }
}

void PhysicsWorld::prepare_constraints(f32 dt) {
    m_constraints.clear();
    m_constraints.reserve(m_manifolds.size());

    for (const Manifold& m : m_manifolds) {
        const RigidBody& a = m_bodies[m.body_a];
        const RigidBody& b = m_bodies[m.body_b];

        ContactConstraint c;
        c.body_a = m.body_a;
        c.body_b = m.body_b;
        c.normal = m.normal;
        build_tangent_basis(m.normal, c.tangent[0], c.tangent[1]);
        // Geometric mean: a slippery body on a grippy one is in between, which
        // is what "combined friction" is normally taken to mean.
        c.friction = std::sqrt(a.friction * b.friction);
        c.count = m.point_count;

        for (u32 i = 0; i < m.point_count; ++i) {
            const ContactPoint& p = m.points[i];
            ContactPointConstraint& pc = c.points[i];
            pc.r_a = p.position - a.position;
            pc.r_b = p.position - b.position;

            // 1 / (inv_m_a + inv_m_b + n·((I⁻¹(r×n))×r) for both bodies)
            const f32 k_normal = a.inv_mass + b.inv_mass +
                m.normal.dot(world_inv_inertia_mul(a, pc.r_a.cross(m.normal)).cross(pc.r_a)) +
                m.normal.dot(world_inv_inertia_mul(b, pc.r_b.cross(m.normal)).cross(pc.r_b));
            pc.normal_mass = (k_normal > 1e-8f) ? 1.0f / k_normal : 0.0f;

            for (int t = 0; t < 2; ++t) {
                const Vec3& tangent = c.tangent[t];
                const f32 k_t = a.inv_mass + b.inv_mass +
                    tangent.dot(world_inv_inertia_mul(a, pc.r_a.cross(tangent)).cross(pc.r_a)) +
                    tangent.dot(world_inv_inertia_mul(b, pc.r_b.cross(tangent)).cross(pc.r_b));
                pc.tangent_mass[t] = (k_t > 1e-8f) ? 1.0f / k_t : 0.0f;
            }

            // Restitution only above a threshold speed. Without that cutoff a
            // ball bounces forever on ever-smaller hops and never settles.
            const Vec3 relative = (b.linear_velocity + b.angular_velocity.cross(pc.r_b)) -
                                  (a.linear_velocity + a.angular_velocity.cross(pc.r_a));
            const f32 approach_speed = relative.dot(m.normal);
            f32 restitution_speed = 0.0f;
            if (approach_speed < -m_settings.min_restitution_speed) {
                restitution_speed = -approach_speed * std::max(a.restitution, b.restitution);
            }

            // Baumgarte: correct a fraction of the penetration through the
            // velocity solve, with a slop so resting contacts are left alone.
            const f32 penetration = std::max(0.0f, p.penetration - m_settings.penetration_slop);
            const f32 bias = (m_settings.baumgarte / dt) * penetration;

            pc.target_velocity = restitution_speed;
            pc.bias_velocity = bias;
            pc.position_impulse = 0.0f;

            // Warm start: inherit last frame's impulse for the same feature, so
            // the solver starts from where it left off instead of from zero.
            const ContactPoint* prev_point = nullptr;
            for (const Manifold& pm : m_prev_manifolds) {
                if (pm.body_a == c.body_a && pm.body_b == c.body_b) {
                    prev_point = find_previous_point(pm, p.position, kContactMatchDistance);
                    break;
                }
            }
            pc.normal_impulse = (prev_point != nullptr) ? prev_point->normal_impulse : 0.0f;
            pc.tangent_impulse[0] = (prev_point != nullptr) ? prev_point->tangent_impulse[0] : 0.0f;
            pc.tangent_impulse[1] = (prev_point != nullptr) ? prev_point->tangent_impulse[1] : 0.0f;
        }

        m_constraints.push_back(c);
    }
}

void PhysicsWorld::warm_start() {
    // Apply the inherited impulses before the iteration loop. This is the whole
    // point of warm starting: a resting stack needs an impulse of m*g*dt every
    // step just to hold still, and rediscovering it from zero each step leaves
    // the solver a few iterations short, which shows up as slow sinking.
    for (ContactConstraint& c : m_constraints) {
        RigidBody& a = m_bodies[c.body_a];
        RigidBody& b = m_bodies[c.body_b];

        for (u32 i = 0; i < c.count; ++i) {
            const ContactPointConstraint& pc = c.points[i];
            const Vec3 impulse = c.normal * pc.normal_impulse +
                                 c.tangent[0] * pc.tangent_impulse[0] +
                                 c.tangent[1] * pc.tangent_impulse[1];
            if (impulse.length_sq() <= 0.0f) {
                continue;
            }
            a.linear_velocity -= impulse * a.inv_mass;
            a.angular_velocity -= world_inv_inertia_mul(a, pc.r_a.cross(impulse));
            b.linear_velocity += impulse * b.inv_mass;
            b.angular_velocity += world_inv_inertia_mul(b, pc.r_b.cross(impulse));
        }
    }
}

void PhysicsWorld::solve_contacts() {
    for (u32 iter = 0; iter < m_settings.velocity_iterations; ++iter) {
        for (ContactConstraint& c : m_constraints) {
            RigidBody& a = m_bodies[c.body_a];
            RigidBody& b = m_bodies[c.body_b];

            for (u32 i = 0; i < c.count; ++i) {
                ContactPointConstraint& pc = c.points[i];

                auto apply = [&](const Vec3& impulse) {
                    a.linear_velocity -= impulse * a.inv_mass;
                    a.angular_velocity -= world_inv_inertia_mul(a, pc.r_a.cross(impulse));
                    b.linear_velocity += impulse * b.inv_mass;
                    b.angular_velocity += world_inv_inertia_mul(b, pc.r_b.cross(impulse));
                };
                auto relative_velocity = [&]() {
                    return (b.linear_velocity + b.angular_velocity.cross(pc.r_b)) -
                           (a.linear_velocity + a.angular_velocity.cross(pc.r_a));
                };

                // --- Normal -------------------------------------------------
                {
                    const f32 vn = relative_velocity().dot(c.normal);
                    f32 delta = -(vn - pc.target_velocity) * pc.normal_mass;
                    // Clamping the ACCUMULATED impulse at zero is what makes the
                    // contact push-only. Clamping the increment would let a
                    // contact pull, and boxes would stick to each other.
                    const f32 old = pc.normal_impulse;
                    pc.normal_impulse = std::max(0.0f, old + delta);
                    delta = pc.normal_impulse - old;
                    if (delta != 0.0f) {
                        apply(c.normal * delta);
                    }
                }

                // --- Friction -----------------------------------------------
                // Coulomb: the tangential impulse is limited by mu * normal
                // impulse, using the normal impulse accumulated so far this
                // iteration. Solving friction after the normal is what makes
                // that limit meaningful.
                for (int t = 0; t < 2; ++t) {
                    const Vec3& tangent = c.tangent[t];
                    const f32 vt = relative_velocity().dot(tangent);
                    f32 delta = -vt * pc.tangent_mass[t];
                    const f32 max_friction = c.friction * pc.normal_impulse;
                    const f32 old = pc.tangent_impulse[t];
                    pc.tangent_impulse[t] = std::clamp(old + delta, -max_friction, max_friction);
                    delta = pc.tangent_impulse[t] - old;
                    if (delta != 0.0f) {
                        apply(tangent * delta);
                    }
                }
            }
        }
    }

    // Persist the accumulated impulses so the next step can warm start from
    // them. m_constraints and m_manifolds are built 1:1 in the same order.
    for (size_t ci = 0; ci < m_constraints.size() && ci < m_manifolds.size(); ++ci) {
        const ContactConstraint& c = m_constraints[ci];
        Manifold& m = m_manifolds[ci];
        for (u32 i = 0; i < c.count && i < m.point_count; ++i) {
            m.points[i].normal_impulse = c.points[i].normal_impulse;
            m.points[i].tangent_impulse[0] = c.points[i].tangent_impulse[0];
            m.points[i].tangent_impulse[1] = c.points[i].tangent_impulse[1];
        }
    }
}

void PhysicsWorld::solve_position_correction() {
    // Pseudo-velocity pass: identical structure to the velocity solve, but the
    // accumulated impulse is separate and the result is never written back into
    // the real velocity. This is the "split impulse" formulation — the
    // penetration is corrected without the correction showing up as motion.
    for (u32 iter = 0; iter < m_settings.position_iterations; ++iter) {
        for (ContactConstraint& c : m_constraints) {
            RigidBody& a = m_bodies[c.body_a];
            RigidBody& b = m_bodies[c.body_b];

            for (u32 i = 0; i < c.count; ++i) {
                ContactPointConstraint& pc = c.points[i];
                const Vec3 relative =
                    (b.pseudo_linear_velocity + b.pseudo_angular_velocity.cross(pc.r_b)) -
                    (a.pseudo_linear_velocity + a.pseudo_angular_velocity.cross(pc.r_a));
                const f32 vn = relative.dot(c.normal);

                f32 delta = -(vn - pc.bias_velocity) * pc.normal_mass;
                const f32 old = pc.position_impulse;
                pc.position_impulse = std::max(0.0f, old + delta);
                delta = pc.position_impulse - old;
                if (delta == 0.0f) {
                    continue;
                }
                const Vec3 impulse = c.normal * delta;
                a.pseudo_linear_velocity -= impulse * a.inv_mass;
                a.pseudo_angular_velocity -= world_inv_inertia_mul(a, pc.r_a.cross(impulse));
                b.pseudo_linear_velocity += impulse * b.inv_mass;
                b.pseudo_angular_velocity += world_inv_inertia_mul(b, pc.r_b.cross(impulse));
            }
        }
    }
}

void PhysicsWorld::integrate_positions(f32 dt) {
    for (RigidBody& b : m_bodies) {
        if (!b.alive || b.type == BodyType::Static || b.asleep) {
            continue;
        }
        // Real velocity plus the position-only correction. The correction is
        // not stored back into the real velocity, which is what keeps a resting
        // body at rest instead of oscillating at the correction speed.
        b.position += (b.linear_velocity + b.pseudo_linear_velocity) * dt;
        const Vec3 angular_total = b.angular_velocity + b.pseudo_angular_velocity;
        b.pseudo_linear_velocity = Vec3::zero;
        b.pseudo_angular_velocity = Vec3::zero;

        // q' = q + 0.5 * (w as a quaternion) * q * dt, then renormalise. The
        // renormalise is not optional: the addition drifts the quaternion off
        // the unit sphere and the rotation slowly scales.
        const Quat omega(angular_total.x, angular_total.y, angular_total.z, 0.0f);
        const Quat delta = omega * b.orientation;
        b.orientation = Quat{
            b.orientation.x + delta.x * 0.5f * dt,
            b.orientation.y + delta.y * 0.5f * dt,
            b.orientation.z + delta.z * 0.5f * dt,
            b.orientation.w + delta.w * 0.5f * dt,
        }.normalized();
    }
}

void PhysicsWorld::update_sleep(f32 dt) {
    const u32 n = static_cast<u32>(m_bodies.size());
    if (n == 0) {
        return;
    }

    // Sleeping has to be decided for a connected GROUP, not per body. In a stack
    // each body would otherwise fall asleep and be woken immediately by its still
    // awake neighbour, so no stack could ever settle — which is exactly what
    // happened before this was an island decision.
    m_island_parent.resize(n);
    for (u32 i = 0; i < n; ++i) {
        m_island_parent[i] = i;
    }
    auto find_root = [&](u32 x) {
        while (m_island_parent[x] != x) {
            m_island_parent[x] = m_island_parent[m_island_parent[x]]; // path halving
            x = m_island_parent[x];
        }
        return x;
    };
    auto unite = [&](u32 a, u32 b) {
        const u32 ra = find_root(a);
        const u32 rb = find_root(b);
        if (ra != rb) {
            m_island_parent[rb] = ra;
        }
    };

    for (const Manifold& m : m_manifolds) {
        // Static bodies must NOT join islands. If the ground did, every object
        // resting on it would be in one island with every other, and a single
        // moving body would keep the whole scene awake.
        if (m_bodies[m.body_a].type == BodyType::Static ||
            m_bodies[m.body_b].type == BodyType::Static) {
            continue;
        }
        unite(m.body_a, m.body_b);
    }

    const f32 lin2 = m_settings.sleep_linear_threshold * m_settings.sleep_linear_threshold;
    const f32 ang2 = m_settings.sleep_angular_threshold * m_settings.sleep_angular_threshold;

    std::vector<bool> island_slow(n, true);
    std::vector<bool> island_has_sleeper(n, false);
    std::vector<bool> island_no_sleep(n, false);
    std::vector<f32> island_min_timer(n, std::numeric_limits<f32>::max());

    for (u32 i = 0; i < n; ++i) {
        const RigidBody& b = m_bodies[i];
        if (!b.alive || b.type == BodyType::Static) {
            continue;
        }
        const u32 r = find_root(i);
        if (!b.allow_sleep) {
            island_no_sleep[r] = true;
        }
        if (b.asleep) {
            island_has_sleeper[r] = true;
        }
        // Kinematic bodies vote too: an island containing something that moves on
        // its own must never sleep, or the mover would be simulated as frozen.
        const bool slow = b.linear_velocity.length_sq() < lin2 &&
                          b.angular_velocity.length_sq() < ang2;
        if (!slow) {
            island_slow[r] = false;
        }
        island_min_timer[r] = std::min(island_min_timer[r], b.sleep_timer);
    }

    for (u32 i = 0; i < n; ++i) {
        RigidBody& b = m_bodies[i];
        if (!b.alive || b.type != BodyType::Dynamic) {
            continue;
        }
        const u32 r = find_root(i);
        const bool can_sleep = island_slow[r] && !island_no_sleep[r];

        if (island_has_sleeper[r]) {
            // Part of the island is already asleep. If everything is slow it
            // stays asleep; if anything moved, the whole island wakes together.
            if (can_sleep) {
                b.asleep = true;
                b.linear_velocity = Vec3::zero;
                b.angular_velocity = Vec3::zero;
                b.sleep_timer = m_settings.sleep_time_required;
            } else {
                b.asleep = false;
                b.sleep_timer = 0.0f;
            }
            continue;
        }

        if (can_sleep) {
            // The island's slowest-to-convince member sets the pace, so a body
            // that just joined the island resets the whole island's timer.
            const f32 timer = island_min_timer[r] + dt;
            b.sleep_timer = timer;
            if (timer >= m_settings.sleep_time_required) {
                b.asleep = true;
                // Zeroing on sleep is what makes a settled scene bit-identical
                // across runs instead of drifting on residual motion.
                b.linear_velocity = Vec3::zero;
                b.angular_velocity = Vec3::zero;
            }
        } else {
            b.sleep_timer = 0.0f;
            b.asleep = false;
        }
    }
}

u64 PhysicsWorld::state_hash() const {
    // FNV-1a over the raw bits of every body's pose, in index order. Hashing the
    // bits rather than rounding means the test detects any divergence at all,
    // not just one large enough to survive a tolerance.
    u64 hash = 14695981039346656037ull;
    auto mix = [&hash](f32 value) {
        u32 bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits");
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 4; ++i) {
            hash ^= (bits >> (i * 8)) & 0xFFu;
            hash *= 1099511628211ull;
        }
    };

    for (const RigidBody& b : m_bodies) {
        if (!b.alive) {
            continue;
        }
        mix(b.position.x);
        mix(b.position.y);
        mix(b.position.z);
        mix(b.orientation.x);
        mix(b.orientation.y);
        mix(b.orientation.z);
        mix(b.orientation.w);
    }
    return hash;
}

} // namespace nf::physics
