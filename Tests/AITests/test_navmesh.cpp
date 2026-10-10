// AITests — Recast-style voxel navigation mesh: voxelisation, walkable
// classification, regions, polygon cover, off-mesh links, pathing and the
// determinism contract. CPU-only, so it runs anywhere.

#include <NF/AI/NavMesh.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <functional>
#include <vector>

using namespace nf;
using namespace nf::ai;

namespace {

// A flat floor at y = ground_y across the whole area. The canonical fixture:
// every span walkable, the whole area one region.
NavMesh::HeightSampler flat_floor(f32 ground_y) {
    return [ground_y](f32, f32) { return ground_y; };
}

// A plain config suited to small test areas. min_region_area is tiny so even a
// small island forms a region and its structure stays observable.
NavMesh::Config small_cfg() {
    NavMesh::Config c;
    c.cell_size = 0.5f;
    c.cell_height = 0.25f;
    c.walkable_slope_deg = 45.0f;
    c.walkable_climb = 0.5f;
    c.walkable_height = 2.0f;
    c.min_region_area = 0.5f;
    c.agent_radius = 0.0f;
    c.jump_distance = 4.0f;
    c.jump_height = 1.5f;
    c.max_verts_per_poly = 6;
    return c;
}

f32 path_length(const std::vector<Vec3>& p) {
    f32 len = 0.0f;
    for (size_t i = 1; i < p.size(); ++i) len += (p[i] - p[i - 1]).length();
    return len;
}

bool segments_on_mesh(const NavMesh& mesh, const std::vector<Vec3>& path) {
    for (size_t i = 1; i < path.size(); ++i)
        if (!mesh.raycast(path[i - 1], path[i])) return false;
    return true;
}

} // namespace

// --- Build & degenerate input ----------------------------------------------

NF_TEST(navmesh_build_flat_floor_single_region) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{4, 4, 4}, flat_floor(0.0f)));
    NF_CHECK(mesh.built());
    NF_CHECK(!mesh.empty());
    NF_CHECK_EQ(mesh.region_count(), 1u);
    NF_CHECK(mesh.polygon_count() >= 1u);
    NF_CHECK(mesh.voxel_count() > 0u);
    // Introspection mirrors the build area.
    NF_CHECK_NEAR(mesh.area_min().x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(mesh.area_max().x, 4.0f, 1e-4f);
    NF_CHECK_EQ(mesh.grid_w(), 8);
    NF_CHECK_EQ(mesh.grid_h(), 8);
    NF_CHECK_NEAR(mesh.cell_size(), 0.5f, 1e-4f);
    NF_CHECK_NEAR(mesh.cell_height(), 0.25f, 1e-4f);
}

NF_TEST(navmesh_degenerate_input_fails_and_stays_empty) {
    NavMesh mesh;
    NF_CHECK(!mesh.build(small_cfg(), Vec3{2, 0, 0}, Vec3{2, 4, 4}, flat_floor(0.0f)));
    NF_CHECK(!mesh.built());
    NF_CHECK(mesh.empty());
    NF_CHECK(!mesh.build(small_cfg(), Vec3{4, 0, 0}, Vec3{0, 4, 4}, flat_floor(0.0f)));
    NF_CHECK(mesh.empty());
    NF_CHECK(!mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{4, 4, 4}, NavMesh::HeightSampler{}));
    NF_CHECK(mesh.empty());
    NavMesh::Config bad = small_cfg();
    bad.cell_size = 0.0f;
    NF_CHECK(!mesh.build(bad, Vec3{0, 0, 0}, Vec3{4, 4, 4}, flat_floor(0.0f)));
    NF_CHECK(mesh.empty());
}

// --- Determinism -----------------------------------------------------------

