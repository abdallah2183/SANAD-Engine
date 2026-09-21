// NF/Destruction/FractureMath.cpp — convex geometry for breakable meshes.
// Design doc Section 41 (Destruction), Section 114 (Determinism).

#include <NF/Destruction/FractureMath.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace nf::destruction {

namespace {

// ---------------------------------------------------------------------------
// build_convex_hull internals
// ---------------------------------------------------------------------------

struct HullFace {
    u32  a, b, c;      // CCW as seen from outside
    Vec3 normal;       // unit, outward
    f32  plane_d;      // plane through the face: normal.p + d = 0
};

/// Outward distance of `p` from the face: positive when `p` is on the side the
/// normal points at, i.e. outside the hull.
f32 face_distance(const HullFace& f, const std::vector<Vec3>& verts, Vec3 p) {
    return f.normal.dot(p - verts[f.a]);
}

HullFace make_face(u32 a, u32 b, u32 c, const std::vector<Vec3>& verts) {
    HullFace f;
    f.a = a;
    f.b = b;
    f.c = c;
    const Vec3 n = (verts[b] - verts[a]).cross(verts[c] - verts[a]);
    f.normal = n.normalized();
    f.plane_d = -f.normal.dot(verts[a]);
    return f;
}

/// A directed edge key, used to ask "which face owns b->a" while walking a
/// visible region for its horizon.
inline std::pair<u32, u32> edge_key(u32 a, u32 b) { return {a, b}; }

// ---------------------------------------------------------------------------
// clip_piece internals
// ---------------------------------------------------------------------------

/// Signed pyramid volume from the origin for one triangle. Positive when the
/// triangle is wound CCW as seen from outside in a right-handed space; the sign
/// is what lets piece_volume double as a winding check.
inline f32 tet_volume(const Vec3& a, const Vec3& b, const Vec3& c) {
    return a.dot(b.cross(c)) * (1.0f / 6.0f);
}

/// Strictly off the cut plane — neither on it nor a computed crossing. Crossings
/// are always strictly off-plane by construction, so a vertex of the merged
/// buffer is on the plane only when it is an original with |d| ~ 0.
inline bool off_plane(f32 d) { return (d > EPSILON) || (d < -EPSILON); }

/// "No vertex" sentinel for local searches over the merged buffer.
inline constexpr u32 kNoVertex = 0xFFFFFFFFu;

/// Newell's normal of a polygon: it accumulates over every edge, so it stays
/// correct when the first three vertices happen to sit on a straight run of
/// boundary — which a section carrying face-diagonal crossings can. The naive
/// "first triangle's" normal is undefined there.
Vec3 polygon_newell_normal(std::span<const Vec3> polygon) {
    Vec3 normal{0.0f, 0.0f, 0.0f};
    for (usize i = 0u; i < polygon.size(); ++i) {
        normal = normal + polygon[i].cross(polygon[(i + 1u) % polygon.size()]);
    }
    return normal;
}

} // namespace

