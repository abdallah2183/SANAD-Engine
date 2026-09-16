// Tests/PhysicsTests/test_cloth.cpp — first-party cloth / soft body (design §38).
//
// Cloth is the one physics feature Jolt cannot provide, so it is the one physics
// feature whose tests cannot lean on a reference implementation: everything here
// is checked against the model's own invariants (pins do not move, constraints
// stay near their rest length, colliders are not penetrated) and against a
// control run rather than against a second solver.
//
// The control run matters more than usual in this file. "The cloth fell" is not
// evidence on its own — a cloth with no constraints at all falls too. Wherever a
// case asserts that a force did something, the same cloth is run without it.
//
// Every case is headless, single-threaded, fixed dt, no rendering.

#include <NF/Physics/Cloth.hpp>
#include <NF/Test/TestFramework.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;

void settle(Cloth& cloth, int steps, float dt = kDt) {
    for (int i = 0; i < steps; ++i) cloth.step(dt);
}

float mean_y(const Cloth& cloth) {
    float sum = 0.0f;
    for (int i = 0; i < cloth.particle_count(); ++i) sum += cloth.position(i).y;
    return sum / static_cast<float>(cloth.particle_count());
}

Vec3 centre_of_mass(const Cloth& cloth) {
    Vec3 sum = Vec3::zero;
    for (int i = 0; i < cloth.particle_count(); ++i) sum += cloth.position(i);
    return sum / static_cast<float>(cloth.particle_count());
}

float mean_speed(const Cloth& cloth) {
    float sum = 0.0f;
    for (int i = 0; i < cloth.particle_count(); ++i) sum += cloth.velocity(i).length();
    return sum / static_cast<float>(cloth.particle_count());
}

bool all_finite(const Cloth& cloth) {
    for (int i = 0; i < cloth.particle_count(); ++i) {
        const Vec3 p = cloth.position(i);
        const Vec3 v = cloth.velocity(i);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    }
    return true;
}

/// Every particle of the grid, as a flat list — the shape a bit-exact
/// determinism comparison needs and cannot get from a scatter of accessors.
std::vector<float> snapshot(const Cloth& cloth) {
    std::vector<float> out;
    out.reserve(static_cast<usize>(cloth.particle_count()) * 6u);
    for (int i = 0; i < cloth.particle_count(); ++i) {
        const Vec3 p = cloth.position(i);
        const Vec3 v = cloth.velocity(i);
        out.push_back(p.x);
        out.push_back(p.y);
        out.push_back(p.z);
        out.push_back(v.x);
        out.push_back(v.y);
        out.push_back(v.z);
    }
    return out;
}

ClothCollider plane_collider(float y) {
    ClothCollider c;
    c.shape = Shape::make_plane(Vec3{0, 1, 0});
    c.position = Vec3{0, y, 0};
    return c;
}

ClothCollider sphere_collider(Vec3 centre, float radius) {
    ClothCollider c;
    c.shape = Shape::make_sphere(radius);
    c.position = centre;
    return c;
}

ClothCollider box_collider(Vec3 centre, Vec3 half_extents) {
    ClothCollider c;
    c.shape = Shape::make_box(half_extents);
    c.position = centre;
    return c;
}

/// Signed clearance of a point against an axis-aligned box: positive outside,
/// negative inside by the depth of the deepest face. The cloth tests keep their
/// boxes axis-aligned on purpose, so the check does not need the rotation the
/// solver handles.
float box_clearance(const Vec3& p, const Vec3& centre, const Vec3& half_extents) {
    const Vec3 d = (p - centre).abs() - half_extents;
    return std::max(d.x, std::max(d.y, d.z));
}

} // namespace

// --- Topology and setup ---------------------------------------------------