NF_TEST(navmesh_same_as_is_bit_identical) {
    NavMesh a, b;
    NF_CHECK(a.build(small_cfg(), Vec3{0, 0, 0}, Vec3{6, 4, 6}, flat_floor(0.0f)));
    NF_CHECK(b.build(small_cfg(), Vec3{0, 0, 0}, Vec3{6, 4, 6}, flat_floor(0.0f)));
    NF_CHECK(a.same_as(b));
    NF_CHECK(a.rebuild());
    NF_CHECK(a.same_as(b));
    NavMesh c;
    c.build(small_cfg(), Vec3{0, 0, 0}, Vec3{5, 4, 6}, flat_floor(0.0f));
    NF_CHECK(!a.same_as(c));
}

// --- Obstacles -------------------------------------------------------------

NF_TEST(navmesh_obstacle_carves_floor) {
    std::vector<NavObstacle> obs;
    obs.push_back(NavObstacle{Vec3{1.75f, -1.0f, 1.75f}, Vec3{2.25f, 3.0f, 2.25f}});
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{4, 4, 4}, flat_floor(0.0f), obs));
    NF_CHECK(mesh.built());
    NF_CHECK(mesh.polygon_count() >= 2u);
}

NF_TEST(navmesh_obstacle_center_off_mesh) {
    const NavMesh::Config cfg = small_cfg();
    std::vector<NavObstacle> obstacles;
    obstacles.push_back(NavObstacle{Vec3{3.5f, 0, 3.5f}, Vec3{4.5f, 3, 4.5f}});
    NavMesh with_block;
    NF_CHECK(with_block.build(cfg, Vec3{0, 0, 0}, Vec3{8, 3, 8}, flat_floor(0.0f), obstacles));
    NavMesh plain;
    NF_CHECK(plain.build(cfg, Vec3{0, 0, 0}, Vec3{8, 3, 8}, flat_floor(0.0f)));
    NF_CHECK(!with_block.same_as(plain));
    u32 poly = NavMesh::kNone;
    Vec3 pt{};
    NF_CHECK(!with_block.locate(Vec3{4, 0, 4}, poly, pt));
}

NF_TEST(navmesh_set_obstacles_rebuilds) {
    NavMesh mesh;
    const NavMesh::Config cfg = small_cfg();
    NF_CHECK(mesh.build(cfg, Vec3{0, 0, 0}, Vec3{8, 3, 8}, flat_floor(0.0f)));
    NavMesh snapshot = mesh;
    NF_CHECK(mesh.same_as(snapshot));
    std::vector<NavObstacle> obstacles;
    obstacles.push_back(NavObstacle{Vec3{3.5f, 0, 3.5f}, Vec3{4.5f, 3, 4.5f}});
    mesh.set_obstacles(obstacles);
    NF_CHECK(mesh.built());
    NF_CHECK(!mesh.same_as(snapshot));
}

// --- Queries ---------------------------------------------------------------

NF_TEST(navmesh_locate_on_floor) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{4, 4, 4}, flat_floor(0.0f)));
    u32 poly = NavMesh::kNone;
    Vec3 pt{};
    NF_CHECK(mesh.locate(Vec3{2.0f, 0.0f, 2.0f}, poly, pt));
    NF_CHECK(poly < mesh.polygon_count());
    NF_CHECK_NEAR(pt.y, 0.0f, 1e-3f);
    NF_CHECK(!mesh.locate(Vec3{2.0f, 5.0f, 2.0f}, poly, pt));
    NF_CHECK(!mesh.locate(Vec3{-3.0f, 0.0f, 2.0f}, poly, pt));
}

NF_TEST(navmesh_closest_point_beside_mesh) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{4, 3, 4}, flat_floor(0.0f)));
    Vec3 out{};
    NF_CHECK(mesh.closest_point(Vec3{-5, 0, 2}, out));
    NF_CHECK(out.x >= -1e-3f);
    NF_CHECK_NEAR(out.z, 2.0f, 1e-3f);
    NF_CHECK(mesh.closest_point(Vec3{2.0f, 10.0f, 2.0f}, out));
    NF_CHECK_NEAR(out.y, 0.0f, 1e-3f);
}

NF_TEST(navmesh_raycast_across_open_floor) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{8, 3, 8}, flat_floor(0.0f)));
    NF_CHECK(mesh.raycast(Vec3{1, 0, 4}, Vec3{7, 0, 4}));
    NF_CHECK(!mesh.raycast(Vec3{1, 0, 4}, Vec3{20, 0, 4}));
    NF_CHECK(!mesh.raycast(Vec3{1, 3, 4}, Vec3{7, 3, 4}));
}

