#pragma once

// NF/AI/NavMesh.hpp — Recast-style voxel navigation mesh (design doc Section
// 45, "Navigation": NavMesh, Recast-style generation, dynamic obstacles,
// off-mesh links).
//
// NavGrid answers "can I walk from A to B on a flat grid". A real world is not
// flat: terrain rises, a ledge is a wall to an agent that cannot climb it, a
// box is a new floor you can stand on, and a gap between two rooftops is
// crossable only by a link the designer marked as a jump. This module builds
// that geometry and answers queries against it.
//
// The pipeline (each stage is a separate pass, each testable on its own)
// -----------------------------------------------------------------------
//   1. Voxelize   — the build area is cut into columns of `cell_size` on a
//                   side. Each column holds the solid spans found there: the
//                   terrain sampled through `HeightSampler`, plus every
//                   obstacle AABB overlapping the column. Overlapping spans
//                   merge, so a crate sitting on the ground is one span whose
//                   top is the crate lid — the ground simply *rises* there —
//                   while a ledge floating above the floor keeps its own span
//                   and leaves a headroom-limited tunnel beneath it.
//                   Y is quantised to `cell_height` voxels at this stage and
//                   stays integer throughout region work, so no float
//                   arithmetic accumulates into the region topology.
//   2. Walkable   — a span is walkable when an agent can stand on it: enough
//                   headroom to the span above (or the sky), and ground flat
//                   enough in every of the 8 neighbours that the slope test
//                   passes. Steep ground is a wall, not a floor.
//   3. Regions    — flood-fill over (column, span) pairs. Two spans in
//                   neighbouring columns are connected when both are walkable,
//                   their vertical intervals overlap by at least one voxel, and
//                   their tops are within `walkable_climb` of each other — the
//                   step an agent can take. Regions are seeded in raster scan
//                   order so their ids are reproducible; islands smaller than
//                   `min_region_area` are dropped as unreachable decoration.
//   4. Polygons   — each region's cells are decomposed into axis-aligned
//                   rectangles (a cover, exact and disjoint), then adjacent
//                   rectangles are merged while the union stays convex and
//                   within `max_verts_per_poly`. The corner heights come from
//                   the region's own voxels, so a polygon on a slope is a
//                   bilinear patch, not a flat slab.
//   5. Links      — authored `NavLink`s (jump, climb, ladder, teleport pad)
//                   snap their endpoints onto polygons, and the mesh adds its
//                   own auto links between regions separated by a gap or cliff
//                   inside the `jump_distance`/`jump_height` budget.
//
// A note on the polygoniser
// -------------------------
// Recast's watershed region partition, contour tracing and convex
// polygonisation are the reference implementation, and they are not here. The
// reason is the engine's determinism contract, which is a hard requirement and
// not a nice-to-have: replays must be bit-identical, and the watershed's
// expansion order and contour vertex selection depend on tie-breaks that are
// easy to get subtly wrong. The decomposition used instead — raster-seeded
// flood-fill plus maximal-rectangle cover plus convex merge — has a total order
// at every step, produces an exact cover of the same walkable set, and costs a
// few more polygons than Recast would. Those polygons are axis-aligned, so the
// raw corridor hugs grid lines; the funnel and line-of-sight smoothing applied
// afterwards is what makes the path a straight line again, and the player
// never sees the grid.
//
// Dynamic obstacles
// -----------------
// There is no incremental obstacle carving. Changing the obstacle list
// rebuilds the mesh from the stored sampler and area, which is the honest
// answer for a voxel pipeline (Recast tiles rebuild too) and is cheap enough
// at tile scale. Because the pipeline is deterministic, an unchanged obstacle
// set produces a bit-identical mesh — `same_as` is the assertion.
//
// Determinism
// -----------
// Same contract as NavGrid, Perception and AIWorld:
//   - columns, spans and polygons are visited in index order, never pointer
//     or hash order; region ids come from a raster scan;
//   - the A* heap breaks ties on (f, h, insertion sequence), and the neighbour
//     order is fixed, so identical input yields identical waypoints;
//   - no RNG and no wall clock. The only time input is `dt` and this module
//     does not take one.
//
// Units: world metres; angles in degrees; `cell_size`/`cell_height` in metres.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <functional>
#include <vector>

