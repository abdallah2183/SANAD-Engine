// DestructionTests — convex geometry, fracture assets, determinism.
// (design doc Section 41: breakable meshes / fracture assets / debris;
// Section 371 item 4: destruction hooks on the triangle mesh.)
//
// Every number these tests compare against is closed form: a unit cube is
// volume 8, a tetrahedron is a sixth of a triple product, and an L made of
// three unit squares has area three. Nothing here is "it looked right".

#include <NF/Core/Math.hpp>
#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Destruction/FractureMath.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::destruction;

namespace {

/// A cube of side 2 centred at the origin: volume 8, six faces of area 4.
/// Counter-clockwise as seen from outside.
FracturePiece unit_cube() {
    FracturePiece p;
    p.vertices = {
        Vec3{-1.0f, -1.0f, -1.0f}, Vec3{ 1.0f, -1.0f, -1.0f},
        Vec3{ 1.0f,  1.0f, -1.0f}, Vec3{-1.0f,  1.0f, -1.0f},
        Vec3{-1.0f, -1.0f,  1.0f}, Vec3{ 1.0f, -1.0f,  1.0f},
        Vec3{ 1.0f,  1.0f,  1.0f}, Vec3{-1.0f,  1.0f,  1.0f},
    };
    p.indices = {
        0u, 2u, 1u,  0u, 3u, 2u,   // -z
        1u, 6u, 5u,  1u, 2u, 6u,   // +x
        5u, 7u, 4u,  5u, 6u, 7u,   // +z
        4u, 3u, 0u,  4u, 7u, 3u,   // -x
        4u, 0u, 1u,  4u, 1u, 5u,   // -y
        3u, 6u, 2u,  3u, 7u, 6u,   // +y
    };
    return p;
}

/// Four alternating corners of the cube above: a regular tetrahedron of edge
/// 2*sqrt(2), which is volume 8/3 (a third of the cube it is inscribed in).
FracturePiece unit_tetrahedron() {
    FracturePiece p;
    p.vertices = {
        Vec3{ 1.0f,  1.0f,  1.0f},
        Vec3{-1.0f, -1.0f,  1.0f},
        Vec3{-1.0f,  1.0f, -1.0f},
        Vec3{ 1.0f, -1.0f, -1.0f},
    };
    p.indices = { 0u, 2u, 1u,  0u, 1u, 3u,  0u, 3u, 2u,  1u, 2u, 3u };
    return p;
}

/// An L-shaped prism: three unit squares extruded by two. It is watertight and
/// consistently wound but has a reflex edge, so it is the cheapest solid that
/// is definitely not convex.
FracturePiece l_prism() {
    FracturePiece p;
    p.vertices = {
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 0.0f},
        Vec3{1.0f, 1.0f, 0.0f}, Vec3{1.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f},
        Vec3{0.0f, 0.0f, 2.0f}, Vec3{2.0f, 0.0f, 2.0f}, Vec3{2.0f, 1.0f, 2.0f},
        Vec3{1.0f, 1.0f, 2.0f}, Vec3{1.0f, 2.0f, 2.0f}, Vec3{0.0f, 2.0f, 2.0f},
    };
    p.indices = {
        // Cap at +z, contour 0..5 is counter-clockwise seen from above.
        6u,  7u,  8u,   6u,  8u,  9u,   6u,  9u, 10u,   6u, 10u, 11u,
        // Cap at -z, the same contour reversed.
        0u,  2u,  1u,   0u,  3u,  2u,   0u,  4u,  3u,   0u,  5u,  4u,
        // Walls, one quad per contour edge.
        0u,  1u,  7u,   0u,  7u,  6u,
        1u,  2u,  8u,   1u,  8u,  7u,
        2u,  3u,  9u,   2u,  9u,  8u,
        3u,  4u, 10u,   3u, 10u,  9u,
        4u,  5u, 11u,   4u, 11u, 10u,
        5u,  0u,  6u,   5u,  6u, 11u,
    };
    return p;
}

} // namespace

// =========================================================================
// Convex hull
// =========================================================================

NF_TEST(fracture_hull_of_a_cube_is_the_cube) {
    const FracturePiece cube = unit_cube();
    FracturePiece hull;
    build_convex_hull(cube.vertices, hull);

    NF_CHECK(hull.vertices.size() == 8u);
    NF_CHECK(hull.triangle_count() == 12u);
    NF_CHECK(piece_is_watertight(hull));
    NF_CHECK(piece_is_convex(hull));
    NF_CHECK_NEAR(piece_volume(hull), 8.0f, 1e-5f);
}

