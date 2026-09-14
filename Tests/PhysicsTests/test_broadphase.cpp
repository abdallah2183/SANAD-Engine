// Tests/PhysicsTests/test_broadphase.cpp — spatial grid
//
// The property under test is one-directional and that matters: the broadphase
// must return a SUPERSET of the true overlaps (missing one means objects fall
// through each other) and a SUBSET of all pairs (returning everything would be
// correct but useless). False positives are the narrowphase's problem.
//
// So the check is against brute force — a definition, not a second
// implementation of the grid — on configurations including the case that breaks
// naive grids: two bodies that overlap while sitting in different cells.

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/Broadphase.hpp>

#include <algorithm>
#include <vector>

using namespace nf;
using namespace nf::physics;

namespace {

/// Deterministic LCG. The test must be reproducible: a physics broadphase bug
/// that only shows on one random seed is not a test, it is a lottery.
struct Rng {
    u32 state = 0x12345678u;
    f32 next(f32 lo, f32 hi) {
        state = state * 1664525u + 1013904223u;
        const f32 t = static_cast<f32>((state >> 8) & 0xFFFF) / 65535.0f;
        return lo + t * (hi - lo);
    }
};

bool contains(const std::vector<BroadphasePair>& hay, const BroadphasePair& needle) {
    return std::binary_search(hay.begin(), hay.end(), needle);
}

/// Every true overlap must appear in the candidate list.
bool is_superset_of(const std::vector<BroadphasePair>& candidates,
                    const std::vector<BroadphasePair>& truth) {
    for (const BroadphasePair& p : truth) {
        if (!contains(candidates, p)) return false;
    }
    return true;
}

} // namespace

NF_TEST(broadphase_empty_grid_has_no_pairs) {
    SpatialGrid grid(1.0f);
    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK(pairs.empty());
    NF_CHECK_EQ(grid.bounded_count(), size_t{0});
}

NF_TEST(broadphase_single_body_has_no_pairs) {
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3::zero, Vec3(0.5f)));
    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK(pairs.empty());
    NF_CHECK_EQ(grid.bounded_count(), size_t{1});
}

NF_TEST(broadphase_finds_an_overlapping_pair) {
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3::zero, Vec3(0.5f)));
    grid.insert(1, Aabb::from_centre_half_extents(Vec3(0.25f, 0.0f, 0.0f), Vec3(0.5f)));

    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK_EQ(pairs.size(), size_t{1});
    NF_CHECK_EQ(pairs[0].a, 0u);
    NF_CHECK_EQ(pairs[0].b, 1u);
}

NF_TEST(broadphase_rejects_a_separated_pair) {
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3::zero, Vec3(0.5f)));
    grid.insert(1, Aabb::from_centre_half_extents(Vec3(100.0f, 0.0f, 0.0f), Vec3(0.5f)));

    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK(pairs.empty());
}

NF_TEST(broadphase_finds_a_pair_that_spans_a_cell_boundary) {
    // The classic grid bug: two bodies overlap but sit in different cells, so a
    // grid that only pairs within a cell misses the contact entirely. With a
    // 1 m cell, bodies at x = 0.6 and x = 1.4 are in cells 0 and 1 while
    // overlapping.
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3(0.6f, 0.0f, 0.0f), Vec3(0.5f)));
    grid.insert(1, Aabb::from_centre_half_extents(Vec3(1.4f, 0.0f, 0.0f), Vec3(0.5f)));

    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK_EQ(pairs.size(), size_t{1});
    NF_CHECK(contains(pairs, BroadphasePair{0, 1}));
}

NF_TEST(broadphase_output_is_sorted_and_unique) {
    // A body spanning several cells is registered in each, so the same pair gets
    // emitted from more than one cell. The output must still be a set.
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3::zero, Vec3(2.5f)));  // spans many cells
    grid.insert(1, Aabb::from_centre_half_extents(Vec3(1.0f, 0.0f, 0.0f), Vec3(0.5f)));
    grid.insert(2, Aabb::from_centre_half_extents(Vec3(-1.0f, 0.0f, 0.0f), Vec3(0.5f)));

    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);

    for (size_t i = 1; i < pairs.size(); ++i) {
        NF_CHECK(pairs[i - 1] < pairs[i]); // strictly increasing == sorted + unique
    }
    // And every emitted pair is canonically ordered.
    for (const auto& p : pairs) {
        NF_CHECK(p.a < p.b);
    }
}

NF_TEST(broadphase_matches_brute_force_on_a_regular_grid) {
    // A 5x5x2 lattice with spacing 1.0 and half-extent 0.4: nothing touches, so
    // brute force finds nothing. The candidate list is NOT expected to be empty —
    // the broadphase is allowed false positives, and bodies 1 m apart with 0.4
    // half-extents genuinely share cells. What must hold is that it filters:
    // returning every pair would satisfy the superset contract and still be a
    // useless broadphase.
    std::vector<Aabb> boxes;
    std::vector<u32> indices;
    for (i32 z = 0; z < 2; ++z) {
        for (i32 y = 0; y < 5; ++y) {
            for (i32 x = 0; x < 5; ++x) {
                indices.push_back(static_cast<u32>(boxes.size()));
                boxes.push_back(Aabb::from_centre_half_extents(
                    Vec3(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)),
                    Vec3(0.4f)));
            }
        }
    }

    SpatialGrid grid(1.0f);
    for (size_t i = 0; i < boxes.size(); ++i) grid.insert(static_cast<u32>(i), boxes[i]);

    std::vector<BroadphasePair> candidates, truth;
    grid.pairs(candidates);
    SpatialGrid::brute_force_pairs(boxes, truth);

    NF_CHECK_EQ(truth.size(), size_t{0});
    NF_CHECK(is_superset_of(candidates, truth)); // vacuously true, but stated

    const size_t all_pairs = boxes.size() * (boxes.size() - 1) / 2;
    NF_CHECK(!candidates.empty());          // it does look at neighbours at all
    NF_CHECK(candidates.size() < all_pairs); // and it is not just returning everything
}