NF_TEST(navmesh_find_path_open_floor_straight) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{8, 4, 8}, flat_floor(0.0f)));
    std::vector<Vec3> path;
    NF_CHECK(mesh.find_path(Vec3{1.0f, 0, 1.0f}, Vec3{6.5f, 0, 6.5f}, path));
    NF_CHECK(path.size() >= 2u);
    NF_CHECK_EQ(path.size(), 2u);
    NF_CHECK_NEAR(path.front().x, 1.0f, 1e-4f);
    NF_CHECK_NEAR(path.back().x, 6.5f, 1e-4f);
    NF_CHECK(path_length(path) > 1.0f);
    NF_CHECK(segments_on_mesh(mesh, path));
}

NF_TEST(navmesh_find_path_deterministic) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{8, 4, 8}, flat_floor(0.0f)));
    std::vector<Vec3> a, b;
    NF_CHECK(mesh.find_path(Vec3{1.0f, 0, 1.0f}, Vec3{6.5f, 0, 6.5f}, a));
    NF_CHECK(mesh.find_path(Vec3{1.0f, 0, 1.0f}, Vec3{6.5f, 0, 6.5f}, b));
    NF_CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        NF_CHECK_NEAR(a[i].x, b[i].x, 1e-6f);
        NF_CHECK_NEAR(a[i].z, b[i].z, 1e-6f);
    }
}

NF_TEST(navmesh_find_path_endpoint_off_mesh_fails) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{4, 4, 4}, flat_floor(0.0f)));
    std::vector<Vec3> path;
    NF_CHECK(!mesh.find_path(Vec3{1, 0, 1}, Vec3{40, 0, 40}, path));
    NF_CHECK(path.empty());
}

// --- Regions & step vs cliff ----------------------------------------------

NF_TEST(navmesh_step_within_climb_is_one_region) {
    const auto stepped = [](f32 x, f32) { return x < 4.0f ? 0.0f : 0.5f; };
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{8, 3, 4}, stepped));
    NF_CHECK_EQ(mesh.region_count(), 1u);
}

NF_TEST(navmesh_cliff_over_climb_splits_regions) {
    const auto cliff = [](f32 x, f32) { return x < 4.0f ? 0.0f : 2.0f; };
    NavMesh mesh;
    // Ceiling high enough that the upper plateau keeps headroom (ground 2.0 vs
    // walkable_height 2.0): otherwise the upper side is correctly culled as a
    // tunnel, not a cliff.
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{8, 5, 4}, cliff));
    NF_CHECK_EQ(mesh.region_count(), 2u);
}

// --- Links -----------------------------------------------------------------

NF_TEST(navmesh_auto_link_bridges_a_gap) {
    const NavMesh::Config cfg = small_cfg();
    const auto gap_ground = [](f32 x, f32) -> f32 {
        if (x < 3.0f) return 0.0f;
        if (x > 5.0f) return 0.0f;
        return -10.0f;
    };
    NavMesh mesh;
    NF_CHECK(mesh.build(cfg, Vec3{0, -1, 0}, Vec3{8, 3, 8}, gap_ground));
    NF_CHECK_EQ(mesh.region_count(), 2u);
    NF_CHECK(mesh.auto_link_count() >= 1u);
    std::vector<Vec3> path;
    NF_CHECK(mesh.find_path(Vec3{1, 0, 4}, Vec3{7, 0, 4}, path));
    NF_CHECK(!path.empty());
}

