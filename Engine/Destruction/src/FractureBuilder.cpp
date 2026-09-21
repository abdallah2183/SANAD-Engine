// NF/Destruction/FractureBuilder.cpp — turning a mesh into a tree of chunks.
//
// One pass of convex hull, then repeated plane cuts. Every cut plane is
// derived from (seed, chunk id, depth) and nothing else, so the whole asset is
// a pure function of the source mesh and the params — rebuild it anywhere, get
// the same shards (Section 114).

#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Core/Math.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace nf::destruction {

namespace {

/// Mixes the seed with the chunk's address in the tree. Unsigned overflow is
/// defined behaviour and the mix is what makes distant chunks unrelated.
u32 plane_key(u32 seed, u32 chunk_id, u32 depth) {
    return seed + (chunk_id + 1u) * 0x9E3779B9u + (depth + 1u) * 0x85EBCA6Bu;
}

/// Unit direction, uniform over the sphere, from three hashes.
Vec3 hash_direction(u32 key) {
    const f32 a = hash_unit(key);
    const f32 b = hash_unit(key ^ 0xA5A5A5A5u);
    const f32 z = 2.0f * a - 1.0f;
    const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const f32 angle = TWO_PI * b;
    return Vec3{ r * std::cos(angle), r * std::sin(angle), z };
}

/// Any unit vector perpendicular to `n`, picked deterministically. Two vectors
/// that are nearly parallel need a different reference axis to stay well
/// conditioned.
Vec3 orthonormal_u(const Vec3& n) {
    const Vec3 reference = (std::fabs(n.dot(Vec3::up)) > 0.9f) ? Vec3::right : Vec3::up;
    return n.cross(reference).normalized();
}

} // namespace

void build_fracture_asset(const FracturePiece& source, const FractureParams& params,
                          FractureAsset& out, std::string* out_error) {
    out.chunks.clear();
    out.bonds.clear();

    if (source.vertices.size() < 4u) {
        if (out_error != nullptr) *out_error = "fewer than four vertices; no volume to break";
        return;
    }

    FracturePiece hull;
    build_convex_hull(source.vertices, hull);
    if (hull.vertices.empty()) {
        if (out_error != nullptr) *out_error = "degenerate hull; source bounds no volume";
        return;
    }

    FractureChunk root;
    root.piece = std::move(hull);
    root.centroid = piece_centroid(root.piece);
    root.volume = piece_volume(root.piece);
    root.parent = kInvalidChunk;
    root.depth = 0u;
    out.chunks.push_back(std::move(root));

    if (params.target_chunks <= 1u) return;

    // A chunk's plane is fixed by its identity, so a chunk that cannot be cut
    // by its plane can never be cut at all — it is retired once, not retried
    // every pass. That is what makes the loop terminate.
    std::vector<bool> retired(out.chunks.size(), false);

    bool any_split = true;
    while (any_split && out.leaf_count() < params.target_chunks) {
        any_split = false;
        for (u32 id = 0u; id < out.chunks.size(); ++id) {
            if (out.leaf_count() >= params.target_chunks) break;

            // Everything the chunk contributes to the cut is copied before the
            // push_backs below, which would invalidate a reference into the vector.
            const FractureChunk* probe = &out.chunks[id];
            if (!probe->is_leaf() || retired[id]) continue;
            if (probe->depth >= params.max_depth || probe->volume < params.min_chunk_volume) {
                retired[id] = true;
                continue;
            }

            const u32   cut_depth = probe->depth;
            const Vec3  cut_centroid = probe->centroid;
            FracturePiece cut_piece = std::move(probe->piece);
            probe = nullptr;

            const u32  key = plane_key(params.seed, id, cut_depth);
            const Vec3 normal = hash_direction(key);
            const Vec3 axis_u = orthonormal_u(normal);
            const Vec3 axis_v = normal.cross(axis_u);
            const f32  extent = piece_bounds(cut_piece).size().max_abs_component();
            const f32  offset_u = (2.0f * hash_unit(key ^ 0x5A5A5A5Au) - 1.0f) * params.jitter * extent;
            const f32  offset_v = (2.0f * hash_unit(key ^ 0x0F0F0F0Fu) - 1.0f) * params.jitter * extent;
            const Vec3 point = cut_centroid + axis_u * offset_u + axis_v * offset_v;
            const Plane plane = Plane::from_point_and_normal(point, normal);

            FracturePiece half_pos, half_neg;
            std::vector<Vec3> section;
            if (!clip_piece(cut_piece, plane, half_pos, half_neg, &section)) {
                retired[id] = true;
                continue;
            }

            const f32 volume_pos = piece_volume(half_pos);
            const f32 volume_neg = piece_volume(half_neg);
            if (volume_pos < params.min_chunk_volume || volume_neg < params.min_chunk_volume) {
                // The plane only shaved the chunk. Its one chance is spent.
                retired[id] = true;
                continue;
            }

            const u32 id_pos = static_cast<u32>(out.chunks.size());
            const u32 id_neg = id_pos + 1u;
            const bool pos_detaches = (volume_pos <= volume_neg);
            const u32 detach = pos_detaches ? id_pos : id_neg;
            const u32 hold = pos_detaches ? id_neg : id_pos;

            FractureChunk child_pos;
            child_pos.piece = std::move(half_pos);
            child_pos.centroid = piece_centroid(child_pos.piece);
            child_pos.volume = volume_pos;
            child_pos.parent = id;
            child_pos.depth = cut_depth + 1u;

            FractureChunk child_neg;
            child_neg.piece = std::move(half_neg);
            child_neg.centroid = piece_centroid(child_neg.piece);
            child_neg.volume = volume_neg;
            child_neg.parent = id;
            child_neg.depth = cut_depth + 1u;

            // Order matters: the children must land at id_pos and id_neg.
            out.chunks.push_back(std::move(child_pos));
            out.chunks.push_back(std::move(child_neg));
            retired.resize(out.chunks.size(), false);

            out.chunks[id].children[0] = id_pos;
            out.chunks[id].children[1] = id_neg;

            // The bond's geometry is the cut itself: the section polygon is
            // already cyclic and planar, so area and centroid are closed form.
            FractureBond bond;
            bond.parent = id;
            bond.detach_chunk = detach;
            bond.hold_chunk = hold;
            bond.plane_normal = pos_detaches ? normal : -normal;
            bond.plane_d = pos_detaches ? plane.d : -plane.d;
            bond.area = polygon_area(section);
            bond.centroid = polygon_centroid(section);
            bond.strength = params.strength_per_area * bond.area;
            out.bonds.push_back(bond);

            any_split = true;
        }
    }
}

} // namespace nf::destruction
