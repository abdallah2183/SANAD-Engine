#pragma once

// NF/Physics/Cloth.hpp — first-party cloth / soft-body simulation (Phase 16, §38).
//
// WHY THIS IS NOT ON JOLT: §38 lists Cloth next to the Jolt-backed features, but
// Jolt is a rigid-body engine and ships no deformable solver. This module is
// therefore first-party and deliberately Jolt-free — no Jolt headers, no Jolt
// types, no JoltWorld handle. It depends on NF/Core math and on the engine's own
// shape vocabulary (NF/Physics/Shapes.hpp) so a game can hand it the same
// sphere/box/plane colliders it already builds for everything else, while the
// rigid-body solver stays completely untouched by it.
//
// MODEL AND ITS LIMITS — read this before using it for anything that has to look
// physically right:
//   - Position-Based Dynamics (PBD), NOT XPBD. `stiffness` is the fraction of the
//     full positional correction applied per iteration, so it is a visual knob,
//     not a material constant: the effective stiffness of the sheet grows with
//     `iterations`, and a value that looks taut at 8 iterations looks soft at 2.
//   - Distance constraints only (structural / shear / bend). The cloth resists
//     stretching and only weakly resists folding — there is no bending moment, no
//     plasticity and no tearing. Nothing here ever breaks.
//   - No self-collision. The sheet passes through itself; a deeply folded cloth
//     sinks into its own folds.
//   - Collision is particle-vs-primitive, one projection pass per substep. Fast
//     cloth can tunnel through a thin collider or be pushed out through the far
//     face of a box. The fix is a smaller dt (more substeps), not a bigger solver.
//   - Pins are the only attachment. There is no "attach to a moving body" hook.
//
// DETERMINISM: no randomness, no unordered containers, no threading and no
// allocation inside step(). Iteration order is fixed by construction, so
// identical configurations stepped with identical dt give bit-identical results
// — the property the replay recorder and the network rollback both need.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Physics/Shapes.hpp>

#include <memory>
#include <vector>

namespace nf::physics {

/// Cloth grid setup. The cloth starts as a flat res_x × res_z grid in the XZ
/// plane, centred on `origin`, with `spacing` between neighbouring particles.
struct ClothConfig {
    int res_x = 12;            // particles along +X (>= 2)
    int res_z = 12;            // particles along +Z (>= 2)
    float spacing = 0.25f;     // rest distance between neighbours
    Vec3 origin{0.0f, 2.0f, 0.0f};
    float mass = 0.2f;         // per particle (total mass / count is also fine — document it)
    float damping = 0.02f;     // velocity damping per second
    float stiffness = 0.9f;    // constraint stiffness in [0,1] (PBD compliance proxy)
    int iterations = 8;        // solver iterations per substep
    int substeps = 2;          // substeps per step() call
    Vec3 gravity{0.0f, -9.81f, 0.0f};
};

/// What the cloth can collide against. Reuses the engine's first-party shape
/// vocabulary (sphere / box / plane) so a game can feed the same colliders it
/// already has; nothing here touches the rigid-body solver.
///
/// COLLIDERS ARE ASSUMED STATIC BETWEEN set_colliders() CALLS: their world-space
/// AABB and plane normal are hoisted out of the particle loop at that point. A
/// moving collider must be re-submitted each step, which is the documented
/// contract (and what a game driving a collider from a rigid body already does).
struct ClothCollider {
    Shape shape;               // from NF/Physics/Shapes.hpp
    Vec3 position{0,0,0};      // world position (box: centre; plane: any point on it)
    Quat orientation = Quat::identity();
};

class Cloth {
public:
    explicit Cloth(const ClothConfig& config = ClothConfig{});
    /// Out-of-line because the pimpl is incomplete here; `unique_ptr<Impl>` also
    /// makes Cloth non-copyable and non-movable, which is intentional — a cloth
    /// is owned in place (a container of Cloth needs std::unique_ptr).
    ~Cloth();

    int particle_count() const;         // res_x * res_z
    int index(int ix, int iz) const;    // grid -> particle index

    /// Pins a particle in place (infinite mass). Unpinned particles ignore it.
    ///
    /// Pinning also zeroes the particle's velocity and its previous position, so
    /// a particle pinned mid-flight stops where it is instead of snapping back on
    /// the next substep. Unpinning leaves it where it is, at rest. An
    /// out-of-range index is ignored: like index(), every mutator is total.
    void pin(int particle_index);
    void unpin(int particle_index);
    bool is_pinned(int particle_index) const;

    /// Colliders are replaced wholesale each call (simple + deterministic).
    void set_colliders(const std::vector<ClothCollider>& colliders);

    /// Constant wind acceleration applied to every particle (m/s^2). The game
    /// can vary it per step to fake gusts.
    void set_wind(const Vec3& wind_acceleration);

    /// Advances the simulation by dt, internally split into `substeps`.
    /// dt <= 0 is a no-op.
    void step(float dt);

    /// An out-of-range index returns the zero vector (and zero velocity) rather
    /// than reading past the end: a bad index is a caller bug, and a position of
    /// (0,0,0) is the kind that shows up in a debug draw instead of a crash.
    Vec3 position(int particle_index) const;
    Vec3 velocity(int particle_index) const;

    /// Per-particle normal (average of the triangle normals around it), for a
    /// renderer that wants to light the cloth. Zero-length vectors are
    /// returned as +Y rather than NaN when a neighbourhood is degenerate.
    /// An out-of-range index also returns +Y.
    Vec3 normal(int particle_index) const;

    /// Rest length diagnostic: the largest |current - rest| stretch ratio, so a
    /// test (or the profiler) can see how much the cloth is stretched. 0 means
    /// every constraint sits exactly at its rest length; 0.25 means some
    /// constraint is 25% longer (or shorter) than it was built with. This is a
    /// scalar proxy for solver error, not a physical strain: it takes no account
    /// of which kind of constraint is worst.
    float max_stretch() const;

private:
    struct Impl;                 // pimpl keeps the header light and stable
    std::unique_ptr<Impl> m_impl;
};

} // namespace nf::physics