NF_TEST(fracture_hull_collapses_a_non_convex_mesh) {
    // An L-shaped prism: its 2D hull is the pentagon (0,0),(2,0),(2,1),(1,2),
    // (0,2) — the missing corner is sliced off by the hull edge through (2,1)
    // and (1,2), so the hull is not the full 2x2 box. Shoelace gives 3.5 for
    // the pentagon and the prism is two tall, hence seven, not eight.
    const FracturePiece el = l_prism();

    FracturePiece hull;
    build_convex_hull(el.vertices, hull);

    NF_CHECK(!hull.vertices.empty());
    NF_CHECK(piece_is_convex(hull));
    NF_CHECK_NEAR(piece_volume(hull), 7.0f, 1e-5f);
}

NF_TEST(fracture_hull_rejects_flat_input) {
    std::vector<Vec3> flat = {
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.5f, 0.5f, 0.0f},
    };
    FracturePiece hull;
    build_convex_hull(flat, hull);
    NF_CHECK(hull.vertices.empty());
}

NF_TEST(fracture_hull_ignores_interior_and_duplicate_points) {
    std::vector<Vec3> points = {
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f},
        Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 0.0f, 2.0f},
        Vec3{0.0f, 0.0f, 0.0f},   // exact duplicate
        Vec3{0.5f, 0.5f, 0.5f},   // strictly interior
    };
    FracturePiece hull;
    build_convex_hull(points, hull);

    NF_CHECK(hull.vertices.size() == 4u);
    NF_CHECK(hull.triangle_count() == 4u);
    NF_CHECK_NEAR(piece_volume(hull), 8.0f / 6.0f, 1e-5f);
}

// =========================================================================
// Measurement
// =========================================================================

NF_TEST(fracture_volume_matches_closed_form) {
    NF_CHECK_NEAR(piece_volume(unit_cube()), 8.0f, 1e-5f);
    NF_CHECK_NEAR(piece_volume(unit_tetrahedron()), 8.0f / 3.0f, 1e-5f);
}

