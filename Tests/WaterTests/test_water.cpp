// Tests/WaterTests/test_water.cpp — the water field's invariants.
//
// The wave field is arithmetic, so these are EXACT where the arithmetic is
// exact: a flat wave is a sampled sine, the crest of one wave sits at a
// quarter wavelength, the analytic normal is the derivative. Where the model
// is an estimate (reflection) or seeded (foam texture) the test asserts the
// shape — monotone, bounded, same-total-different-pattern — not a constant it
// has no right to be. The mesh tests pin the contract between the baked grid
// and the live CPU field, because that agreement is the whole point of a
// surface that is one function on both sides of the GPU.

#include <NF/Test/TestFramework.hpp>
#include <NF/Water/Water.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using nf::f32;
using nf::u32;
using nf::u64;
using nf::Vec2;
using nf::Vec3;
using nf::rendering::StaticMesh;
using nf::water::ShoreSampler;
using nf::water::Wave;
using nf::water::WaterMeshStats;
using nf::water::WaterOptions;
using nf::water::build_water_mesh;
using nf::water::constant_shore;
using nf::water::water_displacement;
using nf::water::water_foam;
using nf::water::water_height;
using nf::water::water_normal;
using nf::water::water_reflection;
using nf::water::water_surge;

/// A single wave along +X, flat (steepness 0): a sampled sine. This is the
/// control surface — everything the field adds on top (steepness, more waves,
/// time) is measured against what this one does.
std::vector<Wave> flat_wave() {
    Wave w;
    w.amplitude = 0.5f;
    w.wavelength = 16.0f;
    w.speed = 1.5f;
    w.direction = Vec2{1.0f, 0.0f};
    w.steepness = 0.0f;
    return {w};
}

/// The same wave, bent: a Gerstner crest instead of a swell.
std::vector<Wave> gerstner_wave() {
    std::vector<Wave> waves = flat_wave();
    waves[0].steepness = 0.6f;
    return waves;
}

WaterOptions options() {
    WaterOptions o;
    o.level = 0.0f;
    o.size = 100.0f;
    o.resolution = 8u;
    o.time = 0.0f;
    return o;
}

/// Component-wise Vec2 comparison. `Vec2` has no `nearly_equals` (unlike Vec3)
/// and no `operator==` on purpose, so a tolerance comparison is spelled out.
bool same_vec2(const Vec2& a, const Vec2& b, f32 tol = 1e-5f) {
    return std::abs(a.x - b.x) < tol && std::abs(a.y - b.y) < tol;
}

} // namespace

// ---------------------------------------------------------------------------
// The field as a sum of sinusoids
// ---------------------------------------------------------------------------

NF_TEST(flat_wave_is_a_sampled_sine) {
    const std::vector<Wave> waves = flat_wave();
    const WaterOptions o = options();
    NF_CHECK_NEAR(water_height(0.0f, 0.0f, waves, o), 0.0f, 1e-6f);
    NF_CHECK_NEAR(water_height(4.0f, 0.0f, waves, o), 0.5f, 1e-6f); // λ/4: crest
    NF_CHECK_NEAR(water_height(8.0f, 0.0f, waves, o), 0.0f, 1e-6f); // λ/2
    NF_CHECK_NEAR(water_height(12.0f, 0.0f, waves, o), -0.5f, 1e-6f); // 3λ/4
    NF_CHECK_NEAR(water_height(4.0f, 0.0f, waves, o),
                  water_height(4.0f, 16.0f, waves, o), 1e-6f); // invariant in Z
}

NF_TEST(level_lifts_the_whole_surface) {
    const std::vector<Wave> waves = flat_wave();
    WaterOptions o = options();
    o.level = 7.0f;
    NF_CHECK_NEAR(water_height(4.0f, 0.0f, waves, o), 7.5f, 1e-6f);
    NF_CHECK_NEAR(water_height(12.0f, 0.0f, waves, o), 6.5f, 1e-6f);
}