NF_TEST(broadphase_matches_brute_force_on_random_configurations) {
    Rng rng;
    for (int trial = 0; trial < 40; ++trial) {
        const int n = 1 + static_cast<int>(rng.next(1.0f, 40.0f));
        const f32 half = rng.next(0.2f, 1.5f);

        std::vector<Aabb> boxes;
        boxes.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const Vec3 c(rng.next(-8.0f, 8.0f), rng.next(-8.0f, 8.0f), rng.next(-8.0f, 8.0f));
            const Vec3 h(half, half, half);
            boxes.push_back(Aabb::from_centre_half_extents(c, h));
        }

        SpatialGrid grid(rng.next(0.5f, 3.0f));
        for (size_t i = 0; i < boxes.size(); ++i) grid.insert(static_cast<u32>(i), boxes[i]);

        std::vector<BroadphasePair> candidates, truth;
        grid.pairs(candidates);
        SpatialGrid::brute_force_pairs(boxes, truth);

        // The contract, in both directions.
        NF_CHECK(is_superset_of(candidates, truth));
        NF_CHECK(std::is_sorted(candidates.begin(), candidates.end()));
        NF_CHECK(std::adjacent_find(candidates.begin(), candidates.end()) == candidates.end());
    }
}

NF_TEST(broadphase_never_misses_an_overlap_when_bodies_share_a_corner) {
    // Degenerate geometry: two boxes touching exactly at one corner. Inclusive
    // overlap is deliberate — a resting contact is exactly this case, and
    // excluding it makes objects sink through the floor.
    std::vector<Aabb> boxes{
        Aabb::from_centre_half_extents(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f)),
        Aabb::from_centre_half_extents(Vec3(2.0f, 2.0f, 2.0f), Vec3(1.0f)),
    };
    SpatialGrid grid(1.0f);
    for (size_t i = 0; i < boxes.size(); ++i) grid.insert(static_cast<u32>(i), boxes[i]);

    std::vector<BroadphasePair> candidates, truth;
    grid.pairs(candidates);
    SpatialGrid::brute_force_pairs(boxes, truth);
    NF_CHECK_EQ(truth.size(), size_t{1});
    NF_CHECK(is_superset_of(candidates, truth));
}

NF_TEST(broadphase_unbounded_body_pairs_with_everything) {
    // A plane has no meaningful cell; it must be a candidate against every body
    // or nothing ever lands on the floor.
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3::zero, Vec3(0.5f)));
    grid.insert(1, Aabb::from_centre_half_extents(Vec3(50.0f, 0.0f, 0.0f), Vec3(0.5f)));
    grid.add_unbounded(2);

    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK_EQ(pairs.size(), size_t{2});
    NF_CHECK(contains(pairs, BroadphasePair{0, 2}));
    NF_CHECK(contains(pairs, BroadphasePair{1, 2}));
    NF_CHECK_EQ(grid.unbounded_count(), size_t{1});
}

NF_TEST(broadphase_oversized_body_falls_back_to_unbounded) {
    // Without the cap this insert() would loop over ~10^18 cells and the process
    // would never return. A body this large is a bug elsewhere, but the
    // broadphase must survive it rather than hang.
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3::zero, Vec3(1.0e12f)));
    grid.insert(1, Aabb::from_centre_half_extents(Vec3(5.0f, 0.0f, 0.0f), Vec3(0.5f)));

    NF_CHECK_EQ(grid.unbounded_count(), size_t{1});
    NF_CHECK_EQ(grid.bounded_count(), size_t{1});

    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK(contains(pairs, BroadphasePair{0, 1}));
}

NF_TEST(broadphase_order_is_stable_across_rebuilds) {
    // The determinism guarantee rests on this: identical input must produce an
    // identical sequence, not merely an identical set. Hash-map iteration order
    // is what would break it.
    std::vector<Aabb> boxes;
    Rng rng;
    for (int i = 0; i < 30; ++i) {
        const Vec3 c(rng.next(-6.0f, 6.0f), rng.next(-6.0f, 6.0f), rng.next(-6.0f, 6.0f));
        boxes.push_back(Aabb::from_centre_half_extents(c, Vec3(0.8f)));
    }

    std::vector<BroadphasePair> first, second;
    {
        SpatialGrid grid(1.5f);
        for (size_t i = 0; i < boxes.size(); ++i) grid.insert(static_cast<u32>(i), boxes[i]);
        grid.pairs(first);
    }
    {
        SpatialGrid grid(1.5f);
        for (size_t i = 0; i < boxes.size(); ++i) grid.insert(static_cast<u32>(i), boxes[i]);
        grid.pairs(second);
    }

    NF_CHECK(first == second);
    NF_CHECK(!first.empty()); // a vacuous pass would prove nothing
}

NF_TEST(broadphase_clear_resets_everything) {
    SpatialGrid grid(1.0f);
    grid.insert(0, Aabb::from_centre_half_extents(Vec3::zero, Vec3(0.5f)));
    grid.add_unbounded(1);
    grid.clear();

    NF_CHECK_EQ(grid.bounded_count(), size_t{0});
    NF_CHECK_EQ(grid.unbounded_count(), size_t{0});
    NF_CHECK_EQ(grid.occupied_cell_count(), size_t{0});

    std::vector<BroadphasePair> pairs;
    grid.pairs(pairs);
    NF_CHECK(pairs.empty());
}