namespace nf::ai {

/// An axis-aligned solid carved into (or added onto) the world. Overlapping
/// obstacles behave as their union, because the voxeliser merges spans.
struct NavObstacle {
    Vec3 min{};
    Vec3 max{};

    bool operator==(const NavObstacle& o) const = default;
};

/// A connection the walkable surface does not provide on its own: a jump
/// across a gap, a climb up a ledge, a ladder, a teleport pad. `start` and
/// `end` are snapped onto polygons at build time; a link whose endpoint is not
/// on the mesh is dropped (and counted in `dropped_link_count()`) rather than
/// silently teleporting an agent into the void.
struct NavLink {
    Vec3 start{};
    Vec3 end{};
    /// Both directions when true. A one-way link is a drop-down or a
    /// jump-up-only ledge: A* honours the direction, so an agent does not path
    /// back up a pit it jumped into.
    bool bidirectional = true;
};

/// One convex polygon of the mesh. Vertices are ordered around the polygon
/// (counter-clockwise in the XZ plane) and carry their own height, so a
/// polygon spanning a slope is a bilinear patch rather than a flat slab.
/// `neighbours[i]` is the polygon across edge (i, i+1), or `kNone`.
struct NavPoly {
    std::vector<Vec3> verts;
    std::vector<u32> neighbours;
    u32 region = 0;

    bool operator==(const NavPoly& o) const = default;
};

class NavMesh {
public:
    struct Config {
        f32 cell_size = 0.5f;         ///< Voxel width/depth (XZ).
        f32 cell_height = 0.25f;      ///< Voxel height (Y). Quantises spans.
        f32 walkable_slope_deg = 45.0f; ///< Steeper ground is a wall.
        f32 walkable_climb = 0.5f;    ///< Max step between adjacent spans.
        f32 walkable_height = 2.0f;   ///< Min headroom above a walkable top.
        f32 min_region_area = 2.0f;   ///< Min walkable area, world metres^2.
        f32 agent_radius = 0.0f;      ///< Obstacles are inflated by this.
        f32 jump_distance = 4.0f;     ///< Auto link: horizontal reach.
        f32 jump_height = 1.5f;       ///< Auto link: vertical reach.
        u32 max_verts_per_poly = 6;   ///< Convex merge ceiling.
    };

    /// Pure height query: world y of the solid surface at (x, z). A terrain
    /// heightfield, a tile callback, a test fixture — anything that maps a
    /// point to a height. Must be deterministic: the same (x, z) always yields
    /// the same y, or the mesh is not reproducible.
    using HeightSampler = std::function<f32(f32 x, f32 z)>;

    static constexpr u32 kNone = ~0u;

    NavMesh() = default;

    /// Voxelize `[area_min, area_max]` and build the mesh. `area_min.y` is the
    /// floor every terrain span stands on; `area_max.y` is the sky headroom is
    /// measured against. Degenerate input (zero or negative extents, a sampler
    /// that is not callable, a non-positive cell size) fails and leaves the
    /// mesh empty rather than building something that looks like a mesh.
    bool build(const Config& cfg,
               Vec3 area_min,
               Vec3 area_max,
               const HeightSampler& sampler,
               const std::vector<NavObstacle>& obstacles = {},
               const std::vector<NavLink>& links = {});

    /// Re-runs the last `build` with the current obstacles/links. This is the
    /// dynamic-obstacle entry point: move a crate, call it, path again.
    /// Returns false if there was nothing to rebuild.
    bool rebuild();

    /// Replaces the obstacle set and rebuilds. Convenience for the common case
    /// where the area, sampler and links do not change.
    void set_obstacles(const std::vector<NavObstacle>& obstacles);

