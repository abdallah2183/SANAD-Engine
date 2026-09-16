// AITests — grid A* pathfinding: reachability, obstacles, smoothing.

#include <NF/AI/NavGrid.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::ai;

namespace {

bool segments_walkable(const NavGrid& grid, const std::vector<Vec3>& path) {
    for (size_t i = 1; i < path.size(); ++i) {
        if (!grid.has_line_of_sight(path[i - 1], path[i])) return false;
    }
    return true;
}

} // namespace

NF_TEST(nav_open_grid_goes_straight) {
    NavGrid grid(10, 10, 1.0f);
    std::vector<Vec3> path;
    NF_CHECK(grid.find_path(Vec3{0.5f, 0, 0.5f}, Vec3{8.5f, 0, 8.5f}, path));
    NF_CHECK(path.size() == 2); // smoothed to a straight shot
    NF_CHECK_NEAR(path.front().x, 0.5f, 1e-4f);
    NF_CHECK_NEAR(path.back().x, 8.5f, 1e-4f);
}

NF_TEST(nav_wall_forces_detour) {
    NavGrid grid(10, 10, 1.0f);
    for (int z = 0; z < 9; ++z) grid.set_walkable(5, z, false); // wall with a gap at z=9
    std::vector<Vec3> path;
    NF_CHECK(grid.find_path(Vec3{1.5f, 0, 4.5f}, Vec3{8.5f, 0, 4.5f}, path));
    NF_CHECK(path.size() >= 3); // must route around through the gap
    NF_CHECK(segments_walkable(grid, path));
    // The detour passes the gap row (z ~ 9.5).
    bool visits_gap = false;
    for (const auto& w : path) {
        if (w.z > 8.5f) visits_gap = true;
    }
    NF_CHECK(visits_gap);
    // Path length beats walking the wall: detour < 30 units.
    float len = 0;
    for (size_t i = 1; i < path.size(); ++i) len += (path[i] - path[i - 1]).length();
    NF_CHECK(len < 30.0f);
    NF_CHECK(len > 7.0f); // ...but longer than the straight 7
}

NF_TEST(nav_unreachable_returns_false) {
    NavGrid grid(5, 5, 1.0f);
    for (int x = 0; x < 5; ++x)
        for (int z = 0; z < 5; ++z) grid.set_walkable(x, z, false);
    grid.set_walkable(0, 0, true);
    grid.set_walkable(4, 4, true);
    std::vector<Vec3> path;
    NF_CHECK(!grid.find_path(Vec3{0.5f, 0, 0.5f}, Vec3{4.5f, 0, 4.5f}, path));
    NF_CHECK(path.empty());
    // Blocked endpoints also fail.
    NF_CHECK(!grid.find_path(Vec3{2.5f, 0, 2.5f}, Vec3{4.5f, 0, 4.5f}, path));
    NF_CHECK(!grid.find_path(Vec3{0.5f, 0, 0.5f}, Vec3{2.5f, 0, 2.5f}, path));
    // Outside the grid fails too.
    NF_CHECK(!grid.find_path(Vec3{-5.0f, 0, 0.5f}, Vec3{4.5f, 0, 4.5f}, path));
}

NF_TEST(nav_same_cell_returns_single_waypoint) {
    NavGrid grid(4, 4, 2.0f);
    std::vector<Vec3> path;
    NF_CHECK(grid.find_path(Vec3{0.5f, 0, 0.5f}, Vec3{1.5f, 0, 1.5f}, path));
    NF_CHECK(path.size() == 1);
}

NF_TEST(nav_no_corner_cutting) {
    NavGrid grid(3, 3, 1.0f);
    // Block (1,0): the diagonal shortcut (0,0)->(1,1) would graze its corner.
    // A* must route around, and smoothing must not reintroduce the cut.
    grid.set_walkable(1, 0, false);
    NF_CHECK(!grid.has_line_of_sight(Vec3{0.5f, 0, 0.5f}, Vec3{1.5f, 0, 1.5f}));
    std::vector<Vec3> path;
    NF_CHECK(grid.find_path(Vec3{0.5f, 0, 0.5f}, Vec3{2.5f, 0, 2.5f}, path));
    NF_CHECK(segments_walkable(grid, path));
    // Longer than the direct diagonal (2*sqrt(2) ~= 2.83).
    float len = 0;
    for (size_t i = 1; i < path.size(); ++i) len += (path[i] - path[i - 1]).length();
    NF_CHECK(len > 2.9f);
}

NF_TEST(nav_deterministic_across_runs) {
    NavGrid grid(12, 12, 1.0f);
    for (int i = 2; i < 10; ++i) {
        grid.set_walkable(i, 4, false);
        grid.set_walkable(i, 8, false);
    }
    std::vector<Vec3> a, b;
    NF_CHECK(grid.find_path(Vec3{0.5f, 0, 0.5f}, Vec3{11.5f, 0, 11.5f}, a));
    NF_CHECK(grid.find_path(Vec3{0.5f, 0, 0.5f}, Vec3{11.5f, 0, 11.5f}, b));
    NF_CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        NF_CHECK_NEAR(a[i].x, b[i].x, 1e-6f);
        NF_CHECK_NEAR(a[i].z, b[i].z, 1e-6f);
    }
}
