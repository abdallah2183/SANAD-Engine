#pragma once

// NF/Destruction/FractureMath.hpp — convex geometry for breakable meshes.
// Design doc Section 41 (Destruction): "Breakable meshes / Fracture assets /
// Debris / Impulses", Section 114 (Determinism).
//
// The whole file is pure CPU math over plain buffers — no RHI, no physics —
// so a fracture asset can be built and verified on any machine, and so the
// numbers below can be checked against closed-form answers in tests.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <span>
#include <vector>

namespace nf::destruction {

// =========================================================================
// Deterministic noise
//
// One hash for the whole module. Every fracture plane, every jitter offset and
// every debris spawn derives from it, so no RNG is ever seeded anywhere in the
// destruction pipeline and the same input asset + seed yields byte-identical
// chunks on every run (Section 114). The 2D engine has its own copy in
// Math2D.hpp; that one is a 2D-scoped helper and this is 3D, and duplicating a
// five-line mix function is cheaper than making Scene2D a dependency of a
// physics module that has nothing to do with sprites.
// =========================================================================
inline f32 hash_unit(u32 bits) {
    bits ^= 0x9E3779B9u;
    bits = (bits ^ (bits >> 16)) * 0x85EBCA6Bu;
    bits = (bits ^ (bits >> 13)) * 0xC2B2AE35u;
    bits ^= bits >> 16;
    return static_cast<f32>(bits) / static_cast<f32>(0xFFFFFFFFu);
}

// =========================================================================
// FracturePiece — a closed, consistently wound, strictly convex polyhedron.
//
// This is the only geometry type the destruction module understands, and it is
// shaped to match assets::MeshAsset (positions + indices) on purpose: a chunk
// is what a mesh becomes when it breaks, and it is what a convex-hull physics
// shape wants, so a chunk reaches the world as render geometry and as a
// collider from one structure with no conversion step in between.
//
// Winding is counter-clockwise as seen from outside, in a right-handed,
// y-up space — the same convention the renderer's meshes use, so chunks render
// with the same front faces as the object they came from.
// =========================================================================
struct FracturePiece {
    std::vector<Vec3> vertices;
    std::vector<u32>  indices;   // flat triangles, 3 per face

    void clear() { vertices.clear(); indices.clear(); }
    usize triangle_count() const { return indices.size() / 3u; }
};

/// Half-space. A point is on the inside when `normal.dot(p) + d >= 0`.
struct Plane {
    Vec3 normal;
    f32  d;

    static Plane from_point_and_normal(Vec3 point, Vec3 normal) {
        return {normal, -normal.dot(point)};
    }

    f32 signed_distance(Vec3 p) const { return normal.dot(p) + d; }
};

struct PieceBounds {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    Vec3 size() const { return max - min; }
    Vec3 center() const { return (min + max) * 0.5f; }
};

// =========================================================================
// Convex hull
// =========================================================================

/// Builds the smallest convex piece containing every input point. Any input
/// works — a non-convex mesh simply collapses to its hull — and the result is
/// always closed, convex and consistently wound. Fracture is defined on the
/// hull rather than the raw triangles because a slice of a convex solid is
/// convex, which is what keeps every chunk a valid collider (see the note on
/// FracturePiece).
///
/// Fewer than four non-coplanar points cannot bound a volume and leave an empty
/// piece; the caller treats that as "not breakable".
void build_convex_hull(std::span<const Vec3> points, FracturePiece& out);

// =========================================================================
// Measurement
// =========================================================================

/// Volume by the divergence theorem: the sum of signed pyramid volumes from the
/// origin. Correct for any closed, consistently wound mesh regardless of where
/// the origin sits (the signs cancel per tetrahedron), and it doubles as a
/// winding check — a piece wound the wrong way comes out negative, which is
/// how the tests catch a flipped cap.
f32 piece_volume(const FracturePiece& piece);

/// Volume centroid. Tetrahedron-area weighted, same derivation as
/// piece_volume, so the two always agree about which side of the origin a
/// chunk's mass is on.
Vec3 piece_centroid(const FracturePiece& piece);

PieceBounds piece_bounds(const FracturePiece& piece);

/// True when the surface closes: every edge is shared by exactly two triangles
/// and Euler's relation holds. This is the cheap structural check the asset
/// builder and the tests use to promise that a chunk is renderable as a solid;
/// it says nothing about convexity.
bool piece_is_watertight(const FracturePiece& piece);

/// True when no vertex is beyond any face's plane. Cheap enough to run on
/// every chunk the builder emits; a non-convex chunk is a bug, because it would
/// be handed to a physics shape that assumes convexity.
bool piece_is_convex(const FracturePiece& piece, f32 tol = 1e-4f);

// =========================================================================
// Cutting
// =========================================================================

/// Splits `in` along `plane` into the part on the inside (`out_pos`, where the
/// signed distance is >= 0) and the part on the outside (`out_neg`).
///
/// Both results are closed and watertight: the hole each cut leaves is capped
/// with a fan over the cross-section polygon, ordered by angle around the
/// plane normal so the cap is convex when the cut section is convex — always
/// the case for a convex input.
///
/// Returns false when the plane misses the piece entirely (one side empty), in
/// which case the empty side is left empty and the intact side is a copy of
/// `in`. A caller that wants "cut or do nothing" checks the return value.
///
/// `out_section`, when given, receives the cut polygon in cyclic order — the
/// geometry of the face the two halves share, which is what a fracture bond
/// measures. Empty when the function returns false.
bool clip_piece(const FracturePiece& in, const Plane& plane,
                FracturePiece& out_pos, FracturePiece& out_neg,
                std::vector<Vec3>* out_section = nullptr);

/// Area of a planar convex polygon.
f32 polygon_area(std::span<const Vec3> polygon);

/// Area centroid of a planar convex polygon.
Vec3 polygon_centroid(std::span<const Vec3> polygon);

} // namespace nf::destruction