NF_TEST(fracture_centroid_of_a_symmetric_piece_is_its_center) {
    const Vec3 c = piece_centroid(unit_cube());
    NF_CHECK_NEAR(c.x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(c.y, 0.0f, 1e-5f);
    NF_CHECK_NEAR(c.z, 0.0f, 1e-5f);
}

NF_TEST(fracture_centroid_tracks_mass_off_center) {
    // The same tetrahedron translated: the centroid must translate with it.
    const FracturePiece tet = unit_tetrahedron();
    const Vec3 shift{3.0f, -2.0f, 5.0f};

    FracturePiece moved;
    moved.vertices.reserve(tet.vertices.size());
    for (const Vec3& v : tet.vertices) moved.vertices.push_back(v + shift);
    moved.indices = tet.indices;

    const Vec3 base = piece_centroid(tet);
    const Vec3 c = piece_centroid(moved);
    NF_CHECK_NEAR((c - base).x, shift.x, 1e-5f);
    NF_CHECK_NEAR((c - base).y, shift.y, 1e-5f);
    NF_CHECK_NEAR((c - base).z, shift.z, 1e-5f);
}

NF_TEST(fracture_volume_sign_detects_inverted_winding) {
    FracturePiece cube = unit_cube();
    for (usize i = 0u; i < cube.indices.size(); i += 3u) {
        std::swap(cube.indices[i], cube.indices[i + 1u]);
    }
    NF_CHECK(piece_volume(cube) < 0.0f);
}

NF_TEST(fracture_watertight_check_rejects_a_hole) {
    FracturePiece cube = unit_cube();
    cube.indices.resize(cube.indices.size() - 3u);   // drop one triangle
    NF_CHECK(!piece_is_watertight(cube));
}

NF_TEST(fracture_convexity_check_rejects_a_concave_solid) {
    const FracturePiece el = l_prism();

    // Geometry is sound, so a non-convex verdict is about the shape and not
    // about broken input.
    NF_CHECK(piece_is_watertight(el));
    NF_CHECK_NEAR(std::fabs(piece_volume(el)), 6.0f, 1e-5f);
    NF_CHECK(!piece_is_convex(el, 1e-3f));
}

NF_TEST(fracture_bounds_of_the_unit_cube) {
    const PieceBounds b = piece_bounds(unit_cube());
    NF_CHECK_NEAR(b.size().x, 2.0f, 1e-5f);
    NF_CHECK_NEAR(b.size().y, 2.0f, 1e-5f);
    NF_CHECK_NEAR(b.size().z, 2.0f, 1e-5f);
}

NF_TEST(fracture_polygon_area_and_centroid_of_a_square) {
    std::vector<Vec3> square = {
        Vec3{-1.0f, 0.0f, -1.0f},
        Vec3{ 1.0f, 0.0f, -1.0f},
        Vec3{ 1.0f, 0.0f,  1.0f},
        Vec3{-1.0f, 0.0f,  1.0f},
    };
    NF_CHECK_NEAR(polygon_area(square), 4.0f, 1e-5f);
    const Vec3 c = polygon_centroid(square);
    NF_CHECK_NEAR(c.x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(c.z, 0.0f, 1e-5f);
}

NF_TEST(fracture_polygon_centroid_is_area_weighted) {
    // A unit square: the vertex average and the area centroid agree.
    std::vector<Vec3> square = {
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
        Vec3{1.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f},
    };
    NF_CHECK_NEAR(polygon_area(square), 1.0f, 1e-5f);
    NF_CHECK_NEAR(polygon_centroid(square).x, 0.5f, 1e-5f);

    // An L of three unit squares: the vertex average is (1, 1) — the missing
    // corner's centre — while the area centroid is (5/6, 5/6). An averaging
    // centroid would park the mass in empty space.
    std::vector<Vec3> el = {
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f},
        Vec3{2.0f, 0.0f, 1.0f}, Vec3{1.0f, 0.0f, 1.0f},
        Vec3{1.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, 2.0f},
    };
    NF_CHECK_NEAR(polygon_area(el), 3.0f, 1e-5f);
    NF_CHECK_NEAR(polygon_centroid(el).x, 5.0f / 6.0f, 1e-5f);
    NF_CHECK_NEAR(polygon_centroid(el).z, 5.0f / 6.0f, 1e-5f);
}

// =========================================================================
// Cutting
// =========================================================================

NF_TEST(fracture_clip_cube_in_half_by_volume) {
    const Plane plane = Plane::from_point_and_normal(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});

    FracturePiece pos, neg;
    NF_CHECK(clip_piece(unit_cube(), plane, pos, neg));

    NF_CHECK(piece_is_watertight(pos));
    NF_CHECK(piece_is_watertight(neg));
    NF_CHECK(piece_is_convex(pos));
    NF_CHECK(piece_is_convex(neg));
    NF_CHECK_NEAR(piece_volume(pos), 4.0f, 1e-5f);
    NF_CHECK_NEAR(piece_volume(neg), 4.0f, 1e-5f);
}

NF_TEST(fracture_clip_preserves_total_volume) {
    const Plane plane = Plane::from_point_and_normal(
        Vec3{0.3f, -0.2f, 0.1f}, Vec3{0.4f, 0.5f, 0.2f}.normalized());

    FracturePiece pos, neg;
    NF_CHECK(clip_piece(unit_cube(), plane, pos, neg));
    NF_CHECK_NEAR(piece_volume(pos) + piece_volume(neg), 8.0f, 1e-4f);
}

NF_TEST(fracture_clip_section_is_the_cut_face) {
    const Plane plane = Plane::from_point_and_normal(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});

    FracturePiece pos, neg;
    std::vector<Vec3> section;
    NF_CHECK(clip_piece(unit_cube(), plane, pos, neg, &section));

    // A cube of side 2 cut through its centre leaves a 2x2 face. The polygon
    // can carry more than the four true corners: a crossing that lands on a
    // face diagonal becomes a fifth vertex collinear on the section's boundary,
    // so the count is a lower bound while the area and centroid are exact.
    NF_CHECK(section.size() >= 4u);
    NF_CHECK_NEAR(polygon_area(section), 4.0f, 1e-5f);
    NF_CHECK_NEAR(polygon_centroid(section).x, 0.0f, 1e-5f);
}

NF_TEST(fracture_clip_missing_the_piece_reports_no_cut) {
    const Plane plane = Plane::from_point_and_normal(Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});

    FracturePiece pos, neg;
    NF_CHECK(!clip_piece(unit_cube(), plane, pos, neg));
    NF_CHECK(pos.vertices.empty());
    NF_CHECK_NEAR(piece_volume(neg), 8.0f, 1e-5f);
}

