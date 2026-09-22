// Tests/FoliageTests/test_scatter.cpp — the foliage scatter's invariants.
//
// The counts here are EXACT where the scatter is exact: at weight 1.0 with no
// mask and no cull every candidate cell emits, and the cell grid is
// integer-indexed, so the count is the cell count. Where the scatter is
// probabilistic (mask, fade ring) the test asserts the distribution instead —
// the point is that a mask halves a field and a fade thins it, not that any
// particular hash draw lands.

#include <NF/Foliage/Foliage.hpp>
#include <NF/Test/TestFramework.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace {

using nf::f32;
using nf::u32;
using nf::u64;
using nf::Vec3;
using nf::foliage::DensityMask;
using nf::foliage::FoliageInstance;
using nf::foliage::FoliageType;
using nf::foliage::GroundSample;
using nf::foliage::GroundSampler;
using nf::foliage::ScatterOptions;
using nf::foliage::ScatterStats;
using nf::foliage::WindState;
using nf::foliage::scatter_foliage;
using nf::foliage::wind_displacement;

/// Bit-for-bit instance comparison. `FoliageInstance` has no operator== on
/// purpose — it is a GPU buffer element, and the float members make a defaulted
/// comparison read as a bug — but the determinism test needs *exact* equality,
/// not a tolerance, so it is spelled out here instead.
bool same_instance(const FoliageInstance& a, const FoliageInstance& b) {
    return a.position[0u] == b.position[0u] && a.position[1u] == b.position[1u] &&
           a.position[2u] == b.position[2u] && a.scale == b.scale && a.yaw == b.yaw &&
           a.type_index == b.type_index && a.lod == b.lod && a.wind_phase == b.wind_phase;
}

bool same_instances(const std::vector<FoliageInstance>& a,
                    const std::vector<FoliageInstance>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0u; i < a.size(); ++i) {
        if (!same_instance(a[i], b[i])) return false;
    }
    return true;
}

/// Flat ground, no slope — the field everything counts on.
GroundSampler flat_ground() {
    return [](f32, f32) { return GroundSample{0.0f, 0.0f}; };
}

/// One species covering the whole rect at density 0.25: the cell side is 2.0
/// world units, so the 20x20 rect is exactly 10x10 cells and the expected count
/// is exactly 100.
FoliageType field_type() {
    FoliageType t;
    t.mesh = "grass";
    t.density = 0.25f;
    return t;
}

ScatterOptions field_options() {
    ScatterOptions o;
    o.seed = 7u;
    return o;
}

/// The cells a rect covers, recovered from each instance's position: the
/// jitter keeps a candidate inside its own cell, so `floor(x / cell)` is the
/// cell the scatter visited. This is what makes the sub-rect test possible —
/// the comparison key is the cell, not the (jittered) position.
struct CellKey {
    u32 type_index;
    nf::i64 cx;
    nf::i64 cz;
    bool operator<(const CellKey& o) const {
        return std::tie(type_index, cx, cz) < std::tie(o.type_index, o.cx, o.cz);
    }
};

std::map<CellKey, FoliageInstance> by_cell(const std::vector<FoliageInstance>& instances,
                                           f32 cell) {
    std::map<CellKey, FoliageInstance> out;
    for (const FoliageInstance& i : instances) {
        const CellKey k{i.type_index,
                        static_cast<nf::i64>(std::floor(i.position[0u] / cell)),
                        static_cast<nf::i64>(std::floor(i.position[2u] / cell))};
        out.emplace(k, i);
    }
    return out;
}

} // namespace

NF_TEST(scatter_count_matches_density_times_area) {
    const FoliageType t = field_type();
    const std::vector<FoliageType> types{t};
    const std::vector<FoliageInstance> out =
        scatter_foliage(types, flat_ground(), DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f,
                        field_options());
    // 10x10 cells, every one accepted at weight 1.0.
    NF_CHECK_EQ(out.size(), 100u);
}

NF_TEST(scatter_is_deterministic_for_the_same_seed) {
    const std::vector<FoliageType> types{field_type()};
    const ScatterOptions o = field_options();
    const std::vector<FoliageInstance> a = scatter_foliage(types, flat_ground(), DensityMask(),
                                                           0.0f, 0.0f, 20.0f, 20.0f, o);
    const std::vector<FoliageInstance> b = scatter_foliage(types, flat_ground(), DensityMask(),
                                                           0.0f, 0.0f, 20.0f, 20.0f, o);
    NF_CHECK_EQ(a.size(), b.size());
    // Bit-for-bit: the hash has no state and no float input.
    NF_CHECK(same_instances(a, b));
}

