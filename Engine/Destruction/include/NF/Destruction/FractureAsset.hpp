#pragma once

// NF/Destruction/FractureAsset.hpp — a breakable mesh as a tree of chunks.
// Design doc Section 41: "Breakable meshes / Fracture assets", and Section 371
// item 4: "Destruction hooks on the triangle mesh — chunk-based debris".
//
// A fracture asset is built once from a mesh (asset-cook time or at load) and
// is pure data: a binary tree of convex chunks plus the bonds that hold the
// tree together. Nothing here touches physics or rendering, so the asset can
// be built, inspected and verified on any machine.

#include <NF/Destruction/FractureMath.hpp>
#include <NF/Core/Types.hpp>

#include <span>
#include <string>
#include <vector>

namespace nf::destruction {

inline constexpr u32 kInvalidChunk = 0xFFFFFFFFu;

/// One region of a breakable mesh, in the asset's local space.
///
/// A chunk is either a leaf — a convex solid, the one kind of thing that can
/// become a debris body — or an internal node, which exists only to describe
/// how a region splits. The tree is full and binary: every internal chunk has
/// exactly two children, produced by one plane cut.
struct FractureChunk {
    FracturePiece piece;
    Vec3 centroid{0.0f, 0.0f, 0.0f};   // volume centroid, cached at build
    f32  volume = 0.0f;                // cached at build
    u32  parent = kInvalidChunk;
    u32  children[2] = {kInvalidChunk, kInvalidChunk};
    u32  depth = 0u;

    bool is_leaf() const { return children[0] == kInvalidChunk; }
};

/// The internal face where two chunks meet, and how hard it resists being
/// torn apart.
///
/// Bonds are recorded at build time between the two children of every split,
/// so a bond's geometry is the cut polygon itself — no face matching, no
/// epsilon search. `detach_chunk` is the child that becomes debris when the
/// bond breaks; the other side keeps the root, so the asset always leaves one
/// surviving body rather than dissolving entirely.
struct FractureBond {
    u32  parent = kInvalidChunk;     // the chunk whose split made this face
    u32  detach_chunk = kInvalidChunk;   // child index that comes loose
    u32  hold_chunk = kInvalidChunk;     // the other child
    Vec3 plane_normal{0.0f, 0.0f, 0.0f};   // unit, from hold to detach
    f32  plane_d = 0.0f;
    f32  area = 0.0f;                // shared face area
    Vec3 centroid{0.0f, 0.0f, 0.0f}; // shared face centroid
    f32  strength = 0.0f;            // impulse that shatters it
};

/// Build knobs. Every one has a default, and every one is data — no RNG, no
/// clock, no allocation order feeds the output (Section 114).
struct FractureParams {
    u32 seed = 0x5EEDBEEFu;          // root of every plane; same seed, same shards
    u32 target_chunks = 8u;          // stop splitting at this many leaves
    u32 max_depth = 6u;              // hard ceiling regardless of target
    f32 min_chunk_volume = 0.001f;   // never split a chunk below this
    f32 jitter = 0.30f;              // plane wander, as a fraction of chunk extent
    f32 strength_per_area = 25.0f;   // bond strength = this * shared face area
};

struct FractureAsset {
    std::vector<FractureChunk> chunks;
    std::vector<FractureBond>  bonds;

    /// Chunk 0 is the root: the piece that remains "the object" when every
    /// bond around it has broken.
    bool empty() const { return chunks.empty(); }
    u32  leaf_count() const;

    /// Leaves of the subtree rooted at `chunk`, in tree order. This is the set
    /// of debris bodies one broken bond releases.
    void collect_leaves(u32 chunk, std::vector<u32>& out) const;

    /// Bonds still holding `chunk` to the tree — the ones a damage query has to
    /// consider for that chunk's subtree.
    void bonds_of(u32 chunk, std::vector<u32>& out) const;
};

/// Builds a fracture asset from `source`. The mesh is reduced to its convex
/// hull first (see build_convex_hull for why that is a feature rather than a
/// loss), then split by hash-derived planes until the target is reached or
/// nothing left is splittable.
///
/// Deterministic by construction: every plane derives from (seed, chunk id,
/// depth), and chunk ids are assigned in creation order, so two builds from the
/// same mesh and params are byte-identical. The test suite asserts that.
///
/// `out_error`, when non-null, receives a reason for a degenerate result
/// (fewer than four points, or a hull that no plane can split).
void build_fracture_asset(const FracturePiece& source, const FractureParams& params,
                          FractureAsset& out, std::string* out_error = nullptr);

} // namespace nf::destruction
