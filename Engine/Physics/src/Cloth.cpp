#include <NF/Physics/Cloth.hpp>

#include <algorithm>
#include <cmath>

namespace nf::physics {

namespace {

/// Length below which a vector has no usable direction, and below which a
/// constraint has no usable axis. Two coincident particles must be skipped, not
/// normalised: dividing by ~0 there is exactly how a cloth turns into a cloud of
/// NaNs, and one NaN endpoint poisons every constraint it touches.
constexpr f32 kMinLength = 1.0e-6f;

/// Smallest rest spacing and particle mass the solver will work with. Both are
/// divisors (rest lengths in max_stretch, mass in 1/mass), so a zero would be a
/// divide by zero rather than a silly-looking cloth.
constexpr f32 kMinSpacing = 1.0e-3f;
constexpr f32 kMinMass = 1.0e-6f;

bool is_finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Vec3 has no operator[], and the box push-out needs to address one axis chosen
/// at run time. Kept file-local and tiny rather than adding indexing to Core's
/// math type for one caller.
void set_component(Vec3& v, int axis, f32 value) {
    if (axis == 0) {
        v.x = value;
    } else if (axis == 1) {
        v.y = value;
    } else {
        v.z = value;
    }
}

} // namespace

// ============================================================================
// Implementation
//
// A plain PBD loop: integrate, project the distance constraints a few times,
// push particles out of the colliders, then recover the velocities from the
// position change. Everything it needs is preallocated, because step() runs
// inside the engine's fixed-timestep loop and an allocation there is a
// frame-time spike waiting to happen.
// ============================================================================

struct Cloth::Impl {
    /// One distance constraint. The kind (structural / shear / bend) is not
    /// stored: every family is solved by the same projection, and the kind only
    /// ever mattered while the list was being built.
    struct Constraint {
        u32 a = 0;
        u32 b = 0;
        f32 rest = 0.0f;
    };

    /// Sanitised configuration. step() reads only from here, so a hostile config
    /// (res 0, substeps -3, spacing 0) can never reach the solver arithmetic.
    ClothConfig config;

    std::vector<Vec3> pos;
    std::vector<Vec3> prev;   ///< Position at the start of the substep, i.e. the
                              ///< other half of PBD's (pos - prev)/h velocity.
    std::vector<Vec3> vel;
    std::vector<Vec3> spawn;  ///< Where the particle was built: the NaN guard's
                              ///< last-resort reset target.
    std::vector<f32> inv_mass; ///< 0 for a pinned particle, which is what makes
                               ///< "a pinned particle never moves" fall out of
                               ///< the constraint projection for free.
    std::vector<u8> pinned;
    std::vector<Constraint> constraints;

    std::vector<ClothCollider> colliders;
    /// Hoisted per-collider data, rebuilt in set_colliders() and never inside
    /// step(): colliders are static between submissions (see the header).
    std::vector<Aabb> collider_aabb;
    std::vector<Vec3> collider_normal; ///< plane normal in world space

    /// Written by the collision pass of the current substep and read by its
    /// velocity update, so the surface normal does not have to be recomputed.
    std::vector<Vec3> contact_normal;
    std::vector<u8> contact;

    Vec3 wind{0.0f, 0.0f, 0.0f};

    int res_x = 2;
    int res_z = 2;

    int grid_index(int ix, int iz) const { return iz * res_x + ix; }

    void build();
    void build_constraints();
    void add_constraint(int ia, int ib);