NF_TEST(fracture_clip_corners_repeatedly) {
    // A cube clipped twice is still convex and watertight — the property that
    // lets the builder recurse without ever repairing geometry.
    const Plane cut_x = Plane::from_point_and_normal(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});
    const Plane cut_y = Plane::from_point_and_normal(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});

    FracturePiece a_pos, a_neg;
    NF_CHECK(clip_piece(unit_cube(), cut_x, a_pos, a_neg));

    FracturePiece b_pos, b_neg;
    NF_CHECK(clip_piece(a_pos, cut_y, b_pos, b_neg));

    NF_CHECK(piece_is_watertight(b_pos));
    NF_CHECK(piece_is_convex(b_pos));
    NF_CHECK_NEAR(piece_volume(b_pos), 2.0f, 1e-5f);
}

// =========================================================================
// Assets
// =========================================================================

NF_TEST(fracture_asset_builds_a_full_binary_tree) {
    const FracturePiece cube = unit_cube();

    FractureParams params;
    params.target_chunks = 8u;

    FractureAsset asset;
    std::string error;
    build_fracture_asset(cube, params, asset, &error);

    NF_CHECK(error.empty());
    NF_CHECK(!asset.empty());
    NF_CHECK(asset.leaf_count() >= 4u);

    // A full binary tree: one internal chunk (and so one bond) per split,
    // every internal chunk has two children, and the root has no parent.
    NF_CHECK(2u * asset.bonds.size() + 1u == asset.chunks.size());
    NF_CHECK(asset.chunks[0u].parent == kInvalidChunk);

    for (usize i = 0u; i < asset.chunks.size(); ++i) {
        const FractureChunk& chunk = asset.chunks[i];
        if (chunk.is_leaf()) continue;

        NF_CHECK(chunk.children[0] != kInvalidChunk);
        NF_CHECK(chunk.children[1] != kInvalidChunk);
        NF_CHECK(asset.chunks[chunk.children[0]].parent == static_cast<u32>(i));
        NF_CHECK(asset.chunks[chunk.children[1]].parent == static_cast<u32>(i));
    }
}

NF_TEST(fracture_asset_leaves_partition_the_original_volume) {
    const FracturePiece cube = unit_cube();

    FractureParams params;
    params.target_chunks = 12u;

    FractureAsset asset;
    build_fracture_asset(cube, params, asset);
    NF_CHECK(asset.leaf_count() >= 4u);

    std::vector<u32> leaves;
    asset.collect_leaves(0u, leaves);
    NF_CHECK(static_cast<u32>(leaves.size()) == asset.leaf_count());

    f32 total = 0.0f;
    for (const u32 leaf : leaves) {
        const FractureChunk& chunk = asset.chunks[leaf];
        NF_CHECK(piece_is_watertight(chunk.piece));
        NF_CHECK(piece_is_convex(chunk.piece));
        NF_CHECK(chunk.volume > 0.0f);
        total += chunk.volume;
    }
    NF_CHECK_NEAR(total, 8.0f, 1e-3f);
}

NF_TEST(fracture_asset_root_volume_is_the_hull_volume) {
    FractureParams params;
    params.target_chunks = 4u;

    FractureAsset asset;
    build_fracture_asset(unit_tetrahedron(), params, asset);

    NF_CHECK(!asset.empty());
    NF_CHECK_NEAR(asset.chunks[0u].volume, 8.0f / 3.0f, 1e-5f);
}

NF_TEST(fracture_asset_is_deterministic) {
    const FracturePiece cube = unit_cube();

    FractureParams params;
    params.seed = 0x12345678u;
    params.target_chunks = 9u;
    params.jitter = 0.55f;

    FractureAsset a, b;
    build_fracture_asset(cube, params, a);
    build_fracture_asset(cube, params, b);

    NF_CHECK(a.chunks.size() == b.chunks.size());
    NF_CHECK(a.bonds.size() == b.bonds.size());
    for (usize i = 0u; i < a.chunks.size(); ++i) {
        const FractureChunk& ca = a.chunks[i];
        const FractureChunk& cb = b.chunks[i];
        NF_CHECK(ca.volume == cb.volume);
        NF_CHECK(ca.children[0] == cb.children[0]);
        NF_CHECK(ca.children[1] == cb.children[1]);
        NF_CHECK(ca.piece.vertices.size() == cb.piece.vertices.size());
        for (usize v = 0u; v < ca.piece.vertices.size(); ++v) {
            NF_CHECK(ca.piece.vertices[v].nearly_equals(cb.piece.vertices[v], 0.0f));
        }
    }
    for (usize i = 0u; i < a.bonds.size(); ++i) {
        NF_CHECK(a.bonds[i].area == b.bonds[i].area);
        NF_CHECK(a.bonds[i].strength == b.bonds[i].strength);
    }
}