NF_TEST(scatter_differs_for_a_different_seed) {
    const std::vector<FoliageType> types{field_type()};
    ScatterOptions o = field_options();
    const std::vector<FoliageInstance> a = scatter_foliage(types, flat_ground(), DensityMask(),
                                                           0.0f, 0.0f, 20.0f, 20.0f, o);
    o.seed = 8u;
    const std::vector<FoliageInstance> b = scatter_foliage(types, flat_ground(), DensityMask(),
                                                           0.0f, 0.0f, 20.0f, 20.0f, o);
    NF_CHECK_EQ(a.size(), b.size());
    NF_CHECK(!same_instances(a, b));
}

NF_TEST(sub_rect_reproduces_the_full_bake_cell_for_cell) {
    // The streaming invariant (design doc 60): a chunk scattered on its own is
    // identical to the same chunk scattered as part of a larger rect. A shared
    // cell must answer the same, or a player walking into a tile would watch
    // the grass rearrange.
    const std::vector<FoliageType> types{field_type()};
    const f32 cell = 2.0f; // 1 / sqrt(0.25)
    const std::vector<FoliageInstance> full =
        scatter_foliage(types, flat_ground(), DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f,
                        field_options());
    const std::vector<FoliageInstance> sub =
        scatter_foliage(types, flat_ground(), DensityMask(), 4.0f, 4.0f, 12.0f, 12.0f,
                        field_options());
    const std::map<CellKey, FoliageInstance> full_map = by_cell(full, cell);

    NF_CHECK_EQ(sub.size(), 16u); // 4x4 cells in [4, 12)
    for (const FoliageInstance& i : sub) {
        const CellKey k{i.type_index,
                        static_cast<nf::i64>(std::floor(i.position[0u] / cell)),
                        static_cast<nf::i64>(std::floor(i.position[2u] / cell))};
        const auto it = full_map.find(k);
        NF_CHECK(it != full_map.end());
        if (it == full_map.end()) continue;
        // Every random draw the cell made agrees — not just the position, the
        // scale, the yaw, and the phase the wind reads.
        NF_CHECK_EQ(it->second.scale, i.scale);
        NF_CHECK_EQ(it->second.yaw, i.yaw);
        NF_CHECK_EQ(it->second.wind_phase, i.wind_phase);
        NF_CHECK_EQ(it->second.position[0u], i.position[0u]);
        NF_CHECK_EQ(it->second.position[2u], i.position[2u]);
    }
}

NF_TEST(instances_stand_on_the_ground_they_passed) {
    // A sloped ground: the instance's Y is the sampled height, so a field on a
    // ramp follows the ramp rather than floating through it.
    const std::vector<FoliageType> types{field_type()};
    const GroundSampler ramp = [](f32, f32 z) { return GroundSample{z * 0.5f, 0.0f}; };
    const std::vector<FoliageInstance> out =
        scatter_foliage(types, ramp, DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f, field_options());
    NF_CHECK(!out.empty());
    for (const FoliageInstance& i : out) {
        NF_CHECK_NEAR(i.position[1u], i.position[2u] * 0.5f, 1e-5f);
    }
}

NF_TEST(height_gate_keeps_only_the_band) {
    // Ground rises with Z; the type owns [0, 4]. Of the ten Z-cells (side 2),
    // only cz 0 ([0,2)) and cz 1 ([2,4)) pass — ten X-cells each.
    FoliageType t = field_type();
    t.min_height = 0.0f;
    t.max_height = 4.0f;
    const GroundSampler ramp = [](f32, f32 z) { return GroundSample{z, 0.0f}; };
    const std::vector<FoliageInstance> out =
        scatter_foliage({t}, ramp, DensityMask(), 0.0f, -10.0f, 20.0f, 10.0f, field_options());
    NF_CHECK_EQ(out.size(), 20u);
    for (const FoliageInstance& i : out) {
        NF_CHECK(i.position[2u] >= 0.0f && i.position[2u] < 4.0f);
    }
}