NF_TEST(the_field_is_pure_and_does_not_carry_state) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.time = 0.7f;
    // Two calls at the same point answer the same value — no accumulator, no
    // cache that the first query could have poisoned.
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        NF_CHECK_EQ(water_height(x, 3.0f, waves, o), water_height(x, 3.0f, waves, o));
    }
}

NF_TEST(the_seed_does_not_move_the_waves) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions a = options();
    a.time = 1.25f;
    WaterOptions b = a;
    b.seed = 99u; // a different foam texture, not a different sea
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        NF_CHECK_EQ(water_height(x, 3.0f, waves, a), water_height(x, 3.0f, waves, b));
        NF_CHECK(same_vec2(water_displacement(x, 3.0f, waves, a),
                           water_displacement(x, 3.0f, waves, b)));
    }
}

NF_TEST(waves_sum_independently) {
    std::vector<Wave> both;
    {
        Wave w1 = flat_wave()[0];
        Wave w2 = w1;
        w2.direction = Vec2{0.0f, 1.0f};
        w2.amplitude = 0.25f;
        w2.wavelength = 8.0f;
        both = {w1, w2};
    }
    const std::vector<Wave> only_x = {both[0]};
    const std::vector<Wave> only_z = {both[1]};
    const WaterOptions o = options();
    for (u32 i = 0u; i <= 20u; ++i) {
        for (u32 j = 0u; j <= 20u; ++j) {
            const f32 x = -10.0f + static_cast<f32>(i) * 1.0f;
            const f32 z = -10.0f + static_cast<f32>(j) * 1.0f;
            const f32 sum = water_height(x, z, both, o);
            const f32 parts = water_height(x, z, only_x, o) +
                              water_height(x, z, only_z, o);
            NF_CHECK_NEAR(sum, parts, 1e-5f);
        }
    }
}

NF_TEST(direction_need_not_be_a_unit_vector) {
    std::vector<Wave> unit = flat_wave();
    std::vector<Wave> scaled = unit;
    scaled[0].direction = Vec2{2.0f, 0.0f};
    WaterOptions o = options();
    o.time = 1.1f; // nonzero, so a phase term the normalisation affects is live
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        NF_CHECK_NEAR(water_height(x, 2.0f, unit, o), water_height(x, 2.0f, scaled, o),
                      1e-6f);
    }
}

NF_TEST(the_crest_travels_at_the_stated_speed) {
    const std::vector<Wave> waves = flat_wave();
    WaterOptions at_t = options();
    WaterOptions at_t_plus_dt = options();
    at_t.time = 0.0f;
    at_t_plus_dt.time = 1.0f;
    const f32 speed = waves[0].speed;
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        // The field one second later is the field now, carried `speed` units
        // along the direction.
        NF_CHECK_NEAR(water_height(x - speed, 0.0f, waves, at_t),
                      water_height(x, 0.0f, waves, at_t_plus_dt), 1e-5f);
    }
}

NF_TEST(steepness_above_one_is_clamped_not_self_intersecting) {
    // Unclamped, sharp = steepness·amplitude·k would be ~3.9 and the surface
    // would loop over itself; the field must refuse to draw that.
    std::vector<Wave> legal = flat_wave();
    legal[0].steepness = 1.0f;
    legal[0].wavelength = 4.0f;
    std::vector<Wave> absurd = legal;
    absurd[0].steepness = 5.0f;
    const WaterOptions o = options();
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        NF_CHECK_NEAR(water_height(x, 0.0f, legal, o), water_height(x, 0.0f, absurd, o),
                      1e-6f);
        NF_CHECK(same_vec2(water_displacement(x, 0.0f, legal, o),
                           water_displacement(x, 0.0f, absurd, o), 1e-6f));
    }
}

// ---------------------------------------------------------------------------
// Displacement and the normal
// ---------------------------------------------------------------------------

NF_TEST(a_flat_wave_does_not_displace_horizontally) {
    const std::vector<Wave> waves = flat_wave();
    const WaterOptions o = options();
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        const Vec2 d = water_displacement(x, 3.0f, waves, o);
        NF_CHECK(std::abs(d.x) < 1e-6f);
        NF_CHECK(std::abs(d.y) < 1e-6f);
    }
}