NF_TEST(cloth_grid_size_and_indices) {
    ClothConfig cfg;
    cfg.res_x = 5;
    cfg.res_z = 3;
    Cloth cloth(cfg);

    NF_CHECK(cloth.particle_count() == 15); // res_x * res_z, nothing more
    NF_CHECK(cloth.particle_count() == 5 * 3);

    // Row-major grid: ix runs fastest, iz is the row.
    NF_CHECK(cloth.index(0, 0) == 0);
    NF_CHECK(cloth.index(4, 0) == 4);
    NF_CHECK(cloth.index(0, 1) == 5);
    NF_CHECK(cloth.index(4, 2) == 14);

    // Out of range is -1, not a wrap-around or an out-of-bounds read.
    NF_CHECK(cloth.index(-1, 0) == -1);
    NF_CHECK(cloth.index(0, -1) == -1);
    NF_CHECK(cloth.index(5, 0) == -1);
    NF_CHECK(cloth.index(0, 3) == -1);
    // ...and the accessors stay total through it.
    NF_CHECK(cloth.position(cloth.index(9, 9)).length_sq() == 0.0f);
    NF_CHECK(cloth.velocity(cloth.index(9, 9)).length_sq() == 0.0f);
    NF_CHECK(!cloth.is_pinned(cloth.index(9, 9)));

    // The sheet is centred on the origin: a 5-wide grid at spacing 0.25 spans
    // -0.5 .. +0.5, a 3-deep one spans -0.25 .. +0.25.
    const Vec3 corner = cloth.position(cloth.index(0, 0));
    NF_CHECK_NEAR(corner.x, -0.5f, 1e-5f);
    NF_CHECK_NEAR(corner.y, 2.0f, 1e-5f); // the configured origin's Y
    NF_CHECK_NEAR(corner.z, -0.25f, 1e-5f);
    const Vec3 opposite = cloth.position(cloth.index(4, 2));
    NF_CHECK_NEAR(opposite.x, 0.5f, 1e-5f);
    NF_CHECK_NEAR(opposite.z, 0.25f, 1e-5f);

    // Neighbours really are `spacing` apart, which is what the rest lengths are
    // measured from.
    NF_CHECK_NEAR((cloth.position(cloth.index(1, 0)) - corner).length(), 0.25f, 1e-5f);

    // Default construction (and a step) must not crash either.
    Cloth default_cloth;
    NF_CHECK(default_cloth.particle_count() == 144);
    NF_CHECK_NEAR(default_cloth.position(default_cloth.index(11, 11)).y, 2.0f, 1e-5f);
    default_cloth.step(kDt);
    NF_CHECK(all_finite(default_cloth));
}

// --- Gravity, pins, and hanging ------------------------------------------

NF_TEST(cloth_falls_under_gravity) {
    Cloth cloth(ClothConfig{}); // unpinned, no colliders
    const float y0 = centre_of_mass(cloth).y;
    NF_CHECK_NEAR(mean_speed(cloth), 0.0f, 1e-6f); // it starts at rest

    settle(cloth, 120); // two seconds
    const Vec3 com = centre_of_mass(cloth);
    // Two seconds of 9.81 m/s² is roughly 19 m of free fall; the exact figure
    // depends on the substep damping, so the assertion is "it fell a lot".
    NF_CHECK(com.y < y0 - 5.0f);
    NF_CHECK(mean_speed(cloth) > 5.0f);
    NF_CHECK(all_finite(cloth));
}