NF_TEST(slope_gate_rejects_the_steep_half) {
    // The sampler is a cliff for x > 0 and flat for x <= 0. A candidate in cell
    // cx >= 0 can never land left of the boundary, so the count is the negative
    // half of the 10x10 cell grid.
    FoliageType t = field_type();
    t.max_slope = 0.1f;
    const GroundSampler cliff = [](f32 x, f32) {
        return GroundSample{0.0f, x > 0.0f ? nf::HALF_PI : 0.0f};
    };
    const std::vector<FoliageInstance> out =
        scatter_foliage({t}, cliff, DensityMask(), -10.0f, -10.0f, 10.0f, 10.0f,
                        field_options());
    NF_CHECK_EQ(out.size(), 50u);
    for (const FoliageInstance& i : out) {
        NF_CHECK(i.position[0u] < 0.0f);
    }
}

NF_TEST(weight_scales_the_count_and_mask_scales_it_again) {
    const std::vector<FoliageType> base{field_type()};
    const ScatterOptions o = field_options();

    // Weight 0.25 keeps roughly a quarter: the exact draw is hash-dependent, so
    // the test is the distribution, and 100 cells at 0.25 is comfortably inside
    // [15, 35] for any seed that is not pathological.
    FoliageType light = field_type();
    light.weight = 0.25f;
    const std::vector<FoliageInstance> w =
        scatter_foliage({light}, flat_ground(), DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f, o);
    NF_CHECK(w.size() >= 15u && w.size() <= 35u);

    // The mask multiplies the weight: a half mask halves the full field.
    const DensityMask half = [](f32, f32) { return 0.5f; };
    const std::vector<FoliageInstance> m =
        scatter_foliage(base, flat_ground(), half, 0.0f, 0.0f, 20.0f, 20.0f, o);
    NF_CHECK(m.size() >= 35u && m.size() <= 65u);

    // A zero mask rejects everything — the artist painted nothing here.
    const DensityMask none = [](f32, f32) { return 0.0f; };
    const std::vector<FoliageInstance> z =
        scatter_foliage(base, flat_ground(), none, 0.0f, 0.0f, 20.0f, 20.0f, o);
    NF_CHECK(z.empty());
}

NF_TEST(mask_is_sampled_per_position_not_per_cell) {
    // Two halves of the rect carry different masks; the count follows the mask,
    // not a single draw for the whole field.
    const DensityMask striped = [](f32 x, f32) { return x < 10.0f ? 1.0f : 0.0f; };
    const std::vector<FoliageInstance> out =
        scatter_foliage({field_type()}, flat_ground(), striped, 0.0f, 0.0f, 20.0f, 20.0f,
                        field_options());
    for (const FoliageInstance& i : out) {
        NF_CHECK(i.position[0u] < 10.0f);
    }
    NF_CHECK_EQ(out.size(), 50u);
}

NF_TEST(scale_and_yaw_stay_inside_their_ranges) {
    FoliageType t = field_type();
    t.min_scale = 0.5f;
    t.max_scale = 2.0f;
    t.yaw_spread = nf::HALF_PI;
    const std::vector<FoliageInstance> out =
        scatter_foliage({t}, flat_ground(), DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f,
                        field_options());
    NF_CHECK(!out.empty());
    bool saw_low = false;
    bool saw_high = false;
    for (const FoliageInstance& i : out) {
        NF_CHECK(i.scale >= 0.5f && i.scale <= 2.0f);
        NF_CHECK(i.yaw >= -nf::HALF_PI * 0.5f && i.yaw <= nf::HALF_PI * 0.5f);
        NF_CHECK(i.wind_phase >= 0.0f && i.wind_phase < 1.0f);
        if (i.scale < 0.9f) saw_low = true;
        if (i.scale > 1.6f) saw_high = true;
    }
    // The range is actually used: a scale draw that never leaves the middle
    // would mean the min/max are decorative.
    NF_CHECK(saw_low);
    NF_CHECK(saw_high);
}

NF_TEST(degenerate_types_and_rects_emit_nothing) {
    const GroundSampler g = flat_ground();
    const ScatterOptions o = field_options();

    FoliageType zero_density = field_type();
    zero_density.density = 0.0f;
    NF_CHECK(scatter_foliage({zero_density}, g, DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f, o).empty());

    FoliageType zero_weight = field_type();
    zero_weight.weight = 0.0f;
    NF_CHECK(scatter_foliage({zero_weight}, g, DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f, o).empty());

    FoliageType inverted_scale = field_type();
    inverted_scale.min_scale = 2.0f;
    inverted_scale.max_scale = 1.0f;
    NF_CHECK(scatter_foliage({inverted_scale}, g, DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f, o)
                 .empty());

    // A rect with no area has no cells.
    NF_CHECK(scatter_foliage({field_type()}, g, DensityMask(), 10.0f, 0.0f, 5.0f, 20.0f, o)
                 .empty());
    NF_CHECK(scatter_foliage(std::vector<FoliageType>{}, g, DensityMask(), 0.0f, 0.0f, 20.0f,
                             20.0f, o)
                 .empty());
}