NF_TEST(gerstner_displacement_vanishes_at_the_extremes_and_peaks_between) {
    const std::vector<Wave> waves = gerstner_wave();
    const WaterOptions o = options();
    const Vec2 at_crest = water_displacement(4.0f, 0.0f, waves, o);
    const Vec2 at_trough = water_displacement(12.0f, 0.0f, waves, o);
    const Vec2 at_rise = water_displacement(0.0f, 0.0f, waves, o);
    NF_CHECK(at_crest.length() < 1e-6f);
    NF_CHECK(at_trough.length() < 1e-6f);
    // The orbital motion is maximal where the surface is level and steepest,
    // and it points along the wave's direction — the two halves of the trochoid
    // converge on the crest from either side, which is what narrows it.
    NF_CHECK(at_rise.length() > 0.2f);
    NF_CHECK_NEAR(at_rise.x, waves[0].steepness * waves[0].amplitude, 1e-6f);
    NF_CHECK_NEAR(at_rise.y, 0.0f, 1e-6f);
}

NF_TEST(a_still_surface_has_an_up_normal) {
    const std::vector<Wave> waves;
    const WaterOptions o = options();
    const Vec3 n = water_normal(3.0f, 5.0f, waves, o);
    NF_CHECK(n.nearly_equals(Vec3{0.0f, 1.0f, 0.0f}));
}

NF_TEST(normals_are_unit_and_tilt_with_the_face) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.time = 0.9f;
    bool tilted = false;
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        const Vec3 n = water_normal(x, 1.0f, waves, o);
        NF_CHECK_NEAR(n.length(), 1.0f, 1e-5f);
        NF_CHECK(n.y > 0.0f); // a legal Gerstner sum never folds over
        if (n.y < 0.999f) tilted = true;
    }
    NF_CHECK(tilted);
}

NF_TEST(the_analytic_normal_is_the_height_fields_normal) {
    // With steepness 0 the surface *is* the height field over the rest grid, so
    // the closed-form Jacobian must agree with central differences of the same
    // height function to the difference scheme's order — this is what makes a
    // vertex normal and a physics query read the same surface.
    std::vector<Wave> waves;
    {
        Wave w1 = flat_wave()[0];
        w1.direction = Vec2{0.866f, 0.5f};
        Wave w2 = w1;
        w2.direction = Vec2{-0.5f, 0.866f};
        w2.wavelength = 11.0f;
        w2.amplitude = 0.3f;
        waves = {w1, w2};
    }
    WaterOptions o = options();
    o.time = 0.4f;
    const f32 eps = 0.01f;
    for (u32 i = 0u; i <= 16u; ++i) {
        for (u32 j = 0u; j <= 16u; ++j) {
            const f32 x = -8.0f + static_cast<f32>(i) * 1.0f;
            const f32 z = -8.0f + static_cast<f32>(j) * 1.0f;
            const f32 hx = water_height(x + eps, z, waves, o) -
                           water_height(x - eps, z, waves, o);
            const f32 hz = water_height(x, z + eps, waves, o) -
                           water_height(x, z - eps, waves, o);
            const Vec3 fd = Vec3{-hx / (2.0f * eps), 1.0f, -hz / (2.0f * eps)}.normalized();
            NF_CHECK(water_normal(x, z, waves, o).nearly_equals(fd, 1e-4f));
        }
    }
}

// ---------------------------------------------------------------------------
// Foam
// ---------------------------------------------------------------------------

NF_TEST(foam_sits_on_the_crest_not_the_trough) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.foam_steepness = 0.5f;
    o.foam_softness = 0.5f;
    const ShoreSampler shore;
    const f32 at_crest = water_foam(4.0f, 0.0f, waves, o, shore);
    const f32 at_trough = water_foam(12.0f, 0.0f, waves, o, shore);
    NF_CHECK(at_crest > 0.5f);
    NF_CHECK(at_trough < 1e-6f);
}