NF_TEST(cloth_pins_hold) {
    ClothConfig cfg;
    Cloth cloth(cfg);

    // Pin the whole iz = 0 edge: the classic hanging sheet.
    std::vector<Vec3> spawn;
    for (int ix = 0; ix < cfg.res_x; ++ix) {
        const int p = cloth.index(ix, 0);
        spawn.push_back(cloth.position(p));
        cloth.pin(p);
        NF_CHECK(cloth.is_pinned(p));
    }
    NF_CHECK(!cloth.is_pinned(cloth.index(0, 1)));

    // A second pin, in the middle of the sheet: an edge pin only shows that the
    // sheet hangs, a mid-sheet pin shows that the sheet sags *around* a fixed
    // point without dragging it along.
    const int probe = cloth.index(3, 6);
    const Vec3 probe_spawn = cloth.position(probe);
    cloth.pin(probe);

    settle(cloth, 240); // four seconds of gravity pulling on the free edge

    // A pin is exact, not approximate: these particles are not integrated at
    // all, so they are bit-identical to where they were built.
    for (int ix = 0; ix < cfg.res_x; ++ix) {
        const int p = cloth.index(ix, 0);
        const Vec3 now = cloth.position(p);
        NF_CHECK(now.x == spawn[static_cast<usize>(ix)].x);
        NF_CHECK(now.y == spawn[static_cast<usize>(ix)].y);
        NF_CHECK(now.z == spawn[static_cast<usize>(ix)].z);
        NF_CHECK(cloth.velocity(p).length_sq() == 0.0f);
    }
    NF_CHECK(cloth.position(probe).x == probe_spawn.x);
    NF_CHECK(cloth.position(probe).y == probe_spawn.y);
    NF_CHECK(cloth.position(probe).z == probe_spawn.z);
    NF_CHECK(cloth.velocity(probe).length_sq() == 0.0f);

    // ...and the rest of the sheet really is hanging from them: the mean height
    // of the free particles is well below the pinned ones.
    float pinned_mean = 0.0f;
    float free_mean = 0.0f;
    int pinned_count = 0;
    int free_count = 0;
    for (int iz = 0; iz < cfg.res_z; ++iz) {
        for (int ix = 0; ix < cfg.res_x; ++ix) {
            const int p = cloth.index(ix, iz);
            if (cloth.is_pinned(p)) {
                pinned_mean += cloth.position(p).y;
                ++pinned_count;
            } else {
                free_mean += cloth.position(p).y;
                ++free_count;
            }
        }
    }
    pinned_mean /= static_cast<float>(pinned_count);
    free_mean /= static_cast<float>(free_count);
    NF_CHECK(pinned_count == cfg.res_x + 1); // the whole edge, plus the mid-sheet pin
    NF_CHECK_NEAR(pinned_mean, 2.0f, 1e-5f); // every pin is on the spawn plane
    NF_CHECK(free_mean < pinned_mean - 0.5f);

    // Unpinning is a real release, and its observable is motion: the particle
    // stops being pinned in place and takes part in the simulation again. Note
    // that it moves *up* here rather than down — a hanging sheet is a pendulum
    // in tension, so a released particle rides that motion, and asserting a
    // direction would be asserting the phase of the swing.
    const Vec3 before = cloth.position(probe);
    cloth.unpin(probe);
    NF_CHECK(!cloth.is_pinned(probe));
    settle(cloth, 60);
    NF_CHECK((cloth.position(probe) - before).length() > 0.05f);

    // Out-of-range pin/unpin are ignored rather than writing out of bounds.
    cloth.pin(-1);
    cloth.pin(cloth.particle_count());
    cloth.unpin(-1);
    cloth.unpin(cloth.particle_count());
    NF_CHECK(all_finite(cloth));
}

NF_TEST(cloth_hangs_taut_between_two_pins) {
    ClothConfig cfg;
    Cloth cloth(cfg);
    // Two corners only: the hardest hanging case for the solver, because every
    // constraint in the sheet ends up on the chain between them.
    cloth.pin(cloth.index(0, 0));
    cloth.pin(cloth.index(cfg.res_x - 1, 0));

    settle(cloth, 240);

    // "Still connected": no constraint is anywhere near torn apart. The bound is
    // generous because PBD at 8 iterations with stiffness 0.9 is soft under load
    // (that softness is the documented model, not a bug), but a sheet that had
    // come apart would be at several hundred percent.
    const float stretch = cloth.max_stretch();
    NF_CHECK(stretch < 0.5f);
    NF_CHECK(stretch >= 0.0f);

    float top_mean = 0.0f;
    float bottom_mean = 0.0f;
    for (int ix = 0; ix < cfg.res_x; ++ix) {
        top_mean += cloth.position(cloth.index(ix, 0)).y;
        bottom_mean += cloth.position(cloth.index(ix, cfg.res_z - 1)).y;
    }
    top_mean /= static_cast<float>(cfg.res_x);
    bottom_mean /= static_cast<float>(cfg.res_x);
    NF_CHECK(bottom_mean < top_mean - 1.0f);
    NF_CHECK_NEAR(cloth.position(cloth.index(0, 0)).y, 2.0f, 1e-5f); // the pins held
    NF_CHECK(all_finite(cloth));
}

// --- Collision ------------------------------------------------------------

