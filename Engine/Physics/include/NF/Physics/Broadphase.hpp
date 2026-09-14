#pragma once

#include <NF/Physics/Shapes.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nf::physics {

/// A pair of body indices that the broadphase could not rule out. Ordered so
/// that `a < b`, which is what makes the output comparable and deduplicable.
struct BroadphasePair {
    u32 a = 0;
    u32 b = 0;

    bool operator<(const BroadphasePair& o) const {
        return (a != o.a) ? (a < o.a) : (b < o.b);
    }
    bool operator==(const BroadphasePair& o) const { return a == o.a && b == o.b; }
};

/// Uniform spatial hash grid.
///
/// Chosen over a BVH because the input is a modest number of similarly sized
/// bodies, it needs no rebuild heuristic, and it allocates nothing per frame
/// after the first. The replaceable seam is `pairs()`: a BVH or a Jolt
/// broadphase satisfies the same contract (a superset of real overlaps, in a
/// deterministic order), so nothing above this needs to change.
///
/// The contract that matters: **the output must be a superset of the true
/// overlaps and a subset of all pairs.** Missing a pair means objects fall
/// through each other; the test suite checks the superset property against brute
/// force rather than trusting the implementation.
class SpatialGrid {
public:
    explicit SpatialGrid(f32 cell_size = 2.0f);

    void clear();

    /// Insert a bounded body. `index` is whatever the caller uses to identify it
    /// — the physics world uses its own body slot index.
    void insert(u32 index, const Aabb& box);

    /// Register a body that must be tested against everything: an infinite plane
    /// has no meaningful cell, and clamping it to one would either miss contacts
    /// or smear it across the whole grid.
    void add_unbounded(u32 index);

    /// Emit candidate pairs. `out` is cleared first. The result is sorted and
    /// deduplicated, so the same set of bodies always produces the same sequence
    /// regardless of hash-map iteration order — which is what the determinism
    /// test depends on.
    void pairs(std::vector<BroadphasePair>& out) const;

    /// Brute-force reference: every pair whose AABBs overlap. Exists so the grid
    /// can be checked against a definition rather than against itself, and so
    /// the test does not have to reimplement the grid to validate it.
    static void brute_force_pairs(const std::vector<Aabb>& boxes,
                                  std::vector<BroadphasePair>& out);

    size_t occupied_cell_count() const { return m_cells.size(); }
    size_t bounded_count() const { return m_bounded_count; }
    size_t unbounded_count() const { return m_unbounded.size(); }

private:
    struct CellCoord {
        i32 x = 0, y = 0, z = 0;
    };

    static u64 pack(const CellCoord& c);

    f32 m_cell_size = 2.0f;
    f32 m_inv_cell_size = 0.5f;
    size_t m_bounded_count = 0;

    std::unordered_map<u64, std::vector<u32>> m_cells;
    /// Occupied coordinates, kept explicitly so `pairs()` can walk neighbours
    /// without iterating the hash map and depending on its order.
    std::vector<CellCoord> m_occupied;
    std::vector<u32> m_unbounded;
};

} // namespace nf::physics