void build_convex_hull(std::span<const Vec3> points, FracturePiece& out) {
    out.clear();
    if (points.size() < 4u) return;

    const std::vector<Vec3> verts(points.begin(), points.end());
    const auto count = static_cast<u32>(verts.size());

    // Seed tetrahedron. Every search is a scan in input order, so a tie goes to
    // the earliest index on every machine — the hull is a function of the point
    // set alone, never of pointer order or allocator state.
    u32 i0 = 0u;
    for (u32 i = 1u; i < count; ++i) {
        if (verts[i].x < verts[i0].x) i0 = i;
    }
    u32 i1 = 0u;
    for (u32 i = 0u; i < count; ++i) {
        if (verts[i].x > verts[i1].x) i1 = i;
    }
    if (i0 == i1) return;   // zero extent in x: a flat point cloud

    // Farthest from the x-axis line, then farthest from the seed triangle's
    // plane. Both rejections matter: a collinear cloud and a coplanar cloud
    // both bound no volume, and either would produce a degenerate hull.
    const Vec3 line_dir = (verts[i1] - verts[i0]).normalized();
    u32 i2 = 0u;
    f32 best2 = -1.0f;
    for (u32 i = 0u; i < count; ++i) {
        const Vec3 rel  = verts[i] - verts[i0];
        const Vec3 perp = rel - line_dir * rel.dot(line_dir);
        const f32 d2 = perp.length_sq();
        if (d2 > best2) { best2 = d2; i2 = i; }
    }
    if (best2 < EPSILON) return;   // collinear

    u32 i3 = 0u;
    f32 best3 = -1.0f;
    const Vec3 seed_normal = (verts[i2] - verts[i0]).cross(verts[i1] - verts[i0]).normalized();
    for (u32 i = 0u; i < count; ++i) {
        const f32 dist = std::fabs(seed_normal.dot(verts[i] - verts[i0]));
        if (dist > best3) { best3 = dist; i3 = i; }
    }
    if (best3 < EPSILON) return;   // coplanar: no volume to bound

    std::vector<HullFace> faces;
    // Each seed face is oriented so that the vertex opposite it is inside; the
    // three faces after the first share i3, so each needs its own opposite
    // point or the tetrahedron starts inside out and every insertion after it
    // is meaningless.
    const auto seed = [&](u32 a, u32 b, u32 c, u32 opposite) {
        HullFace f = make_face(a, b, c, verts);
        if (face_distance(f, verts, verts[opposite]) > 0.0f) {
            f = make_face(a, c, b, verts);
        }
        faces.push_back(f);
    };
    seed(i0, i1, i2, i3);
    seed(i0, i1, i3, i2);
    seed(i0, i2, i3, i1);
    seed(i1, i2, i3, i0);

    for (u32 p = 0u; p < count; ++p) {
        if (p == i0 || p == i1 || p == i2 || p == i3) continue;
        const Vec3 point = verts[p];

        std::vector<u8> visible(faces.size(), 0u);
        bool any_visible = false;
        for (usize f = 0u; f < faces.size(); ++f) {
            if (face_distance(faces[f], verts, point) > EPSILON) {
                visible[f] = 1u;
                any_visible = true;
            }
        }
        if (!any_visible) continue;   // already inside the hull

        // Directed edge -> owning face, over the current hull only.
        std::map<std::pair<u32, u32>, u32> owner;
        for (u32 f = 0u; f < static_cast<u32>(faces.size()); ++f) {
            const HullFace& face = faces[f];
            owner[edge_key(face.a, face.b)] = f;
            owner[edge_key(face.b, face.c)] = f;
            owner[edge_key(face.c, face.a)] = f;
        }

        // A visible face's edge is on the horizon when the face across it does
        // not also see the point; those edges are the rim the new faces hang
        // from. New face (a, b, p) keeps the old outward side, because p is on
        // the outward side of the face the edge came from.
        std::vector<HullFace> kept;
        std::vector<HullFace> added;
        for (u32 f = 0u; f < static_cast<u32>(faces.size()); ++f) {
            if (visible[f] == 0u) {
                kept.push_back(faces[f]);
                continue;
            }
            const u32 ring[3] = {faces[f].a, faces[f].b, faces[f].c};
            for (u32 k = 0u; k < 3u; ++k) {
                const u32 a = ring[k];
                const u32 b = ring[(k + 1u) % 3u];
                const auto it = owner.find(edge_key(b, a));
                const bool neighbour_visible =
                    (it != owner.end()) && (visible[it->second] != 0u);
                if (!neighbour_visible) {
                    added.push_back(make_face(a, b, p, verts));
                }
            }
        }
        faces = std::move(kept);
        faces.insert(faces.end(), added.begin(), added.end());
    }

    // Compact to the vertices a face actually uses; interior points the hull
    // never referenced are dropped, so the piece is exactly its boundary.
    std::map<u32, u32> remap;
    for (const HullFace& f : faces) {
        for (const u32 v : {f.a, f.b, f.c}) {
            if (remap.find(v) == remap.end()) {
                remap[v] = static_cast<u32>(out.vertices.size());
                out.vertices.push_back(verts[v]);
            }
        }
    }
    for (const HullFace& f : faces) {
        out.indices.push_back(remap[f.a]);
        out.indices.push_back(remap[f.b]);
        out.indices.push_back(remap[f.c]);
    }
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

f32 piece_volume(const FracturePiece& piece) {
    f32 total = 0.0f;
    for (usize i = 0u; i < piece.indices.size(); i += 3u) {
        total += tet_volume(piece.vertices[piece.indices[i]],
                            piece.vertices[piece.indices[i + 1u]],
                            piece.vertices[piece.indices[i + 2u]]);
    }
    return total;
}

Vec3 piece_centroid(const FracturePiece& piece) {
    f32  total_vol = 0.0f;
    Vec3 acc{0.0f, 0.0f, 0.0f};
    for (usize i = 0u; i < piece.indices.size(); i += 3u) {
        const Vec3& a = piece.vertices[piece.indices[i]];
        const Vec3& b = piece.vertices[piece.indices[i + 1u]];
        const Vec3& c = piece.vertices[piece.indices[i + 2u]];
        const f32   v = tet_volume(a, b, c);
        total_vol += v;
        acc = acc + (a + b + c) * v;
    }
    if (std::fabs(total_vol) < EPSILON) return piece_bounds(piece).center();
    return acc * (1.0f / (4.0f * total_vol));
}

PieceBounds piece_bounds(const FracturePiece& piece) {
    PieceBounds b;
    if (piece.vertices.empty()) return b;
    b.min = piece.vertices[0];
    b.max = piece.vertices[0];
    for (const Vec3& v : piece.vertices) {
        b.min = b.min.min(v);
        b.max = b.max.max(v);
    }
    return b;
}

bool piece_is_watertight(const FracturePiece& piece) {
    const usize triangle_count = piece.triangle_count();
    if (triangle_count < 4u) return false;

    // Each undirected edge must belong to exactly two triangles.
    std::vector<std::pair<u32, u32>> edges;
    edges.reserve(triangle_count * 3u);
    for (usize i = 0u; i < piece.indices.size(); i += 3u) {
        const u32 ring[3] = {piece.indices[i], piece.indices[i + 1u],
                             piece.indices[i + 2u]};
        for (u32 k = 0u; k < 3u; ++k) {
            const u32 a = ring[k];
            const u32 b = ring[(k + 1u) % 3u];
            if (a == b) return false;   // zero-length edge
            edges.push_back(a < b ? std::make_pair(a, b)
                                  : std::make_pair(b, a));
        }
    }
    std::sort(edges.begin(), edges.end());

    // A group per undirected edge; each group must hold exactly the two
    // directed copies a closed surface puts on it.
    usize i = 0u;
    usize unique_edges = 0u;
    while (i < edges.size()) {
        usize j = i;
        while (j < edges.size() && edges[j] == edges[i]) ++j;
        if (j - i != 2u) return false;
        ++unique_edges;
        i = j;
    }

    // Euler: the closed surface of a solid topologically equivalent to a ball
    // has χ = 2. This is the cheap promise that a chunk is renderable as a
    // solid; it says nothing about convexity.
    const isize v = static_cast<isize>(piece.vertices.size());
    const isize e = static_cast<isize>(unique_edges);
    const isize f = static_cast<isize>(triangle_count);
    return (v - e + f) == 2;
}

bool piece_is_convex(const FracturePiece& piece, f32 tol) {
    for (usize i = 0u; i < piece.indices.size(); i += 3u) {
        const Vec3& a = piece.vertices[piece.indices[i]];
        const Vec3& b = piece.vertices[piece.indices[i + 1u]];
        const Vec3& c = piece.vertices[piece.indices[i + 2u]];
        const Vec3  n = (b - a).cross(c - a);

        // A zero-area face is skipped, not convicted. A cut can leave a face
        // whose three corners are collinear — the shared ids of a degenerate
        // input triangle weld to one point — and such a face pins no plane, so
        // it can testify to neither convexity nor its absence. This cannot hide
        // a real reflex edge: that needs two faces of positive area meeting
        // concavely, and both are still tested here.
        if (n.length_sq() < EPSILON) continue;
        const Vec3 unit = n.normalized();
        const f32  d = -unit.dot(a);
        for (const Vec3& v : piece.vertices) {
            if (unit.dot(v) + d > tol) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cutting
// ---------------------------------------------------------------------------

bool clip_piece(const FracturePiece& in, const Plane& plane,
                FracturePiece& out_pos, FracturePiece& out_neg,
                std::vector<Vec3>* out_section) {
    out_pos.clear();
    out_neg.clear();

    // The plane is used as a true distance field: the on-plane test, the weld
    // below, and the convexity margins all compare `d` against EPSILON in units
    // of length, so the normal is unit here even when a caller hands over a
    // scaled one. A zero normal leaves every vertex equidistant, which the
    // one-sided test below already reports as "not a cut".
    const f32  n_len = plane.normal.length();
    const Vec3 n_unit = (n_len > EPSILON) ? plane.normal * (1.0f / n_len)
                                          : plane.normal;
    const f32  n_d = (n_len > EPSILON) ? (plane.d / n_len) : plane.d;

    const usize n = in.vertices.size();
    std::vector<f32> d(n);
    bool any_pos = false;
    bool any_neg = false;
    for (usize i = 0u; i < n; ++i) {
        d[i] = n_unit.dot(in.vertices[i]) + n_d;
        if (d[i] > EPSILON) any_pos = true;
        else if (d[i] < -EPSILON) any_neg = true;
    }
    // A plane that leaves the piece entirely on one side is not a cut. The
    // intact side is handed back as a copy so a caller that wants "cut or do
    // nothing" can rely on the return value alone.
    if (!any_pos || !any_neg) {
        if (any_pos) out_pos = in;
        else if (any_neg) out_neg = in;
        else out_pos = in;
        return false;
    }

    // Shared geometry: original vertices keep ids in [0, n); a crossing of edge
    // {a, b} gets one id, so the two halves of a cut point are the same vertex
    // rather than two floats that merely compare equal. That is what makes both
    // halves weld shut instead of leaving a seam.
    std::vector<Vec3>                  merged(in.vertices);
    std::map<std::pair<u32, u32>, u32> crossing_id;
    // A crossing that lands on a point the piece already owns reuses that id
    // rather than taking a new one. This is not a rare coincidence: a cap is a
    // polygon triangulated with diagonals, and when the polygon's boundary
    // carries collinear vertices — which it does as soon as the source mesh has
    // a face diagonal the cut crosses — a diagonal runs through one of them, so
    // a later cut crosses that diagonal exactly at a vertex the mesh already
    // knows. A second id at the same point pinches both halves along a
    // zero-length edge: the surface stops being a 2-manifold, the watertight
    // check fails on edge count, and the chunk's hull stretches over the pinch
    // into a volume it does not have. The reused vertex is within the plane
    // tolerance of the cut by construction, so it still reads on-plane and the
    // section holds it exactly once.
    constexpr f32 weld_sq = EPSILON * EPSILON;
    auto nearest_existing = [&](const Vec3& p) -> u32 {
        for (u32 w = 0u; w < merged.size(); ++w) {
            if ((merged[w] - p).length_sq() <= weld_sq) return w;
        }
        return kNoVertex;
    };

    auto crossing = [&](u32 a, u32 b) -> u32 {
        const auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
        auto it = crossing_id.find(key);
        if (it == crossing_id.end()) {
            const Vec3 va = in.vertices[a];
            const Vec3 vb = in.vertices[b];
            // t lies in (0, 1): the two endpoints are strictly on opposite
            // sides, so the denominator cannot vanish.
            const f32   t = d[a] / (d[a] - d[b]);
            const Vec3  x = va + (vb - va) * t;
            u32 id = nearest_existing(x);
            if (id == kNoVertex) {
                id = static_cast<u32>(merged.size());
                merged.push_back(x);
            }
            it = crossing_id.insert({key, id}).first;
        }
        return it->second;
    };

    // True for a vertex of the shared geometry that sits on the plane: either a
    // crossing (id >= n, which is on the plane by construction) or an original
    // vertex whose signed distance is within tolerance. Reading d[] directly is
    // safe here because original ids are the only ones that index it.
    auto on_plane_vertex = [&](u32 v) {
        return (v >= n) || !off_plane(d[v]);
    };

    // Emits a convex polygon into one half. Corners are welded into the piece by
    // their shared id rather than copied per polygon. Without that, a vertex
    // touched by four faces would exist four times and the piece would fail its
    // own closed-surface test on vertex count alone — the geometry would be
    // sound and the bookkeeping would say otherwise.
    //
    // Triangulation is a fan, not ear clipping. A clipped polygon can carry
    // vertices sitting on a straight run of boundary — a crossing that lands on
    // a face diagonal ends up mid-edge — and a polygon whose boundary is one
    // straight run has no ear to take: every candidate ear has zero area, and
    // the ear selector would erase past the end of the list. The fan still
    // triangulates it, emitting zero-area triangles that carry no volume and no
    // convexity but do carry the boundary edges exactly once each, which is what
    // the watertight check pairs against the walls. Faces with no area are
    // skipped downstream, in piece_is_convex, rather than avoided here.
    auto emit_polygon = [&](FracturePiece& piece, std::map<u32, u32>& local,
                            std::vector<u32> poly) {
        // Collapse vertices that repeat around the cycle. A degenerate input
        // triangle — one whose three vertices are collinear, which an earlier cut
        // can leave on a face — has every piercing edge meeting the plane at one
        // welded point, so the walk hands this polygon the same id twice. Kept,
        // that id would pair with itself and the triangle would reference one
        // vertex twice; dropped, the polygon is still the same boundary.
        if (poly.size() > 1u) {
            while (poly.size() > 1u && poly.front() == poly.back()) poly.pop_back();
            std::vector<u32> kept;
            kept.reserve(poly.size());
            for (const u32 v : poly) {
                if (!kept.empty() && kept.back() == v) continue;
                kept.push_back(v);
            }
            poly.swap(kept);
        }

        if (poly.size() < 3u) return;

        std::vector<u32> idx;
        idx.reserve(poly.size());
        for (const u32 v : poly) {
            auto it = local.find(v);
            if (it == local.end()) {
                it = local.insert({v, static_cast<u32>(piece.vertices.size())}).first;
                piece.vertices.push_back(merged[v]);
            }
            idx.push_back(it->second);
        }

        // A fan from the first vertex. Every polygon reaching here is convex by
        // construction — a wall is a triangle split by a line, the cut polygon is
        // the section of a convex solid — so the fan is an exact triangulation.
        // It also emits every boundary edge of the polygon exactly once, which is
        // what the watertight check pairs against the neighbouring walls, and it
        // degrades gracefully instead of failing when the polygon is degenerate:
        // a later cut can pierce a face whose boundary already carries crossings
        // on a straight run, and then the wall polygon is a collinear run with no
        // area to carve. Ear clipping has no ear to take there and would erase
        // past the end of the list; a fan just emits zero-area triangles, which
        // carry no volume and no convexity but still carry the edges.
        for (usize i = 1u; i + 1u < idx.size(); ++i) {
            piece.indices.push_back(idx[0u]);
            piece.indices.push_back(idx[i]);
            piece.indices.push_back(idx[i + 1u]);
        }
    };

    // Each half keeps its own vertex-to-index map: the two halves share the cut
    // polygon's geometry but not their index space.
    std::map<u32, u32> pos_local;
    std::map<u32, u32> neg_local;

    for (usize i = 0u; i < in.indices.size(); i += 3u) {
        const u32 ring[3] = {in.indices[i], in.indices[i + 1u],
                             in.indices[i + 2u]};
        std::vector<u32> pos_poly;
        std::vector<u32> neg_poly;
        pos_poly.reserve(4u);
        neg_poly.reserve(4u);

        // Walking the triangle's directed edges emits each vertex once, at the
        // start of its own outgoing edge, and each crossing in boundary order.
        // A vertex on the plane belongs to both halves — the shared face is
        // shared geometry, and counting it once per half is what closes both.
        for (u32 k = 0u; k < 3u; ++k) {
            const u32 a = ring[k];
            const u32 b = ring[(k + 1u) % 3u];
            const f32 da = d[a];
            const f32 db = d[b];

            if (da >= 0.0f) pos_poly.push_back(a);
            if (da <= 0.0f) neg_poly.push_back(a);
            if (off_plane(da) && off_plane(db) && ((da > 0.0f) != (db > 0.0f))) {
                // The edge pierces the plane once, strictly in its interior. An
                // endpoint already on the plane IS the boundary point, so no
                // second vertex is invented for it.
                const u32 x = crossing(a, b);
                pos_poly.push_back(x);
                neg_poly.push_back(x);
            }
        }

        emit_polygon(out_pos, pos_local, pos_poly);
        emit_polygon(out_neg, neg_local, neg_poly);
    }

    // Build the cut polygon from every vertex of the piece that lies in the
    // plane, rather than by walking triangle edges. A walk only sees the
    // boundary fragmented across triangle interiors, and a plane passing exactly
    // through existing vertices — a second cut through a face the first one
    // created — leaves the walk with a subset and the cap with a hole. The
    // section of a convex solid is a convex polygon whose vertices are exactly
    // the on-plane vertices of the solid: every on-plane vertex of a convex
    // body is an extreme point, so it lies on the section's boundary and never
    // strictly inside it.
    std::vector<u32> section;
    section.reserve(merged.size());
    for (u32 v = 0u; v < merged.size(); ++v) {
        if (on_plane_vertex(v)) section.push_back(v);
    }

    // Sort around the polygon's own vertex average. That average is strictly
    // interior for any convex polygon of positive area, so it is always a valid
    // sort centre. The order is comparison-based rather than atan2: no
    // transcendental means no dependence on the math library's rounding, so the
    // section is a function of the geometry alone (Section 114).
    Vec3 centre{0.0f, 0.0f, 0.0f};
    for (const u32 s : section) centre = centre + merged[s];
    if (!section.empty()) {
        centre = centre * (1.0f / static_cast<f32>(section.size()));
    }

    Vec3 basis_u = n_unit.nearly_equals(Vec3::up) ? Vec3::right : Vec3::up;
    basis_u = (basis_u - n_unit * basis_u.dot(n_unit)).normalized();
    const Vec3 basis_v = n_unit.cross(basis_u);

    const auto u_of = [&](const u32 v) { return basis_u.dot(merged[v] - centre); };
    const auto v_of = [&](const u32 v) { return basis_v.dot(merged[v] - centre); };

    std::stable_sort(section.begin(), section.end(),
        [&](const u32 a, const u32 b) {
            const f32 ua = u_of(a), va = v_of(a);
            const f32 ub = u_of(b), vb = v_of(b);
            // Half-plane first: the comparator is an angle order, and angle is
            // only a total order within a single turn.
            const bool a_upper = (va > 0.0f) || (va == 0.0f && ua > 0.0f);
            const bool b_upper = (vb > 0.0f) || (vb == 0.0f && ub > 0.0f);
            if (a_upper != b_upper) return a_upper;
            // a before b when a sits counter-clockwise-before b around centre.
            const f32 cross = ua * vb - ub * va;
            if (cross != 0.0f) return cross > 0.0f;
            // Collinear with the centre: near first, which keeps duplicates and
            // mid-edge crossings in a stable order.
            return (ua * ua + va * va) < (ub * ub + vb * vb);
        });

    // The shoelace in this right-handed (u, v, normal) frame is positive iff the
    // sorted polygon is counter-clockwise as seen from the side the normal
    // points at. A non-degenerate convex section sorts CCW by construction, so a
    // non-positive shoelace means the on-plane vertices are collinear or
    // duplicated — the plane only grazed the piece, which is not a cut.
    f32 shoelace = 0.0f;
    for (usize i = 0u; i < section.size(); ++i) {
        const u32 p0 = section[i];
        const u32 p1 = section[(i + 1u) % section.size()];
        shoelace += u_of(p0) * v_of(p1) - u_of(p1) * v_of(p0);
    }

    if (section.size() < 3u || shoelace <= 0.0f) {
        out_pos.clear();
        out_neg.clear();
        out_pos = in;
        return false;
    }

    // The sorted section is counter-clockwise around +n_unit, so emitting it
    // as-is gives the cap a +n_unit normal. That is outward for the negative
    // half: its vertices read n.v + d <= 0, and the cap's own plane is
    // n.v + d_cap with d_cap = -n.p for any on-plane p, so every negative vertex
    // reads <= 0 with margin |d_v|. Reversing gives -n_unit, outward for the
    // positive half by the same argument. Both hold with exact margin, so the
    // convexity check needs no tolerance for the cap.
    std::vector<u32> reversed(section.rbegin(), section.rend());
    emit_polygon(out_pos, pos_local, reversed);
    emit_polygon(out_neg, neg_local, section);

    if (out_section != nullptr) {
        out_section->clear();
        for (const u32 s : section) out_section->push_back(merged[s]);
    }

    return true;
}

f32 polygon_area(std::span<const Vec3> polygon) {
    if (polygon.size() < 3u) return 0.0f;
    // Newell's normal: the first three vertices can sit on a straight run of
    // boundary (a section carrying face-diagonal midpoints), which leaves the
    // naive triangle normal undefined and would report a real face as zero area.
    const Vec3 normal = polygon_newell_normal(polygon).normalized();
    if (normal.length_sq() < EPSILON) return 0.0f;
    f32 double_area = 0.0f;
    for (usize i = 0u; i < polygon.size(); ++i) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[(i + 1u) % polygon.size()];
        double_area += normal.dot(a.cross(b));
    }
    return double_area * 0.5f;
}

Vec3 polygon_centroid(std::span<const Vec3> polygon) {
    if (polygon.size() < 3u) {
        Vec3 acc{0.0f, 0.0f, 0.0f};
        for (const Vec3& p : polygon) acc = acc + p;
        return polygon.empty()
                   ? acc
                   : acc * (1.0f / static_cast<f32>(polygon.size()));
    }
    const Vec3 normal = polygon_newell_normal(polygon).normalized();
    // Area-weighted fan from the first vertex: each triangle (o, a, b)
    // contributes its own centroid and its signed double area, so the weighted
    // mean is the polygon's centroid no matter where the polygon sits. A plain
    // vertex average would park mass in a missing corner.
    const Vec3& origin = polygon[0];
    Vec3 acc{0.0f, 0.0f, 0.0f};
    f32  weight_sum = 0.0f;
    for (usize i = 1u; i + 1u < polygon.size(); ++i) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[i + 1u];
        const f32   w = normal.dot((a - origin).cross(b - origin));
        acc = acc + (origin + a + b) * w;
        weight_sum += w;
    }
    if (std::fabs(weight_sum) < EPSILON) {
        Vec3 plain{0.0f, 0.0f, 0.0f};
        for (const Vec3& p : polygon) plain = plain + p;
        return plain * (1.0f / static_cast<f32>(polygon.size()));
    }
    return acc * (1.0f / (3.0f * weight_sum));
}

} // namespace nf::destruction