NF_TEST(cloth_does_not_penetrate_sphere) {
    constexpr float kRadius = 0.5f;
    const Vec3 centre{0.0f, 0.6f, 0.0f}; // floating above the floor: the sheet drapes over it

    Cloth cloth(ClothConfig{});
    cloth.set_colliders({plane_collider(0.0f), sphere_collider(centre, kRadius)});
    settle(cloth, 300);

    for (int i = 0; i < cloth.particle_count(); ++i) {
        const float distance = (cloth.position(i) - centre).length();
        NF_CHECK(distance >= kRadius - 1e-3f);
    }
    NF_CHECK(all_finite(cloth));
    NF_CHECK(mean_y(cloth) > -1e-3f); // it did not sink through the floor either
}

NF_TEST(cloth_rests_on_plane) {
    Cloth cloth(ClothConfig{});
    cloth.set_colliders({plane_collider(0.0f)});
    settle(cloth, 240);

    // Nothing is below the plane, and the plane's normal (+Y, the valid side —
    // see Shape::make_plane) puts every particle on or above it.
    for (int i = 0; i < cloth.particle_count(); ++i) {
        NF_CHECK(cloth.position(i).y >= -1e-3f);
    }
    // Not merely "above": a sheet dropped flat onto a plane lands on it.
    NF_CHECK_NEAR(mean_y(cloth), 0.0f, 0.05f);

    // And the render normals of a flat, landed sheet point up. (This is the only
    // case that exercises normal(); a ragged heap has no single expected normal.)
    NF_CHECK(cloth.normal(cloth.index(5, 5)).y > 0.99f);
    NF_CHECK(cloth.normal(cloth.index(5, 0)).y > 0.99f); // edge particle: half the quads
    NF_CHECK(cloth.normal(-1).nearly_equals(Vec3::up));  // out of range: +Y, not NaN
    NF_CHECK(all_finite(cloth));
}

NF_TEST(cloth_does_not_penetrate_box) {
    // A box slightly narrower than the sheet, so the overhanging edge drapes
    // over it and hangs down the sides — that is what exercises the side faces
    // and not just the top. The floor is only there to catch the hanging part;
    // it is 3 m below the box and the box never reaches it.
    const Vec3 box_centre{0.0f, -0.5f, 0.0f};
    const Vec3 box_half{1.2f, 0.5f, 1.2f}; // top face at y = 0

    Cloth cloth(ClothConfig{});
    cloth.set_colliders({plane_collider(-3.0f), box_collider(box_centre, box_half)});
    settle(cloth, 300);

    for (int i = 0; i < cloth.particle_count(); ++i) {
        NF_CHECK(box_clearance(cloth.position(i), box_centre, box_half) >= -1e-3f);
    }
    NF_CHECK(all_finite(cloth));
}

// --- Determinism and stability -------------------------------------------

NF_TEST(cloth_is_deterministic) {
    // Same configuration, same dt, same call order. Compared bit-exactly: the
    // two runs are the same code path in the same process with no randomness,
    // no threads and no unordered containers, so any difference at all is a real
    // determinism bug (an uninitialised field, an iteration order that depends
    // on memory layout, an allocation-dependent address) rather than FP noise.
    // A tolerance here would hide exactly the class of bug this test exists for.
    auto run = [](bool with_colliders) {
        ClothConfig cfg;
        cfg.iterations = 6;
        Cloth cloth(cfg);
        cloth.pin(cloth.index(0, 0));
        cloth.pin(cloth.index(cfg.res_x - 1, 0));
        if (with_colliders) {
            cloth.set_colliders({plane_collider(0.0f), sphere_collider(Vec3{0, 0.6f, 0}, 0.5f)});
        }
        cloth.set_wind(Vec3{1.5f, 0.0f, -0.75f});
        for (int i = 0; i < 180; ++i) {
            cloth.step(kDt);
            // A varying dt as well: the damping multiplier is a pow() of dt, and
            // a per-step value that depends on nothing but dt must not drift.
            if (i % 7 == 0) cloth.step(kDt * 0.5f);
        }
        return snapshot(cloth);
    };

    const std::vector<float> a = run(true);
    const std::vector<float> b = run(true);
    NF_CHECK(a.size() == b.size());
    NF_CHECK(!a.empty());
    for (usize i = 0; i < a.size(); ++i) {
        NF_CHECK(a[i] == b[i]);
    }

    // The no-collider path is a different loop (the collision pass is skipped);
    // it has to be just as reproducible.
    const std::vector<float> c = run(false);
    const std::vector<float> d = run(false);
    for (usize i = 0; i < c.size(); ++i) {
        NF_CHECK(c[i] == d[i]);
    }
    // Two runs that differ only in a collider must NOT be identical, or the
    // comparison above would be passing for the wrong reason.
    NF_CHECK(a != c);
}