NF_TEST(stats_count_accepts_rejects_and_culls) {
    const FoliageType t = field_type();
    const GroundSampler cliff = [](f32 x, f32) {
        return GroundSample{0.0f, x > 0.0f ? nf::HALF_PI : 0.0f};
    };
    FoliageType gated = t;
    gated.max_slope = 0.1f;
    ScatterStats stats{};
    const std::vector<FoliageInstance> out =
        scatter_foliage({gated}, cliff, DensityMask(), -10.0f, -10.0f, 10.0f, 10.0f,
                        field_options(), &stats);
    // 100 cells visited, 50 rejected by the slope gate, 50 placed — the budget
    // explains a sparse field instead of leaving it a mystery.
    NF_CHECK_EQ(stats.candidates, 100u);
    NF_CHECK_EQ(stats.rejected, 50u);
    NF_CHECK_EQ(stats.placed, 50u);
    NF_CHECK_EQ(stats.culled, 0u);
    NF_CHECK_EQ(out.size(), stats.placed);
}

NF_TEST(lod_buckets_follow_distance_from_the_centre) {
    FoliageType t = field_type();
    const std::vector<FoliageType> types{t};
    ScatterOptions o = field_options();
    o.center_x = 0.0f;
    o.center_z = 0.0f;
    o.lod_distances[0u] = 5.0f;
    o.lod_distances[1u] = 15.0f;
    o.lod_distances[2u] = 40.0f;
    o.lod_distances[3u] = 100.0f;
    // No cull, so the bucket is reported even in the outermost ring.
    o.cull_radius = 0.0f;
    const std::vector<FoliageInstance> out =
        scatter_foliage(types, flat_ground(), DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f, o);
    NF_CHECK(!out.empty());
    bool seen[4] = {false, false, false, false};
    for (const FoliageInstance& i : out) {
        const f32 dx = i.position[0u] - o.center_x;
        const f32 dz = i.position[2u] - o.center_z;
        const f32 dist = std::sqrt(dx * dx + dz * dz);
        u32 want = 3u;
        for (u32 b = 0u; b < 4u; ++b) {
            if (dist < o.lod_distances[b]) {
                want = b;
                break;
            }
        }
        NF_CHECK_EQ(i.lod, want);
        seen[want] = true;
    }
    // The field really does cross the first three buckets.
    NF_CHECK(seen[0u]);
    NF_CHECK(seen[1u]);
    NF_CHECK(seen[2u]);
}

NF_TEST(cull_radius_keeps_only_the_near_field) {
    const std::vector<FoliageType> types{field_type()};
    ScatterOptions o = field_options();
    o.center_x = 0.0f;
    o.center_z = 0.0f;
    o.cull_radius = 6.0f;
    const std::vector<FoliageInstance> out =
        scatter_foliage(types, flat_ground(), DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f, o);
    NF_CHECK(!out.empty());
    for (const FoliageInstance& i : out) {
        const f32 dx = i.position[0u] - o.center_x;
        const f32 dz = i.position[2u] - o.center_z;
        const f32 dist = std::sqrt(dx * dx + dz * dz);
        // The fade ring may keep an instance short of the radius but never at
        // or beyond it.
        NF_CHECK(dist < o.cull_radius);
    }
}