NF_TEST(fracture_asset_different_seeds_give_different_shards) {
    const FracturePiece cube = unit_cube();

    FractureParams lo;
    lo.seed = 1u;
    lo.target_chunks = 8u;

    FractureParams hi = lo;
    hi.seed = 99999u;

    FractureAsset a, b;
    build_fracture_asset(cube, lo, a);
    build_fracture_asset(cube, hi, b);

    NF_CHECK(!a.bonds.empty() && !b.bonds.empty());
    // The first cut is a pure function of the seed, so different seeds cannot
    // agree on where the object comes apart.
    NF_CHECK(!a.bonds[0u].plane_normal.nearly_equals(b.bonds[0u].plane_normal, 1e-4f));
}

NF_TEST(fracture_asset_respects_the_depth_ceiling) {
    FractureParams params;
    params.max_depth = 2u;          // at most 2^2 leaves
    params.target_chunks = 64u;     // asked for far more than that

    FractureAsset asset;
    build_fracture_asset(unit_cube(), params, asset);

    NF_CHECK(asset.leaf_count() >= 2u);
    NF_CHECK(asset.leaf_count() <= 4u);
    for (const FractureChunk& chunk : asset.chunks) {
        NF_CHECK(chunk.depth <= 2u);
    }
}

NF_TEST(fracture_asset_single_chunk_when_target_is_one) {
    FractureParams params;
    params.target_chunks = 1u;

    FractureAsset asset;
    build_fracture_asset(unit_cube(), params, asset);

    NF_CHECK(asset.chunks.size() == 1u);
    NF_CHECK(asset.bonds.empty());
    NF_CHECK(asset.leaf_count() == 1u);
}

NF_TEST(fracture_asset_reports_degenerate_sources) {
    FracturePiece flat;
    // Four coplanar points: three vertices would fail the count check, but
    // these pass it and still bound no volume.
    flat.vertices = {
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.5f, 0.5f, 0.0f},
    };
    flat.indices = { 0u, 1u, 3u, 0u, 3u, 2u };

    FractureAsset asset;
    std::string error;
    build_fracture_asset(flat, FractureParams{}, asset, &error);

    NF_CHECK(asset.empty());
    NF_CHECK(!error.empty());
}

NF_TEST(fracture_asset_bond_describes_the_shared_face) {
    FractureParams params;
    params.target_chunks = 2u;
    params.strength_per_area = 10.0f;

    FractureAsset asset;
    build_fracture_asset(unit_cube(), params, asset);

    NF_CHECK(asset.bonds.size() == 1u);
    const FractureBond& bond = asset.bonds[0u];

    // Strength is area-driven, so a bigger face holds harder.
    NF_CHECK(bond.area > 0.0f);
    NF_CHECK_NEAR(bond.strength, bond.area * 10.0f, 1e-4f);

    // The bond normal points from hold to detach, so the detach chunk's
    // centroid reads positive on the bond's own plane and the hold chunk's
    // reads negative — the one property the runtime needs to know which way
    // debris flies.
    const FractureChunk& detach = asset.chunks[bond.detach_chunk];
    const FractureChunk& hold = asset.chunks[bond.hold_chunk];
    const f32 d_detach = bond.plane_normal.dot(detach.centroid) + bond.plane_d;
    const f32 d_hold = bond.plane_normal.dot(hold.centroid) + bond.plane_d;
    NF_CHECK(d_detach >= 0.0f);
    NF_CHECK(d_hold <= 0.0f);

    // The two halves are still the whole object.
    NF_CHECK_NEAR(detach.volume + hold.volume, 8.0f, 1e-4f);
}

NF_TEST(fracture_bonds_release_leaves_inside_their_subtree) {
    FractureParams params;
    params.target_chunks = 8u;

    FractureAsset asset;
    build_fracture_asset(unit_cube(), params, asset);

    std::vector<u32> leaves;
    asset.collect_leaves(0u, leaves);
    NF_CHECK(leaves.size() >= 4u);

    std::vector<u32> bonds;
    asset.bonds_of(0u, bonds);
    NF_CHECK(!bonds.empty());

    for (const u32 bond_id : bonds) {
        const FractureBond& bond = asset.bonds[bond_id];
        std::vector<u32> released;
        asset.collect_leaves(bond.detach_chunk, released);
        NF_CHECK(!released.empty());

        // Everything the bond releases is still part of the object.
        for (const u32 leaf : released) {
            bool in_tree = false;
            for (const u32 candidate : leaves) in_tree = in_tree || (candidate == leaf);
            NF_CHECK(in_tree);
        }
    }
}