NF_TEST(cloth_stable_at_large_dt) {
    // 0.1 s steps: 5x the frame time the rest of the suite uses. A PBD sheet at
    // this dt is not going to look good, but it must not produce NaN or fling
    // particles out of the world — the guard in step() and the collision
    // projection are what this case is really testing.
    Cloth cloth(ClothConfig{});
    cloth.set_colliders({plane_collider(0.0f)});
    settle(cloth, 300, 0.1f); // thirty seconds of simulation

    NF_CHECK(all_finite(cloth));
    for (int i = 0; i < cloth.particle_count(); ++i) {
        const Vec3 p = cloth.position(i);
        NF_CHECK(std::abs(p.x) < 5.0f);
        NF_CHECK(std::abs(p.z) < 5.0f);
        NF_CHECK(p.y > -1.0f && p.y < 4.0f); // landed on the plane, did not sink or fly
    }
    // Still one sheet, not a cloud: the constraints survived the coarse steps.
    NF_CHECK(cloth.max_stretch() < 1.0f);

    // A degenerate dt is a no-op, not a division by zero or a NaN shower.
    const std::vector<float> before = snapshot(cloth);
    cloth.step(0.0f);
    cloth.step(-1.0f);
    const std::vector<float> after = snapshot(cloth);
    for (usize i = 0; i < before.size(); ++i) {
        NF_CHECK(before[i] == after[i]);
    }
}

// --- Energy and wind ------------------------------------------------------

NF_TEST(cloth_damping_reduces_energy) {
    // Free fall, no colliders, nothing to collide with: both cloths are in the
    // same motion, so the only difference between the two numbers below is the
    // damping term. (The sheet is uniformly accelerated, so it also does not
    // stretch — this isolates damping from the constraint solver entirely.)
    auto run = [](float damping) {
        ClothConfig cfg;
        cfg.damping = damping;
        Cloth cloth(cfg);
        settle(cloth, 120); // two seconds
        return mean_speed(cloth);
    };

    const float undamped = run(0.0f);
    const float damped = run(0.9f);

    NF_CHECK(undamped > 5.0f);          // the control really did accelerate
    NF_CHECK(damped < 0.5f * undamped); // and the damped cloth is much slower
}

NF_TEST(cloth_wind_pushes_sideways) {
    // Hanging sheet, pinned along its ix = 0 edge — a line running along Z, i.e.
    // perpendicular to the wind. That is the flag orientation: wind along +X is
    // then across the free part of the sheet, and the sheet swings. (Pinning the
    // iz = 0 edge instead puts the pin line parallel to the wind, and the sheet
    // can only answer by shearing in its own plane: measured, that moves the
    // centre of mass a fifth as far, which is a much weaker signal.)
    //
    // Both runs are heavily damped so that they settle at their equilibrium
    // shape instead of being measured mid-swing: the comparison then says
    // "steady-state displacement", not "pendulum phase".
    auto run = [](bool with_wind) {
        ClothConfig cfg;
        cfg.damping = 0.5f;
        Cloth cloth(cfg);
        for (int iz = 0; iz < cfg.res_z; ++iz) cloth.pin(cloth.index(0, iz));
        if (with_wind) cloth.set_wind(Vec3{8.0f, 0.0f, 0.0f});
        settle(cloth, 240);
        return centre_of_mass(cloth);
    };

    const Vec3 still = run(false);
    const Vec3 blown = run(true);

    // The control hangs straight down from a pin line at x = -1.375.
    NF_CHECK(still.x < -1.2f);
    NF_CHECK_NEAR(still.z, 0.0f, 0.05f); // gravity alone pushes nothing sideways
    // 8 m/s² of wind against 9.81 m/s² of gravity tilts the hanging sheet by
    // roughly 39 degrees, which moves the centre of mass the better part of a
    // metre in +X. Measured: 0.78 m.
    NF_CHECK(blown.x - still.x > 0.3f);
    NF_CHECK(blown.x > still.x);
    NF_CHECK(std::abs(blown.z - still.z) < 0.05f); // and only along the wind
}