NF_TEST(fade_ring_thins_the_distant_field_it_does_not_cut_it) {
    // Two rings of equal area past the last LOD distance: the inner one must
    // carry more instances than the outer one, and the outer one is not empty —
    // a hard cut would zero it.
    const std::vector<FoliageType> types{field_type()};
    ScatterOptions o = field_options();
    o.center_x = 0.0f;
    o.center_z = 0.0f;
    o.lod_distances[0u] = 2.0f;
    o.lod_distances[1u] = 4.0f;
    o.lod_distances[2u] = 6.0f;
    o.lod_distances[3u] = 8.0f;
    o.cull_radius = 48.0f;
    const std::vector<FoliageInstance> out =
        scatter_foliage(types, flat_ground(), DensityMask(), -40.0f, -40.0f, 40.0f, 40.0f, o);
    NF_CHECK(!out.empty());
    u32 inner = 0u;
    u32 outer = 0u;
    for (const FoliageInstance& i : out) {
        const f32 dx = i.position[0u] - o.center_x;
        const f32 dz = i.position[2u] - o.center_z;
        const f32 dist = std::sqrt(dx * dx + dz * dz);
        if (dist > 8.0f && dist < 28.0f) ++inner;
        else if (dist >= 28.0f && dist < 48.0f) ++outer;
    }
    NF_CHECK(inner > outer);
    NF_CHECK(outer > 0u);
}

NF_TEST(fade_thinning_is_independent_of_the_acceptance_draw) {
    // Regression: the fade ring used to reuse the acceptance draw, and a
    // candidate admitted on a low odds had already drawn a low number — so it
    // cleared the `hash < keep` thinning at close to rate 1. A sparse field
    // (low weight or a faint mask) therefore kept a *larger* share of its
    // distant instances than a dense one, which is backwards: the fade is
    // supposed to thin the far field whatever admitted it.
    const std::vector<FoliageType> types{field_type()};
    ScatterOptions o = field_options();
    o.center_x = 0.0f;
    o.center_z = 0.0f;
    o.lod_distances[0u] = 2.0f;
    o.lod_distances[1u] = 4.0f;
    o.lod_distances[2u] = 6.0f;
    o.lod_distances[3u] = 8.0f;
    o.cull_radius = 48.0f;

    // The ring population with no cull at all: every candidate past the last
    // LOD distance, which at weight 1.0 is every cell out there.
    const auto in_ring = [](const FoliageInstance& i) {
        const f32 dx = i.position[0u];
        const f32 dz = i.position[2u];
        const f32 dist = std::sqrt(dx * dx + dz * dz);
        return dist >= 8.0f && dist < 48.0f;
    };

    ScatterOptions unculled = o;
    unculled.cull_radius = 0.0f;
    const std::vector<FoliageInstance> full =
        scatter_foliage(types, flat_ground(), DensityMask(), -100.0f, -100.0f, 100.0f,
                        100.0f, unculled);
    u32 ring = 0u;
    for (const FoliageInstance& i : full) {
        if (in_ring(i)) ++ring;
    }
    NF_CHECK(ring > 1000u); // a big enough sample that the rates below mean it

    // Dense field: keep odds average 0.5 across the ring (1 at the inner edge,
    // 0 at the outer), so roughly half of the ring survives.
    const std::vector<FoliageInstance> dense =
        scatter_foliage(types, flat_ground(), DensityMask(), -100.0f, -100.0f, 100.0f,
                        100.0f, o);
    u32 dense_ring = 0u;
    for (const FoliageInstance& i : dense) {
        if (in_ring(i)) ++dense_ring;
    }
    NF_CHECK(dense_ring > ring * 0.35f);
    NF_CHECK(dense_ring < ring * 0.65f);

    // Sparse field at weight 0.1: one in ten candidates is even admitted, then
    // those survivors must thin at the same rate as the dense field's — about
    // half — giving a twentieth of the ring. If the draws were shared, the
    // admission draw that let one through would also let it through the fade,
    // and the sparse field would keep its own rate of the ring instead.
    FoliageType sparse_t = field_type();
    sparse_t.weight = 0.1f;
    const std::vector<FoliageInstance> sparse =
        scatter_foliage({sparse_t}, flat_ground(), DensityMask(), -100.0f, -100.0f,
                        100.0f, 100.0f, o);
    u32 sparse_ring = 0u;
    for (const FoliageInstance& i : sparse) {
        if (in_ring(i)) ++sparse_ring;
    }
    // 0.1 * 0.5 * ring, with room either way for hash noise but not enough for
    // the correlated-draw behaviour (which lands near 0.1 * ring).
    NF_CHECK(sparse_ring > ring * 0.025f);
    NF_CHECK(sparse_ring < ring * 0.075f);
}