    void predict(f32 h, f32 damp, const Vec3& dv);
    void solve_constraints();
    void solve_collisions();
    void resolve(const ClothCollider& collider, const Vec3& plane_normal, usize i);
    void note_contact(usize i, const Vec3& n);
    void update_velocities(f32 h);
    void guard_non_finite();
};

void Cloth::Impl::build() {
    const int nx = config.res_x;
    const int nz = config.res_z;
    const usize count = static_cast<usize>(nx) * static_cast<usize>(nz);

    pos.resize(count);
    prev.resize(count);
    vel.resize(count);
    spawn.resize(count);
    inv_mass.assign(count, 1.0f / config.mass);
    pinned.assign(count, 0);
    contact_normal.assign(count, Vec3::zero);
    contact.assign(count, 0);

    // Flat sheet centred on the origin. The same expression on both axes is what
    // keeps the sheet centred for even and odd resolutions alike, with no
    // special case for the odd one.
    const f32 cx = 0.5f * static_cast<f32>(nx - 1);
    const f32 cz = 0.5f * static_cast<f32>(nz - 1);
    for (int iz = 0; iz < nz; ++iz) {
        for (int ix = 0; ix < nx; ++ix) {
            const Vec3 p = config.origin + Vec3{(static_cast<f32>(ix) - cx) * config.spacing,
                                                0.0f,
                                                (static_cast<f32>(iz) - cz) * config.spacing};
            const usize i = static_cast<usize>(grid_index(ix, iz));
            pos[i] = p;
            prev[i] = p;
            vel[i] = Vec3::zero;
            spawn[i] = p;
        }
    }

    colliders.clear();
    collider_aabb.clear();
    collider_normal.clear();
    build_constraints();
}

void Cloth::Impl::add_constraint(int ia, int ib) {
    const u32 a = static_cast<u32>(ia);
    const u32 b = static_cast<u32>(ib);
    // Rest lengths are measured from the built sheet, not derived from
    // `spacing`: the two agree for a flat grid, and measuring means a future
    // non-flat initial shape needs no second formula here.
    const f32 rest = (pos[b] - pos[a]).length();
    if (rest <= kMinLength) return; // duplicate particles: nothing to solve along
    constraints.push_back(Constraint{a, b, rest});
}

void Cloth::Impl::build_constraints() {
    constraints.clear();
    const int nx = config.res_x;
    const int nz = config.res_z;

    // Exactly the number of pairs the five passes below add, so building the
    // list never reallocates.
    const usize pairs =
        static_cast<usize>(nx - 1) * static_cast<usize>(nz) +          // structural X
        static_cast<usize>(nx) * static_cast<usize>(nz - 1) +          // structural Z
        2u * static_cast<usize>(nx - 1) * static_cast<usize>(nz - 1) + // shear
        static_cast<usize>(nx > 2 ? nx - 2 : 0) * static_cast<usize>(nz) + // bend X
        static_cast<usize>(nx) * static_cast<usize>(nz > 2 ? nz - 2 : 0);  // bend Z
    constraints.reserve(pairs);

    for (int iz = 0; iz < nz; ++iz) {
        for (int ix = 0; ix < nx; ++ix) {
            // Structural: the two in-plane neighbours. These are the constraints
            // that make it a sheet rather than a particle cloud.
            if (ix + 1 < nx) add_constraint(grid_index(ix, iz), grid_index(ix + 1, iz));
            if (iz + 1 < nz) add_constraint(grid_index(ix, iz), grid_index(ix, iz + 1));

            // Shear: both diagonals of the cell. They are what stops the sheet
            // from collapsing into a parallelogram, and they carry most of its
            // resistance to folding because they are the shortest of the three
            // families and therefore corrected hardest.
            if (ix + 1 < nx && iz + 1 < nz) {
                add_constraint(grid_index(ix, iz), grid_index(ix + 1, iz + 1));
                add_constraint(grid_index(ix + 1, iz), grid_index(ix, iz + 1));
            }

            // Bend: skip-one neighbours, at twice the rest length. A cheap proxy
            // for bending stiffness — it resists the *distance* change across a
            // fold, not the fold angle, so a rolled cloth is springy rather than
            // stiff. See the header: this is the part XPBD would do properly.
            if (ix + 2 < nx) add_constraint(grid_index(ix, iz), grid_index(ix + 2, iz));
            if (iz + 2 < nz) add_constraint(grid_index(ix, iz), grid_index(ix, iz + 2));
        }
    }
}

void Cloth::Impl::predict(f32 h, f32 damp, const Vec3& dv) {
    const usize n = pos.size();
    for (usize i = 0; i < n; ++i) {
        // A pinned particle is not integrated at all: no gravity, no wind, no
        // damping, no drift. Skipping it here (rather than multiplying by zero
        // mass) is what makes "stays exactly at its spawn position" a bit-exact
        // guarantee instead of an almost-guarantee.
        if (pinned[i]) continue;
        vel[i] = (vel[i] + dv) * damp;
        prev[i] = pos[i];
        pos[i] += vel[i] * h;
    }
}

void Cloth::Impl::solve_constraints() {
    const f32 stiffness = config.stiffness;
    for (int it = 0; it < config.iterations; ++it) {
        // Gauss-Seidel, in the fixed order the list was built in. That order is
        // the single most important determinism decision in this file: a
        // different order is a different cloth, bit for bit.
        for (const Constraint& c : constraints) {
            const f32 wa = inv_mass[c.a];
            const f32 wb = inv_mass[c.b];
            const f32 w_sum = wa + wb;
            if (w_sum <= 0.0f) continue; // both ends pinned: nothing can move

            const Vec3 delta = pos[c.b] - pos[c.a];
            const f32 len = delta.length();
            if (len < kMinLength) continue;

            // Scaled projection: `stiffness` is the fraction of the full
            // correction taken this iteration, so it is a softness knob shared by
            // every constraint family (see ClothConfig).
            const Vec3 correction = delta * ((len - c.rest) / len * stiffness / w_sum);
            // Inverse mass decides who moves: at w = 0 the correction is applied
            // as an exact +0, i.e. a pinned particle does not move at all. The
            // guard makes that guarantee literal rather than a float coincidence.
            if (wa > 0.0f) pos[c.a] += correction * wa;
            if (wb > 0.0f) pos[c.b] -= correction * wb;
        }
    }
}

void Cloth::Impl::note_contact(usize i, const Vec3& n) {
    contact[i] = 1;
    contact_normal[i] = n;
}

void Cloth::Impl::solve_collisions() {
    // Contact flags are per-substep state. They are cleared even when there is
    // nothing to collide against, or the velocity update would keep removing a
    // velocity component for a contact that ended several frames ago.
    std::fill(contact.begin(), contact.end(), static_cast<u8>(0));
    if (colliders.empty()) return;

    const usize n = pos.size();
    for (usize i = 0; i < n; ++i) {
        if (pinned[i]) continue; // pins are immovable by construction
        const Vec3 before = pos[i];
        for (usize c = 0; c < colliders.size(); ++c) {
            // Cheap rejection first: a particle outside the collider's world AABB
            // cannot be inside the collider. A plane's AABB is the +-1e9 box
            // compute_aabb returns for unbounded shapes, so planes fall straight
            // through to the exact test, which is the point of that convention.
            const Aabb& bb = collider_aabb[c];
            if (pos[i].x < bb.min.x || pos[i].x > bb.max.x ||
                pos[i].y < bb.min.y || pos[i].y > bb.max.y ||
                pos[i].z < bb.min.z || pos[i].z > bb.max.z) {
                continue;
            }
            resolve(colliders[c], collider_normal[c], i);
        }

        // The push-out is a position-level fix, not a kick: the same delta is
        // carried into `prev`, so the velocity update below reports the particle's
        // velocity from *before* it was pushed out of the collider. The contact
        // response is then applied once, explicitly, in update_velocities().
        // Without this every correction would also inject outward velocity and a
        // resting cloth would buzz against the floor forever.
        prev[i] += pos[i] - before;
    }
}

void Cloth::Impl::resolve(const ClothCollider& collider, const Vec3& plane_normal, usize i) {
    switch (collider.shape.type) {
        case ShapeType::Sphere: {
            const Vec3 d = pos[i] - collider.position;
            const f32 dist = d.length();
            const f32 radius = collider.shape.sphere.radius;
            if (dist >= radius) return; // outside: the particle is not penetrating
            // Outward direction. A particle exactly at the centre has no defined
            // direction at all; +Y is the deterministic fallback, the same one
            // Shape::make_plane uses for a degenerate normal.
            const Vec3 n = dist > kMinLength ? d / dist : Vec3::up;
            pos[i] = collider.position + n * radius;
            note_contact(i, n);
            return;
        }

        case ShapeType::Plane: {
            // make_plane's normal points at the valid side — a sphere is free
            // while signed_distance >= radius, so the free half-space is the
            // positive one (collide_sphere_plane in Narrowphase.cpp). The fix is
            // therefore to project any particle with a negative signed distance
            // back onto the surface.
            const f32 signed_distance = (pos[i] - collider.position).dot(plane_normal);
            if (signed_distance >= 0.0f) return;
            pos[i] -= plane_normal * signed_distance;
            note_contact(i, plane_normal);
            return;
        }

        case ShapeType::Box: {
            // Work in the box's own frame: three comparisons answer "inside?"
            // exactly, with no support query and no tolerance to tune.
            const Quat inverse = collider.orientation.conjugate();
            const Vec3 local = inverse.rotate(pos[i] - collider.position);
            const Vec3 h = collider.shape.box.half_extents;
            const f32 px = h.x - std::abs(local.x);
            const f32 py = h.y - std::abs(local.y);
            const f32 pz = h.z - std::abs(local.z);
            if (px <= 0.0f || py <= 0.0f || pz <= 0.0f) return; // outside

            // Deepest penetration on an axis = smallest clearance = the face the
            // particle leaves through. Choosing the *nearest* face (rather than
            // the entry face) is what makes a cloth draped over an edge slide
            // over it instead of being ejected to the far side of the box.
            int axis = 0;
            f32 penetration = px;
            if (py < penetration) {
                penetration = py;
                axis = 1;
            }
            if (pz < penetration) {
                axis = 2;
            }

            const f32 component = axis == 0 ? local.x : (axis == 1 ? local.y : local.z);
            const f32 sign = component >= 0.0f ? 1.0f : -1.0f;
            Vec3 direction_local{0.0f, 0.0f, 0.0f};
            set_component(direction_local, axis, sign);
            const Vec3 n = collider.orientation.rotate(direction_local);

            // Ask the shape where its surface is along that face normal: for a
            // box the support point is a corner of the face, so its projection
            // onto the normal is the half-extent. Keeping the question with
            // Shape::support (rather than re-reading h[axis]) leaves one answerer
            // for "where is the surface" across all three shape types.
            const f32 face =
                (collider.shape.support(collider.position, collider.orientation, n) -
                 collider.position)
                    .dot(n);

            Vec3 corrected = local;
            set_component(corrected, axis, sign * face);
            pos[i] = collider.position + collider.orientation.rotate(corrected);
            note_contact(i, n);
            return;
        }

        default:
            // Unreachable for the closed ShapeType set; total behaviour beats a
            // switch that falls off the end if a shape is ever added.
            return;
    }
}

void Cloth::Impl::update_velocities(f32 h) {
    const f32 inv_h = 1.0f / h;
    const usize n = pos.size();
    for (usize i = 0; i < n; ++i) {
        if (pinned[i]) {
            vel[i] = Vec3::zero; // a pin is a pin: it does not accelerate either
            continue;
        }
        Vec3 v = (pos[i] - prev[i]) * inv_h;
        if (contact[i]) {
            // Remove the velocity component pushing into the surface. The
            // tangential part survives, which is what lets a cloth slide down a
            // sphere instead of sticking to it — and, equally, is the whole
            // friction model: there is no friction here at all.
            const f32 vn = v.dot(contact_normal[i]);
            if (vn < 0.0f) v -= contact_normal[i] * vn;
        }
        vel[i] = v;
    }
}

void Cloth::Impl::guard_non_finite() {
    const usize n = pos.size();
    for (usize i = 0; i < n; ++i) {
        if (is_finite(pos[i]) && is_finite(vel[i])) continue;
        // A non-finite particle is unrecoverable numerically — but it is also
        // contagious: one NaN endpoint poisons every constraint it belongs to,
        // and the whole sheet goes within one iteration. Restoring its spawn
        // state contains the damage to that one particle. Clamping it to a
        // "sane box" instead would leave a visible spike welded into the cloth.
        pos[i] = spawn[i];
        prev[i] = spawn[i];
        vel[i] = Vec3::zero;
    }
}

// ============================================================================
// Public API
// ============================================================================

Cloth::Cloth(const ClothConfig& config) : m_impl(std::make_unique<Impl>()) {
    Impl& s = *m_impl;

    // Sanitise once, here, instead of guarding at every use site downstream.
    // res < 2 has no structural constraint at all and would silently behave like
    // a particle cloud; substeps 0 divides by zero in step(); a very large
    // stiffness would have to be clamped before the projection anyway.
    s.config = config;
    s.config.res_x = std::max(2, config.res_x);
    s.config.res_z = std::max(2, config.res_z);
    s.config.spacing = std::max(config.spacing, kMinSpacing);
    s.config.mass = std::max(config.mass, kMinMass);
    s.config.iterations = std::max(1, config.iterations);
    s.config.substeps = std::max(1, config.substeps);
    s.config.stiffness = std::clamp(config.stiffness, 0.0f, 1.0f);
    // damping = 1 would freeze every particle outright (multiplier 0), which is
    // never what "damp the velocity" is asking for; the half-open range keeps
    // (1 - damping) strictly positive.
    s.config.damping = std::clamp(config.damping, 0.0f, 0.999f);

    s.res_x = s.config.res_x;
    s.res_z = s.config.res_z;
    s.build();
}

Cloth::~Cloth() = default;

int Cloth::particle_count() const {
    return m_impl->res_x * m_impl->res_z;
}

int Cloth::index(int ix, int iz) const {
    const Impl& s = *m_impl;
    if (ix < 0 || iz < 0 || ix >= s.res_x || iz >= s.res_z) return -1;
    return s.grid_index(ix, iz);
}

void Cloth::pin(int particle_index) {
    Impl& s = *m_impl;
    if (particle_index < 0 || particle_index >= static_cast<int>(s.pos.size())) return;
    const usize i = static_cast<usize>(particle_index);
    s.pinned[i] = 1;
    s.inv_mass[i] = 0.0f;
    // Clearing the velocity is what makes "pin a falling cloth" pick the particle
    // up where it is. Leaving it would let the velocity resurface on unpin and
    // fling a particle that had been pinned for a while.
    s.vel[i] = Vec3::zero;
    s.prev[i] = s.pos[i];
}

void Cloth::unpin(int particle_index) {
    Impl& s = *m_impl;
    if (particle_index < 0 || particle_index >= static_cast<int>(s.pos.size())) return;
    const usize i = static_cast<usize>(particle_index);
    s.pinned[i] = 0;
    s.inv_mass[i] = 1.0f / s.config.mass;
    s.vel[i] = Vec3::zero;
    s.prev[i] = s.pos[i];
}

bool Cloth::is_pinned(int particle_index) const {
    const Impl& s = *m_impl;
    if (particle_index < 0 || particle_index >= static_cast<int>(s.pos.size())) return false;
    return s.pinned[static_cast<usize>(particle_index)] != 0;
}

void Cloth::set_colliders(const std::vector<ClothCollider>& colliders) {
    Impl& s = *m_impl;
    s.colliders = colliders;
    s.collider_aabb.resize(s.colliders.size());
    s.collider_normal.resize(s.colliders.size());
    for (usize i = 0; i < s.colliders.size(); ++i) {
        const ClothCollider& c = s.colliders[i];
        c.shape.compute_aabb(c.position, c.orientation, s.collider_aabb[i].min,
                             s.collider_aabb[i].max);
        // Only a plane's normal is needed later, but the array is index-aligned
        // with the collider array, so every slot gets a defined value.
        s.collider_normal[i] = c.shape.type == ShapeType::Plane
                                   ? c.orientation.rotate(c.shape.plane.normal).normalized()
                                   : Vec3::zero;
    }
}

void Cloth::set_wind(const Vec3& wind_acceleration) {
    m_impl->wind = wind_acceleration;
}

void Cloth::step(float dt) {
    // `!(dt > 0)` rather than `dt <= 0` so a NaN dt is also a no-op: NaN would
    // otherwise reach every particle through h in a single call.
    if (!(dt > 0.0f)) return;

    Impl& s = *m_impl;
    const f32 h = dt / static_cast<f32>(s.config.substeps);
    // Per-second damping expressed as a per-substep multiplier. Computed once per
    // step rather than per particle per substep: the value is identical for every
    // particle, so recomputing it would only be slower.
    const f32 damp = std::pow(std::max(0.0f, 1.0f - s.config.damping), h);
    const Vec3 dv = (s.config.gravity + s.wind) * h;

    for (int sub = 0; sub < s.config.substeps; ++sub) {
        s.predict(h, damp, dv);
        s.solve_constraints();
        s.solve_collisions();
        s.update_velocities(h);
        // Per substep, not per step: the guard must see a bad particle before the
        // next substep's constraints spread it across the neighbourhood.
        s.guard_non_finite();
    }
}

Vec3 Cloth::position(int particle_index) const {
    const Impl& s = *m_impl;
    if (particle_index < 0 || particle_index >= static_cast<int>(s.pos.size())) return Vec3::zero;
    return s.pos[static_cast<usize>(particle_index)];
}

Vec3 Cloth::velocity(int particle_index) const {
    const Impl& s = *m_impl;
    if (particle_index < 0 || particle_index >= static_cast<int>(s.vel.size())) return Vec3::zero;
    return s.vel[static_cast<usize>(particle_index)];
}

Vec3 Cloth::normal(int particle_index) const {
    const Impl& s = *m_impl;
    if (particle_index < 0 || particle_index >= static_cast<int>(s.pos.size())) return Vec3::up;

    const int ix = particle_index % s.res_x;
    const int iz = particle_index / s.res_x;

    // The up-to-four grid cells that share this particle, two triangles each.
    // Winding is chosen so that a flat, un-deformed sheet facing +Y gives +Y:
    // with a = (x, z), b = (x, z+1), c = (x+1, z+1), d = (x+1, z), the triangles
    // (a,b,c) and (a,c,d) both have a normal of +Y before normalisation.
    Vec3 sum = Vec3::zero;
    for (int dz = -1; dz <= 0; ++dz) {
        for (int dx = -1; dx <= 0; ++dx) {
            const int x0 = ix + dx;
            const int z0 = iz + dz;
            if (x0 < 0 || z0 < 0 || x0 + 1 >= s.res_x || z0 + 1 >= s.res_z) continue;

            const Vec3 a = s.pos[static_cast<usize>(s.grid_index(x0, z0))];
            const Vec3 b = s.pos[static_cast<usize>(s.grid_index(x0, z0 + 1))];
            const Vec3 c = s.pos[static_cast<usize>(s.grid_index(x0 + 1, z0 + 1))];
            const Vec3 d = s.pos[static_cast<usize>(s.grid_index(x0 + 1, z0))];
            // Unnormalised cross products: area weighting for free, and a
            // degenerate (zero-area) triangle contributes nothing rather than a
            // direction it does not have.
            sum += (b - a).cross(c - a);
            sum += (c - a).cross(d - a);
        }
    }

    const f32 len = sum.length();
    return len > kMinLength ? sum / len : Vec3::up;
}

float Cloth::max_stretch() const {
    const Impl& s = *m_impl;
    f32 worst = 0.0f;
    for (const Impl::Constraint& c : s.constraints) {
        if (c.rest <= kMinLength) continue;
        const f32 len = (s.pos[c.b] - s.pos[c.a]).length();
        worst = std::max(worst, std::abs(len - c.rest) / c.rest);
    }
    return worst;
}

} // namespace nf::physics