NF_TEST(navmesh_authored_link_snaps_and_paths) {
    const NavMesh::Config cfg = small_cfg();
    const auto gap_ground = [](f32 x, f32) -> f32 {
        return (x < 3.0f || x > 5.0f) ? 0.0f : -10.0f;
    };
    std::vector<NavLink> links;
    links.push_back(NavLink{Vec3{1, 0, 4}, Vec3{7, 0, 4}, true});
    NavMesh mesh;
    NF_CHECK(mesh.build(cfg, Vec3{0, -1, 0}, Vec3{8, 3, 8}, gap_ground, {}, links));
    NF_CHECK(mesh.link_count() >= 1u);
    NF_CHECK_EQ(mesh.dropped_link_count(), 0u);
    std::vector<Vec3> path;
    NF_CHECK(mesh.find_path(Vec3{1, 0, 4}, Vec3{7, 0, 4}, path));
    NF_CHECK(!path.empty());
}

NF_TEST(navmesh_authored_link_off_mesh_is_dropped) {
    const NavMesh::Config cfg = small_cfg();
    std::vector<NavLink> links;
    links.push_back(NavLink{Vec3{1, 0, 4}, Vec3{60, 25, 60}, true});
    NavMesh mesh;
    NF_CHECK(mesh.build(cfg, Vec3{0, 0, 0}, Vec3{8, 3, 8}, flat_floor(0.0f), {}, links));
    NF_CHECK_EQ(mesh.dropped_link_count(), 1u);
    NF_CHECK_EQ(mesh.link_count(), 0u);
}

NF_TEST(navmesh_find_path_does_not_cut_a_corner_through_a_hole) {
    // The smoothing bug this pins: the raw start and goal were pinned onto the
    // smoothed centre chain WITHOUT a line-of-sight check, so a straight line
    // between them could thread a hole the corridor had walked around — a
    // wall in the middle of the area was simply crossed. Every consecutive
    // pair of the result is now an LOS-verified edge, which is the property
    // that makes "straight" and "on the mesh" the same thing.
    const NavMesh::Config cfg = small_cfg();
    std::vector<NavObstacle> obstacles;
    // A wall straight across the middle: from (1,0,1) to (10,0,10) the only way
    // is around it, and the direct line passes through its footprint.
    obstacles.push_back(NavObstacle{Vec3{3.0f, -1.0f, 5.75f}, Vec3{9.0f, 1.0f, 6.25f}});
    NavMesh mesh;
    NF_CHECK(mesh.build(cfg, Vec3{0, 0, 0}, Vec3{12, 4, 12}, flat_floor(0.0f), obstacles));
    NF_CHECK(mesh.polygon_count() >= 2u);

    std::vector<Vec3> path;
    NF_CHECK(mesh.find_path(Vec3{1.0f, 0.0f, 1.0f}, Vec3{10.0f, 0.0f, 10.0f}, path));
    NF_CHECK(path.size() >= 2u);
    // Ends are the raw endpoints (the followers track them), and every leg
    // stays on the mesh — the corner cannot be cut.
    NF_CHECK_NEAR(path.front().x, 1.0f, 1e-4f);
    NF_CHECK_NEAR(path.back().x, 10.0f, 1e-4f);
    NF_CHECK(segments_on_mesh(mesh, path));
    // No waypoint may sit inside the wall footprint: the detour goes around,
    // not through.
    for (const Vec3& w : path) {
        const bool inside_wall = w.x > 3.05f && w.x < 8.95f && w.z > 5.7f && w.z < 6.3f;
        NF_CHECK(!inside_wall);
    }
}

// --- rebuild & clear -------------------------------------------------------

NF_TEST(navmesh_rebuild_is_bit_identical) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{8, 3, 8}, flat_floor(0.0f)));
    NavMesh snapshot = mesh;
    NF_CHECK(mesh.rebuild());
    NF_CHECK(mesh.same_as(snapshot));
}

NF_TEST(navmesh_clear_resets_state) {
    NavMesh mesh;
    NF_CHECK(mesh.build(small_cfg(), Vec3{0, 0, 0}, Vec3{4, 4, 4}, flat_floor(0.0f)));
    NF_CHECK(mesh.built());
    mesh.clear();
    NF_CHECK(!mesh.built());
    NF_CHECK(mesh.empty());
    NF_CHECK_EQ(mesh.polygon_count(), 0u);
    NF_CHECK_EQ(mesh.region_count(), 0u);
    NF_CHECK(!mesh.rebuild());
}