NF_TEST(foam_amount_zero_is_glass) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.foam_amount = 0.0f;
    const ShoreSampler shore = constant_shore(2.0f);
    for (u32 i = 0u; i <= 40u; ++i) {
        const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
        NF_CHECK(water_foam(x, 0.0f, waves, o, shore) < 1e-6f);
    }
}

NF_TEST(no_waves_is_no_foam) {
    const std::vector<Wave> waves;
    const WaterOptions o = options();
    const ShoreSampler shore = constant_shore(2.0f);
    NF_CHECK(water_foam(3.0f, 4.0f, waves, o, shore) < 1e-6f);
}

NF_TEST(surf_peaks_at_the_break_depth_and_is_gone_in_deep_water) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.foam_steepness = 0.99f; // crests rare enough that surf is what we measure
    const f32 at_trough = 12.0f; // crest01 == 0 here, so crest foam is zero
    const ShoreSampler breaking = constant_shore(2.0f); // == break depth
    const ShoreSampler deep = constant_shore(100.0f);
    const ShoreSampler dry = constant_shore(0.0f);
    const ShoreSampler none;
    NF_CHECK(water_foam(at_trough, 0.0f, waves, o, breaking) > 0.5f);
    // The deep floor is the open ocean and the dry floor is the beach: neither
    // throws surf at this depth band.
    NF_CHECK(water_foam(at_trough, 0.0f, waves, o, deep) < 1e-6f);
    NF_CHECK(water_foam(at_trough, 0.0f, waves, o, dry) < 1e-6f);
    NF_CHECK(water_foam(at_trough, 0.0f, waves, o, none) < 1e-6f);
}

NF_TEST(the_seed_moves_the_foam_not_the_amount) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions a = options();
    a.foam_steepness = 0.5f;
    a.foam_softness = 0.5f;
    a.time = 0.7f;
    WaterOptions b = a;
    b.seed = 42u;
    const ShoreSampler shore;
    f32 sum_a = 0.0f, sum_b = 0.0f;
    u32 differing = 0u, samples = 0u;
    for (u32 i = 0u; i < 40u; ++i) {
        for (u32 j = 0u; j < 40u; ++j) {
            const f32 x = -50.0f + static_cast<f32>(i) * 2.5f;
            const f32 z = -50.0f + static_cast<f32>(j) * 2.5f;
            const f32 fa = water_foam(x, z, waves, a, shore);
            const f32 fb = water_foam(x, z, waves, b, shore);
            sum_a += fa;
            sum_b += fb;
            if (std::abs(fa - fb) > 1e-6f) ++differing;
            ++samples;
        }
    }
    NF_CHECK(differing > 0u); // the texture actually varies
    NF_CHECK(samples == 1600u);
    // Same total, different places: the multiplier averages to 1 over the
    // lattice, so the seed patches the foam without adding or removing it.
    const f32 mean_a = sum_a / static_cast<f32>(samples);
    const f32 mean_b = sum_b / static_cast<f32>(samples);
    NF_CHECK(mean_a > 0.05f);
    NF_CHECK(std::abs(mean_a - mean_b) < 0.3f * mean_a);
}

// ---------------------------------------------------------------------------
// The cheap reflection
// ---------------------------------------------------------------------------

NF_TEST(reflection_is_a_mirror_at_grazing_and_dark_from_overhead) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.fresnel_normal = 0.02f;
    o.reflection_strength = 1.0f;
    const f32 crest_x = 4.0f; // the tilt is exactly zero at the crest
    NF_CHECK(water_normal(crest_x, 0.0f, waves, o).nearly_equals(Vec3{0.0f, 1.0f, 0.0f}));
    NF_CHECK_NEAR(water_reflection(crest_x, 0.0f, 0.0f, waves, o), 1.0f, 1e-5f);
    NF_CHECK_NEAR(water_reflection(crest_x, 0.0f, nf::HALF_PI, waves, o), 0.02f, 1e-5f);
    const f32 at_45 = water_reflection(crest_x, 0.0f, nf::HALF_PI * 0.25f, waves, o);
    const f32 expected = 0.02f + 0.98f * static_cast<f32>(std::pow(
                                        1.0 - std::sin(static_cast<double>(nf::HALF_PI) * 0.25),
                                        5.0));
    NF_CHECK_NEAR(at_45, expected, 1e-5f);
}