    /// Replaces the authored link set and rebuilds.
    void set_links(const std::vector<NavLink>& links);

    void clear();

    // --- queries ----------------------------------------------------------

    /// Finds the polygon `world` stands on, projecting the point onto it.
    /// Returns false when the point is off the mesh or too far above/below its
    /// surface (the tolerance is `walkable_climb` — a point a step away is on
    /// it, a point on a rooftop above is not).
    bool locate(Vec3 world, u32& out_poly, Vec3& out_point) const;

    /// Nearest point on the mesh to `world`, in world space. A point already on
    /// the mesh projects onto its polygon; one beside the mesh lands on the
    /// nearest edge. Returns false for an empty mesh.
    bool closest_point(Vec3 world, Vec3& out) const;

    /// A* over polygons and off-mesh links, funnel-smoothed, then shortcut by
    /// on-mesh line-of-sight. Waypoints start at `start` and end at `goal`,
    /// both snapped onto the mesh first (so a slightly buried start still
    /// paths). Returns false when either endpoint is off the mesh or no route
    /// exists.
    bool find_path(Vec3 start, Vec3 goal, std::vector<Vec3>& out_waypoints) const;

    /// Straight-line traversal on the mesh: every point of the segment lies on
    /// a polygon, and the polygons it passes through form a connected chain.
    /// This is the smoothing probe, and the corner-cutting guard: a shortcut
    /// that threads a hole is not a shortcut.
    bool raycast(Vec3 a, Vec3 b) const;

    // --- introspection ----------------------------------------------------

    u32 polygon_count() const { return static_cast<u32>(m_polys.size()); }
    u32 region_count() const { return m_region_count; }
    u32 link_count() const { return static_cast<u32>(m_links.size()); }
    u32 auto_link_count() const { return m_auto_link_count; }
    u32 dropped_link_count() const { return m_dropped_links; }
    u32 voxel_count() const { return m_voxel_count; }

    const NavPoly& polygon(u32 i) const { return m_polys[i]; }
    const NavLink& link(u32 i) const { return m_links[i].spec; }
    bool link_is_auto(u32 i) const { return m_links[i].auto_generated; }
    u32 link_start_poly(u32 i) const { return m_links[i].start_poly; }
    u32 link_end_poly(u32 i) const { return m_links[i].end_poly; }

    bool empty() const { return m_polys.empty(); }
    bool built() const { return m_built; }

    Vec3 area_min() const { return m_area_min; }
    Vec3 area_max() const { return m_area_max; }
    i32 grid_w() const { return m_grid_w; }
    i32 grid_h() const { return m_grid_h; }
    f32 cell_size() const { return m_cell_size; }
    f32 cell_height() const { return m_cell_height; }

    /// Bit-identical mesh comparison — the determinism assertion. Two meshes
    /// built from identical input compare equal; a single reordered tie-break,
    /// a single float that drifted, does not.
    bool same_as(const NavMesh& other) const;

private:
    struct LinkRecord {
        NavLink spec{};
        u32 start_poly = kNone;
        u32 end_poly = kNone;
        bool auto_generated = false;
    };

    /// True when the stored build inputs are usable.
    bool can_rebuild() const;

    std::vector<NavPoly> m_polys;
    std::vector<LinkRecord> m_links;

    Config m_cfg{};
    Vec3 m_area_min{};
    Vec3 m_area_max{};
    HeightSampler m_sampler;
    std::vector<NavObstacle> m_obstacles;
    std::vector<NavLink> m_authored_links;

    i32 m_grid_w = 0;
    i32 m_grid_h = 0;
    f32 m_cell_size = 0.5f;
    f32 m_cell_height = 0.25f;
    u32 m_region_count = 0;
    u32 m_auto_link_count = 0;
    u32 m_dropped_links = 0;
    u32 m_voxel_count = 0;
    bool m_built = false;
};

} // namespace nf::ai