NF_TEST(two_species_intermix_on_shared_ground) {
    // Both types own the whole rect at density 0.25, so the two grids coincide
    // and the weights decide the mix: 0.8 against 0.2 favours the first about
    // four to one. Each type lands on its own cells — the two scatters are
    // independent, so a cell can carry both, either, or neither.
    FoliageType a = field_type();
    a.mesh = "grass";
    a.weight = 0.8f;
    FoliageType b = field_type();
    b.mesh = "rock";
    b.weight = 0.2f;
    const std::vector<FoliageInstance> out =
        scatter_foliage({a, b}, flat_ground(), DensityMask(), 0.0f, 0.0f, 20.0f, 20.0f,
                        field_options());
    u32 na = 0u;
    u32 nb = 0u;
    for (const FoliageInstance& i : out) {
        NF_CHECK(i.type_index <= 1u);
        if (i.type_index == 0u) ++na;
        else ++nb;
    }
    NF_CHECK(na > nb);
    NF_CHECK(na + nb == out.size());
}

NF_TEST(wind_displacement_is_zero_for_a_rigid_species_and_a_dead_gust) {
    FoliageType rigid = field_type();
    rigid.wind_amount = 0.0f;
    FoliageInstance i{};
    i.scale = 1.0f;
    i.wind_phase = 0.3f;
    WindState w;
    w.time = 1.5f;
    w.gust = 1.0f;
    NF_CHECK(wind_displacement(i, rigid, w).nearly_equals(Vec3::zero));

    // The same instance under a live species but no gust: no movement either —
    // "no wind" means the same thing at every query site.
    FoliageType live = rigid;
    live.wind_amount = 1.0f;
    w.gust = 0.0f;
    NF_CHECK(wind_displacement(i, live, w).nearly_equals(Vec3::zero));
}

NF_TEST(wind_displacement_follows_the_direction_and_stays_bounded) {
    FoliageType t = field_type();
    t.wind_amount = 0.4f;
    t.wind_speed = 1.0f;
    FoliageInstance i{};
    i.scale = 1.0f;
    // A phase whose wave is well away from a zero crossing, so the magnitude is
    // clearly nonzero at t = 0.
    i.wind_phase = 0.25f;
    WindState w;
    w.time = 0.0f;
    w.gust = 1.0f;
    w.direction = Vec3{0.0f, 0.0f, 1.0f};

    const Vec3 d = wind_displacement(i, t, w);
    // The direction is the only horizontal component: a gust along +Z does not
    // shove the blade sideways.
    NF_CHECK_NEAR(d.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(d.y, 0.0f, 1e-6f);
    NF_CHECK(d.z > 0.0f);
    // |wave| <= 1.0 and the envelope <= gust, so the displacement can never
    // exceed wind_amount * scale * gust — the wind is a bend, not a launch.
    NF_CHECK(d.length() <= t.wind_amount * i.scale * w.gust + 1e-6f);
}

NF_TEST(wind_displacement_scales_with_the_instance_and_is_pure) {
    FoliageType t = field_type();
    t.wind_amount = 0.4f;
    t.wind_speed = 1.0f;
    WindState w;
    w.time = 0.7f;
    w.gust = 1.0f;
    w.direction = Vec3{1.0f, 0.0f, 0.0f};

    FoliageInstance small{};
    small.scale = 1.0f;
    small.wind_phase = 0.25f;
    FoliageInstance big = small;
    big.scale = 2.0f;

    const Vec3 ds = wind_displacement(small, t, w);
    const Vec3 db = wind_displacement(big, t, w);
    // A taller instance bends further — the mesh's own height is not known
    // here, so scale is the proxy.
    NF_CHECK(db.length() > ds.length());
    NF_CHECK_NEAR(db.length(), ds.length() * 2.0f, 1e-5f);

    // Pure: the same inputs answer the same, at any time and in any order.
    NF_CHECK(wind_displacement(small, t, w).nearly_equals(ds));

    // And time moves it — a still field would mean the wind was decorative.
    WindState later = w;
    later.time = 3.3f;
    NF_CHECK(!wind_displacement(small, t, later).nearly_equals(ds));
}

NF_TEST(instance_struct_is_the_instance_buffer_element) {
    // The shader reads this as an array of floats; a surprise pad would shift
    // every attribute after the first instance.
    NF_CHECK_EQ(sizeof(FoliageInstance), 32u);
    FoliageInstance i{};
    NF_CHECK_EQ(i.type_index, 0u);
    NF_CHECK_EQ(i.lod, 0u);
    NF_CHECK_EQ(i.scale, 1.0f);
    NF_CHECK_EQ(i.wind_phase, 0.0f);
}