NF_TEST(reflection_falls_off_monotonically_and_scales_with_strength) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    const f32 crest_x = 4.0f;
    const f32 r0 = water_reflection(crest_x, 0.0f, 0.0f, waves, o);
    const f32 r1 = water_reflection(crest_x, 0.0f, nf::HALF_PI * 0.25f, waves, o);
    const f32 r2 = water_reflection(crest_x, 0.0f, nf::HALF_PI, waves, o);
    NF_CHECK(r0 > r1);
    NF_CHECK(r1 > r2);
    WaterOptions half = o;
    half.reflection_strength = 0.5f;
    NF_CHECK_NEAR(water_reflection(crest_x, 0.0f, 0.0f, waves, half), 0.5f, 1e-5f);
    NF_CHECK(water_reflection(crest_x, 0.0f, nf::HALF_PI, waves, half) <= 0.5f);
}

NF_TEST(reflection_stays_in_range_on_a_choppy_surface) {
    std::vector<Wave> waves;
    {
        Wave w = flat_wave()[0];
        w.steepness = 1.0f;
        w.wavelength = 4.0f;
        waves = {w, w};
    }
    waves[1].direction = Vec2{0.0f, 1.0f};
    const WaterOptions o = options();
    for (u32 i = 0u; i <= 40u; ++i) {
        for (u32 j = 0u; j <= 4u; ++j) {
            const f32 x = -20.0f + static_cast<f32>(i) * 1.0f;
            const f32 elevation = static_cast<f32>(j) * (nf::HALF_PI * 0.25f);
            const f32 r = water_reflection(x, 3.0f, elevation, waves, o);
            NF_CHECK(r >= 0.0f && r <= 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Surge
// ---------------------------------------------------------------------------

NF_TEST(a_balanced_wave_set_sits_about_half_above_rest) {
    const std::vector<Wave> waves = flat_wave();
    const WaterOptions o = options();
    const f32 surge = water_surge(waves, o);
    NF_CHECK(surge > 0.3f);
    NF_CHECK(surge < 0.7f);
}

NF_TEST(surge_is_deterministic_and_zero_without_waves) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.time = 2.5f;
    NF_CHECK_EQ(water_surge(waves, o), water_surge(waves, o));
    NF_CHECK(water_surge({}, o) < 1e-6f);
}

// ---------------------------------------------------------------------------
// The shore sampler
// ---------------------------------------------------------------------------

NF_TEST(constant_shore_reports_one_depth_everywhere) {
    const ShoreSampler shore = constant_shore(3.5f);
    NF_CHECK_NEAR(shore(-100.0f, -100.0f), 3.5f, 1e-6f);
    NF_CHECK_NEAR(shore(0.0f, 0.0f), 3.5f, 1e-6f);
    NF_CHECK_NEAR(shore(77.0f, -13.0f), 3.5f, 1e-6f);
}

// ---------------------------------------------------------------------------
// The mesh
// ---------------------------------------------------------------------------

NF_TEST(the_builder_rejects_degenerate_options) {
    const std::vector<Wave> waves = gerstner_wave();
    const ShoreSampler shore = constant_shore(2.0f);
    WaterOptions o = options();
    NF_CHECK(build_water_mesh(waves, o, shore) != nullptr);
    WaterOptions res1 = o;
    res1.resolution = 1u;
    NF_CHECK(build_water_mesh(waves, res1, shore) == nullptr);
    WaterOptions zero_size = o;
    zero_size.size = 0.0f;
    NF_CHECK(build_water_mesh(waves, zero_size, shore) == nullptr);
    WaterOptions neg_size = o;
    neg_size.size = -10.0f;
    NF_CHECK(build_water_mesh(waves, neg_size, shore) == nullptr);
    NF_CHECK(build_water_mesh({}, o, shore) == nullptr); // no waves is a plane
    // Waves that cannot move the surface either.
    std::vector<Wave> dead = waves;
    dead[0].amplitude = 0.0f;
    NF_CHECK(build_water_mesh(dead, o, shore) == nullptr);
}

NF_TEST(the_grid_is_indexed_and_bounded) {
    const std::vector<Wave> waves = gerstner_wave();
    const ShoreSampler shore = constant_shore(2.0f);
    WaterOptions o = options();
    o.resolution = 8u;
    o.size = 100.0f;
    WaterMeshStats stats;
    std::unique_ptr<StaticMesh> mesh = build_water_mesh(waves, o, shore, &stats);
    NF_CHECK(mesh != nullptr);
    const auto& lod = mesh->lod(0);
    NF_CHECK_EQ(stats.vertices, 64u);
    NF_CHECK_EQ(stats.indices, 294u); // 7*7 cells * 6
    NF_CHECK_EQ(lod.vertices.size(), 64u);
    NF_CHECK_EQ(lod.indices.size(), 294u);
    NF_CHECK_EQ(lod.submeshes.size(), 1u);
    NF_CHECK_EQ(lod.submeshes[0].index_count, 294u);
    NF_CHECK_EQ(lod.material_slots.size(), 1u);
    NF_CHECK(lod.bounds.max_y > lod.bounds.min_y); // a degenerate AABB
    // The box is absolute over the displaced vertices, and a Gerstner sum
    // cannot move the surface further than its own amplitudes: the trough sits
    // as far below the rest level as the crest does above it.
    NF_CHECK(lod.bounds.min_y < o.level);
    NF_CHECK(lod.bounds.max_y > o.level);
    NF_CHECK(lod.bounds.min_y >= o.level - waves[0].amplitude - 1e-4f);
    NF_CHECK(lod.bounds.max_y <= o.level + waves[0].amplitude + 1e-4f);

    // Lifting the rest level lifts the whole box by the same amount: bounds
    // follow the surface, they are not a separate shape.
    o.level = 12.0f;
    std::unique_ptr<StaticMesh> lifted = build_water_mesh(waves, o, shore);
    NF_CHECK(lifted != nullptr);
    const auto& lb = lifted->lod(0).bounds;
    NF_CHECK(lb.min_y > 12.0f - waves[0].amplitude - 1e-4f);
    NF_CHECK(lb.max_y > 12.0f);
    NF_CHECK_NEAR(lb.max_y - lb.min_y, lod.bounds.max_y - lod.bounds.min_y, 1e-4f);
    for (const auto& v : lod.vertices) {
        NF_CHECK(v.position[0] >= -50.0f - 1.0f && v.position[0] <= 50.0f + 1.0f);
        NF_CHECK(v.position[2] >= -50.0f - 1.0f && v.position[2] <= 50.0f + 1.0f);
    }
}

NF_TEST(the_windings_face_up) {
    // One back-face cull state serves the terrain and the water, so the grid's
    // triangles must wind counter-clockwise as seen from +Y — checked on the
    // geometry rather than assumed from the index pattern.
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.resolution = 8u;
    const std::unique_ptr<StaticMesh> mesh = build_water_mesh(waves, o, constant_shore(2.0f));
    NF_CHECK(mesh != nullptr);
    const auto& lod = mesh->lod(0);
    const auto& v = lod.vertices;
    for (std::size_t t = 0u; t < lod.indices.size(); t += 3u) {
        const Vec3 a = Vec3{v[lod.indices[t]].position[0], v[lod.indices[t]].position[1],
                            v[lod.indices[t]].position[2]};
        const Vec3 b = Vec3{v[lod.indices[t + 1]].position[0], v[lod.indices[t + 1]].position[1],
                            v[lod.indices[t + 1]].position[2]};
        const Vec3 c = Vec3{v[lod.indices[t + 2]].position[0], v[lod.indices[t + 2]].position[1],
                            v[lod.indices[t + 2]].position[2]};
        NF_CHECK((b - a).cross(c - a).y > 0.0f);
    }
}

NF_TEST(the_baked_grid_is_the_cpu_field) {
    // The contract: a vertex sits where the surface point *went* (rest position
    // plus the horizontal displacement, height on top), its normal is the
    // field's, and its two extra channels are the same functions a live query
    // returns. A buoy and the shader then read the same water.
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.resolution = 9u;
    o.size = 32.0f;
    o.time = 0.6f;
    const ShoreSampler shore = constant_shore(2.0f);
    WaterMeshStats stats;
    const std::unique_ptr<StaticMesh> mesh = build_water_mesh(waves, o, shore, &stats);
    NF_CHECK(mesh != nullptr);
    const auto& lod = mesh->lod(0);
    const f32 step = o.size / static_cast<f32>(o.resolution - 1u);
    const f32 half = o.size * 0.5f;
    for (u32 j = 0u; j < o.resolution; ++j) {
        for (u32 i = 0u; i < o.resolution; ++i) {
            const u32 idx = j * o.resolution + i;
            const f32 x = -half + static_cast<f32>(i) * step;
            const f32 z = -half + static_cast<f32>(j) * step;
            const auto& v = lod.vertices[idx];
            const f32 h = water_height(x, z, waves, o);
            const Vec2 d = water_displacement(x, z, waves, o);
            NF_CHECK_NEAR(v.position[0], x + d.x, 1e-5f);
            NF_CHECK_NEAR(v.position[1], o.level + h, 1e-5f);
            NF_CHECK_NEAR(v.position[2], z + d.y, 1e-5f);
            const Vec3 grid_normal{v.normal[0], v.normal[1], v.normal[2]};
            NF_CHECK(grid_normal.nearly_equals(water_normal(x, z, waves, o), 1e-5f));
            NF_CHECK_NEAR(v.uv0[0], x, 1e-6f); // world coordinates, not grid fractions
            NF_CHECK_NEAR(v.uv0[1], z, 1e-6f);
            NF_CHECK_NEAR(v.uv1[0], water_foam(x, z, waves, o, shore), 1e-6f);
            NF_CHECK(v.uv1[0] >= 0.0f && v.uv1[0] <= 1.0f);
            NF_CHECK(v.uv1[1] >= 0.0f && v.uv1[1] <= 1.0f);
        }
    }
    NF_CHECK(stats.max_height > 0.35f); // the grid actually caught a crest
    NF_CHECK(stats.max_height <= waves[0].amplitude + 1e-5f);
    NF_CHECK(stats.max_foam > 0.0f);
}

NF_TEST(the_grid_builds_without_a_shore) {
    const std::vector<Wave> waves = gerstner_wave();
    const WaterOptions o = options();
    const ShoreSampler shore; // empty: open ocean, surf nowhere
    WaterMeshStats stats;
    const std::unique_ptr<StaticMesh> mesh = build_water_mesh(waves, o, shore, &stats);
    NF_CHECK(mesh != nullptr);
    NF_CHECK_EQ(stats.vertices, o.resolution * o.resolution);
    NF_CHECK_EQ(stats.indices, (o.resolution - 1u) * (o.resolution - 1u) * 6u);
    for (const auto& v : mesh->lod(0).vertices) {
        NF_CHECK(v.uv1[0] >= 0.0f && v.uv1[0] <= 1.0f);
    }
}

NF_TEST(an_absurd_resolution_is_clamped) {
    const std::vector<Wave> waves = gerstner_wave();
    WaterOptions o = options();
    o.resolution = 5000u; // a caller asking for 25M vertices
    const std::unique_ptr<StaticMesh> mesh = build_water_mesh(waves, o, constant_shore(2.0f));
    NF_CHECK(mesh != nullptr);
    NF_CHECK_EQ(mesh->lod(0).vertices.size(), 1024u * 1024u);
}
