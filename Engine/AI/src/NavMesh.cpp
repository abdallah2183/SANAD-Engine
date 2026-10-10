// NF/AI/NavMesh.cpp — Recast-style voxel navigation mesh.
//
// The pipeline is five passes (voxelize -> walkable -> regions -> polygons ->
// links), each a separate method so it can be reasoned about and tested on its
// own. Determinism is a hard requirement (replays must be bit-identical): the
// raster-seeded flood fill, the fixed 8-neighbour order, the greedy rectangle
// cover and the A* tie-breaks are all spelled out rather than delegated to
// order-dependent std facilities. See NavMesh.hpp for the design rationale.

#include <NF/AI/NavMesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace nf::ai {

namespace {

// Fixed 8-neighbour order (E, W, S, N, then diagonals). Part of the
// determinism contract, matching NavGrid's convention.
constexpr i32 kDirs[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                             {1, 1},  {1, -1}, {-1, 1}, {-1, -1}};

inline usize col_index(i32 x, i32 z, i32 w) {
    return static_cast<usize>(z) * static_cast<usize>(w) + static_cast<usize>(x);
}

inline bool in_bounds(i32 x, i32 z, i32 w, i32 h) {
    return x >= 0 && z >= 0 && x < w && z < h;
}

// True when `p` lies within a millimetre of the polygon's XZ outline.
//
// The point-in-polygon test in locate() is built from strict comparisons, so a
// point ON an edge — or exactly at a corner — falls through to "outside".
// Navigation produces exactly such points by construction: every portal of a
// path is the midpoint of a shared edge, and a rectangle cover meets at corners
// wherever two edges cross. A query that refused the boundary would therefore
// break the corridors it exists to walk, so the outline is checked explicitly
// rather than left to float rounding luck.
//
// A millimetre is a boundary slop, not a walkable extension: a point a real
// distance away is still refused, which is what keeps locate() a query about
// the mesh and not about its neighbourhood.
bool point_on_polygon_boundary_xz(const NavPoly& poly, const Vec3& p) {
    constexpr f32 kEps = 1e-3f;
    const usize vc = poly.verts.size();
    f32 best = std::numeric_limits<f32>::max();
    for (usize i = 0; i < vc; ++i) {
        const Vec3& a = poly.verts[i];
        const Vec3& b = poly.verts[(i + 1) % vc];
        const f32 abx = b.x - a.x;
        const f32 abz = b.z - a.z;
        const f32 ab2 = abx * abx + abz * abz;
        f32 t = 0.0f;
        if (ab2 > 1e-12f) {
            t = ((p.x - a.x) * abx + (p.z - a.z) * abz) / ab2;
            t = std::clamp(t, 0.0f, 1.0f);
        }
        const f32 dx = p.x - (a.x + abx * t);
        const f32 dz = p.z - (a.z + abz * t);
        best = std::min(best, dx * dx + dz * dz);
    }
    return best <= kEps * kEps;
}

} // namespace

// ===========================================================================
// Public build API
// ===========================================================================

bool NavMesh::can_rebuild() const {
    // A build needs a callable sampler, a positive cell size and a non-empty
    // XZ footprint; otherwise the passes would divide by zero or scan nothing.
    return static_cast<bool>(m_sampler) && m_cell_size > 0.0f &&
           m_area_max.x > m_area_min.x && m_area_max.z > m_area_min.z &&
           m_grid_w > 0 && m_grid_h > 0;
}

bool NavMesh::build(const Config& cfg,
                    Vec3 area_min,
                    Vec3 area_max,
                    const HeightSampler& sampler,
                    const std::vector<NavObstacle>& obstacles,
                    const std::vector<NavLink>& links) {
    clear();

    m_cfg = cfg;
    m_area_min = area_min;
    m_area_max = area_max;
    m_sampler = sampler;
    m_obstacles = obstacles;
    m_authored_links = links;
    m_cell_height = cfg.cell_height > 0.0f ? cfg.cell_height : 0.25f;

    // Degenerate input fails and leaves the mesh empty rather than building
    // something that merely looks like a mesh. A non-positive cell size is one
    // of those failures — clamped silently it would divide-by-zero downstream.
    if (!static_cast<bool>(m_sampler)) return false;
    if (cfg.cell_size <= 0.0f) return false;
    if (m_area_max.x <= m_area_min.x || m_area_max.z <= m_area_min.z) return false;
    m_cell_size = cfg.cell_size;

    m_grid_w = static_cast<i32>(std::floor((m_area_max.x - m_area_min.x) / m_cell_size));
    m_grid_h = static_cast<i32>(std::floor((m_area_max.z - m_area_min.z) / m_cell_size));
    if (m_grid_w < 1 || m_grid_h < 1) {
        m_grid_w = 0;
        m_grid_h = 0;
        return false;
    }

    voxelize();
    mark_walkable();
    build_regions();
    build_polygons(); // also links polygon neighbours
    build_links();
    m_built = true;
    return true;
}

bool NavMesh::rebuild() {
    if (!can_rebuild()) return false;
    // Reset per-build counters and scratch; the passes re-fill everything.
    m_region_count = 0;
    m_auto_link_count = 0;
    m_dropped_links = 0;
    m_voxel_count = 0;
    m_polys.clear();
    m_links.clear();

    voxelize();
    mark_walkable();
    build_regions();
    build_polygons();
    build_links();
    m_built = true;
    return true;
}

void NavMesh::set_obstacles(const std::vector<NavObstacle>& obstacles) {
    m_obstacles = obstacles;
    rebuild();
}

void NavMesh::set_links(const std::vector<NavLink>& links) {
    m_authored_links = links;
    rebuild();
}

void NavMesh::clear() {
    m_cols.clear();
    m_walk.clear();
    m_region_of.clear();
    m_polys.clear();
    m_links.clear();
    m_obstacles.clear();
    m_authored_links.clear();
    m_sampler = nullptr;
    m_grid_w = 0;
    m_grid_h = 0;
    m_region_count = 0;
    m_auto_link_count = 0;
    m_dropped_links = 0;
    m_voxel_count = 0;
    m_built = false;
}



// ===========================================================================
// Pass 1  -  voxelize: sample terrain, carve/union obstacles, merge spans.
// ===========================================================================

void NavMesh::voxelize() {
    const i32 w = m_grid_w;
    const i32 h = m_grid_h;
    m_cols.assign(static_cast<usize>(w) * static_cast<usize>(h), NavColumn{});
    const f32 inv = m_cell_size > 0.0f ? 1.0f / m_cell_size : 0.0f;
    const f32 radius = m_cfg.agent_radius > 0.0f ? m_cfg.agent_radius : 0.0f;

    for (i32 z = 0; z < h; ++z) {
        for (i32 x = 0; x < w; ++x) {
            const f32 wx = m_area_min.x + (static_cast<f32>(x) + 0.5f) * m_cell_size;
            const f32 wz = m_area_min.z + (static_cast<f32>(z) + 0.5f) * m_cell_size;
            const f32 gy = m_sampler(wx, wz);

            std::vector<NavColumn::Span> raw;
            // Terrain solid: from the area floor up to the sampled surface. A
            // ground level with the floor still counts — it is the walkable
            // surface (a zero-thickness slab is a floor, not empty air). Only
            // ground *below* the floor (a pit) is a genuine hole with no span.
            if (gy >= m_area_min.y - 1e-4f) {
                raw.push_back(NavColumn::Span{m_area_min.y, gy, gy, false});
            }
            // Obstacles (agent-radius inflated): solid AABBs overlapping the
            // column centre. Union semantics come from the span merge below.
            for (const NavObstacle& o : m_obstacles) {
                const f32 omin_x = o.min.x - radius;
                const f32 omin_z = o.min.z - radius;
                const f32 omax_x = o.max.x + radius;
                const f32 omax_z = o.max.z + radius;
                if (wx < omin_x || wx > omax_x || wz < omin_z || wz > omax_z) continue;
                if (o.max.y <= o.min.y) continue;
                raw.push_back(NavColumn::Span{o.min.y, o.max.y, o.max.y, false});
            }
            if (raw.empty()) continue;

            // Sort ascending by low (then high) so the merge is well-defined.
            std::sort(raw.begin(), raw.end(), [](const NavColumn::Span& a,
                                                 const NavColumn::Span& b) {
                if (a.low != b.low) return a.low < b.low;
                return a.high < b.high;
            });

            // Merge overlapping / abutting spans. A crate on the ground becomes
            // one span topping at the crate lid; a floating ledge keeps its own
            // span and leaves headroom-limited space beneath it.
            std::vector<NavColumn::Span> merged;
            merged.push_back(raw.front());
            for (usize i = 1; i < raw.size(); ++i) {
                NavColumn::Span& back = merged.back();
                const NavColumn::Span& s = raw[i];
                if (s.low <= back.high + 1e-4f) {
                    back.high = std::max(back.high, s.high);
                    back.top = std::max(back.top, s.top);
                } else {
                    merged.push_back(s);
                }
            }
            const usize nspans = merged.size();
            m_cols[col_index(x, z, w)].spans = std::move(merged);
            m_voxel_count += static_cast<u32>(nspans);
            (void)inv;
        }
    }
}

// ===========================================================================
// Pass 2  -  walkable: standable with enough headroom and flat ground.
// ===========================================================================

void NavMesh::mark_walkable() {
    const i32 w = m_grid_w;
    const i32 h = m_grid_h;
    const f32 max_slope = std::tan(m_cfg.walkable_slope_deg *
                                   3.14159265358979323846f / 180.0f);
    m_walk.assign(static_cast<usize>(w) * static_cast<usize>(h), {});

    for (i32 z = 0; z < h; ++z) {
        for (i32 x = 0; x < w; ++x) {
            const usize ci = col_index(x, z, w);
            const NavColumn& col = m_cols[ci];
            std::vector<bool>& wcol = m_walk[ci];
            wcol.assign(col.spans.size(), false);
            for (usize si = 0; si < col.spans.size(); ++si) {
                const NavColumn::Span& s = col.spans[si];

                // Headroom to the span above (or the sky).
                f32 headroom = m_area_max.y - s.top;
                if (si + 1 < col.spans.size()) headroom = col.spans[si + 1].low - s.top;
                if (headroom < m_cfg.walkable_height - 1e-4f) continue;

                // Slope: the closest neighbouring surface must be flat enough
                // in all 8 directions. Steep ground is a wall, not a floor.
                bool flat = true;
                for (const auto& d : kDirs) {
                    const i32 nx = x + d[0];
                    const i32 nz = z + d[1];
                    if (!in_bounds(nx, nz, w, h)) continue;
                    const NavColumn& nc = m_cols[col_index(nx, nz, w)];
                    const NavColumn::Span* best = nullptr;
                    f32 best_dy = std::numeric_limits<f32>::max();
                    for (const NavColumn::Span& ns : nc.spans) {
                        const f32 dy = std::fabs(ns.top - s.top);
                        if (dy < best_dy) { best_dy = dy; best = &ns; }
                    }
                    if (!best) continue;
                    const f32 rise = std::fabs(best->top - s.top);
                    if (m_cell_size > 0.0f && (rise / m_cell_size) > max_slope + 1e-4f) {
                        flat = false;
                        break;
                    }
                }
                if (!flat) continue;
                wcol[si] = true;
            }
        }
    }
}


// ===========================================================================
// Pass 3  -  regions: flood-fill over (column, span) pairs in raster order.
// ===========================================================================

void NavMesh::build_regions() {
    const i32 w = m_grid_w;
    const i32 h = m_grid_h;
    m_region_of.assign(static_cast<usize>(w) * static_cast<usize>(h), {});

    // Two spans connect when both walkable, their solid intervals overlap by at
    // least one voxel, and their tops are within walkable_climb.
    auto connected = [&](usize ci, i32 si, usize cj, i32 sj) -> bool {
        if (!m_walk[ci][si] || !m_walk[cj][sj]) return false;
        const NavColumn::Span& a = m_cols[ci].spans[si];
        const NavColumn::Span& b = m_cols[cj].spans[sj];
        // Degenerate floor spans (low==high, standing on the area floor) touch
        // any solid interval; otherwise require a full voxel of overlap so
        // adjacent columns of the same walkable surface connect.
        const bool a_degenerate = a.high - a.low < m_cell_height - 1e-4f;
        const bool b_degenerate = b.high - b.low < m_cell_height - 1e-4f;
        if (a_degenerate || b_degenerate) {
            const f32 lo = std::max(a.low, b.low);
            const f32 hi = std::min(a.high, b.high);
            if (hi - lo < -1e-4f) return false;
        } else {
            const f32 lo = std::max(a.low, b.low);
            const f32 hi = std::min(a.high, b.high);
            if (hi - lo < m_cell_height - 1e-4f) return false;
        }
        if (std::fabs(a.top - b.top) > m_cfg.walkable_climb + 1e-4f) return false;
        return true;
    };

    u32 next_region = 1;
    for (i32 z = 0; z < h; ++z) {
        for (i32 x = 0; x < w; ++x) {
            const usize ci = col_index(x, z, w);
            const i32 nspans = static_cast<i32>(m_cols[ci].spans.size());
            if (static_cast<i32>(m_region_of[ci].size()) != nspans) {
                m_region_of[ci].assign(static_cast<usize>(nspans), 0);
            }
            for (i32 si = 0; si < nspans; ++si) {
                if (!m_walk[ci][si] || m_region_of[ci][si] != 0) continue;

                std::vector<std::pair<usize, i32>> stack;
                std::vector<std::pair<usize, i32>> members;
                stack.emplace_back(ci, si);
                m_region_of[ci][si] = next_region;
                members.emplace_back(ci, si);
                while (!stack.empty()) {
                    const std::pair<usize, i32> cur = stack.back();
                    stack.pop_back();
                    const usize ccol = cur.first;
                    const i32 csi = cur.second;
                    const i32 cx = static_cast<i32>(ccol % static_cast<usize>(w));
                    const i32 cz = static_cast<i32>(ccol / static_cast<usize>(w));
                    for (const auto& d : kDirs) {
                        const i32 nx = cx + d[0];
                        const i32 nz = cz + d[1];
                        if (!in_bounds(nx, nz, w, h)) continue;
                        const usize ncol = col_index(nx, nz, w);
                        const i32 nnspans = static_cast<i32>(m_cols[ncol].spans.size());
                        if (static_cast<i32>(m_region_of[ncol].size()) != nnspans) {
                            m_region_of[ncol].assign(static_cast<usize>(nnspans), 0);
                        }
                        for (i32 nsi = 0; nsi < nnspans; ++nsi) {
                            if (!m_walk[ncol][nsi] || m_region_of[ncol][nsi] != 0) continue;
                            if (!connected(ccol, csi, ncol, nsi)) continue;
                            m_region_of[ncol][nsi] = next_region;
                            members.emplace_back(ncol, nsi);
                            stack.emplace_back(ncol, nsi);
                        }
                    }
                }

                // Drop islands smaller than min_region_area.
                const f32 area = static_cast<f32>(members.size()) * m_cell_size * m_cell_size;
                if (area < m_cfg.min_region_area - 1e-4f) {
                    for (const auto& m : members) m_region_of[m.first][m.second] = 0;
                } else {
                    ++m_region_count;
                    ++next_region;
                }
            }
        }
    }
}


// ===========================================================================
// Pass 4  -  polygons: maximal-rectangle cover, convex merge, neighbour link.
// ===========================================================================

void NavMesh::build_polygons() {
    const i32 w = m_grid_w;
    const i32 h = m_grid_h;

    // One walkable in-region span per column: the lowest (deterministic).
    std::vector<u32> cell_region(static_cast<usize>(w) * static_cast<usize>(h), 0);
    std::vector<f32> cell_top(static_cast<usize>(w) * static_cast<usize>(h), 0.0f);
    for (i32 z = 0; z < h; ++z) {
        for (i32 x = 0; x < w; ++x) {
            const usize ci = col_index(x, z, w);
            const i32 nspans = static_cast<i32>(m_cols[ci].spans.size());
            for (i32 si = 0; si < nspans; ++si) {
                if (!m_walk[ci][si]) continue;
                if (si < static_cast<i32>(m_region_of[ci].size()) &&
                    m_region_of[ci][si] != 0) {
                    cell_region[ci] = m_region_of[ci][si];
                    cell_top[ci] = m_cols[ci].spans[si].top;
                    break;
                }
            }
        }
    }

    // Group cells by region (raster order preserved within each region).
    std::vector<std::vector<usize>> region_cells(m_region_count);
    for (i32 z = 0; z < h; ++z) {
        for (i32 x = 0; x < w; ++x) {
            const u32 rid = cell_region[col_index(x, z, w)];
            if (rid == 0 || rid > m_region_count) continue;
            region_cells[rid - 1].push_back(col_index(x, z, w));
        }
    }

    auto cell_x = [&](i32 x) { return m_area_min.x + static_cast<f32>(x) * m_cell_size; };
    auto cell_z = [&](i32 z) { return m_area_min.z + static_cast<f32>(z) * m_cell_size; };
    // Height lookup clamped into the grid: a rectangle's far edge sits on the
    // exclusive bound x1/z1, whose height is taken from the last cell inside it.
    auto top_at = [&](i32 x, i32 z) {
        const i32 cx = std::min(x, w - 1);
        const i32 cz = std::min(z, h - 1);
        return cell_top[col_index(cx, cz, w)];
    };

    struct Rect { i32 x0, x1, z0, z1; };

    for (u32 rid = 1; rid <= m_region_count; ++rid) {
        const std::vector<usize>& cells = region_cells[rid - 1];
        if (cells.empty()) continue;

        std::vector<bool> in(static_cast<usize>(w) * static_cast<usize>(h), false);
        for (usize cc : cells) in[cc] = true;

        // Greedy maximal-rectangle cover in raster order: widest, then tallest,
        // fully inside the region. Exact, disjoint; the merge below trims it.
        std::vector<Rect> rects;
        for (i32 z = 0; z < h; ++z) {
            for (i32 x = 0; x < w; ++x) {
                if (!in[col_index(x, z, w)]) continue;
                i32 x1 = x;
                while (x1 < w && in[col_index(x1, z, w)]) ++x1;
                i32 z1 = z + 1;
                while (z1 < h) {
                    bool full = true;
                    for (i32 xx = x; xx < x1; ++xx)
                        if (!in[col_index(xx, z1, w)]) { full = false; break; }
                    if (!full) break;
                    ++z1;
                }
                rects.push_back(Rect{x, x1, z, z1});
                for (i32 zz = z; zz < z1; ++zz)
                    for (i32 xx = x; xx < x1; ++xx) in[col_index(xx, zz, w)] = false;
                x = x1 - 1;
            }
        }

        // Convex merge: fold two rectangles sharing a full edge when the union
        // is still an axis-aligned rectangle (stays 4 verts, under the cap).
        bool merged_any = true;
        while (merged_any) {
            merged_any = false;
            for (usize a = 0; a < rects.size() && !merged_any; ++a) {
                for (usize b = a + 1; b < rects.size() && !merged_any; ++b) {
                    const Rect& ra = rects[a];
                    const Rect& rb = rects[b];
                    if (ra.z0 == rb.z0 && ra.z1 == rb.z1 && ra.x1 == rb.x0) {
                        rects[a].x1 = rb.x1;
                        rects.erase(rects.begin() + static_cast<isize>(b));
                        merged_any = true;
                    } else if (ra.x0 == rb.x0 && ra.x1 == rb.x1 && ra.z1 == rb.z0) {
                        rects[a].z1 = rb.z1;
                        rects.erase(rects.begin() + static_cast<isize>(b));
                        merged_any = true;
                    }
                }
            }
        }

        // Emit one polygon per rectangle. Corner heights from the region's own
        // cells, so a polygon on a slope is a bilinear patch.
        for (const Rect& r : rects) {
            NavPoly poly;
            poly.region = rid;
            poly.verts.push_back(Vec3{cell_x(r.x0), top_at(r.x0, r.z0), cell_z(r.z0)});
            poly.verts.push_back(Vec3{cell_x(r.x1), top_at(r.x1, r.z0), cell_z(r.z0)});
            poly.verts.push_back(Vec3{cell_x(r.x1), top_at(r.x1, r.z1), cell_z(r.z1)});
            poly.verts.push_back(Vec3{cell_x(r.x0), top_at(r.x0, r.z1), cell_z(r.z1)});
            poly.neighbours.assign(4, kNone);
            m_polys.push_back(std::move(poly));
        }
    }

    link_polygon_neighbours();
}


// Neighbour discovery: two polygons are neighbours when they share a boundary
// edge. The cover is a disjoint rectangle tiling, so each interior grid
// boundary belongs to exactly two polygons; endpoints are quantised to grid
// coordinates so the match is exact and float-free.
void NavMesh::link_polygon_neighbours() {
    const usize n = m_polys.size();

    auto to_grid = [&](const Vec3& v) {
        const i32 gx = static_cast<i32>(std::lround((v.x - m_area_min.x) / m_cell_size));
        const i32 gz = static_cast<i32>(std::lround((v.z - m_area_min.z) / m_cell_size));
        return std::pair<i32, i32>{gx, gz};
    };

    struct Key { i32 ax, az, bx, bz; };
    auto key_of = [&](usize p, u32 e) {
        const NavPoly& poly = m_polys[p];
        const usize vc = poly.verts.size();
        auto a = to_grid(poly.verts[e]);
        auto b = to_grid(poly.verts[(e + 1) % vc]);
        i32 ax = a.first, az = a.second, bx = b.first, bz = b.second;
        if (ax > bx || (ax == bx && az > bz)) { std::swap(ax, bx); std::swap(az, bz); }
        return Key{ax, az, bx, bz};
    };
    auto same_key = [](const Key& a, const Key& b) {
        return a.ax == b.ax && a.az == b.az && a.bx == b.bx && a.bz == b.bz;
    };

    std::vector<std::pair<Key, std::pair<u32, u32>>> boundary;
    boundary.reserve(n * 4);
    for (u32 p = 0; p < n; ++p) {
        const usize vc = m_polys[p].verts.size();
        for (u32 e = 0; e < vc; ++e) boundary.push_back({key_of(p, e), {p, e}});
    }
    for (u32 p = 0; p < n; ++p) {
        NavPoly& poly = m_polys[p];
        const usize vc = poly.verts.size();
        for (u32 e = 0; e < vc; ++e) {
            const Key k = key_of(p, e);
            u32 nb = kNone;
            for (const auto& entry : boundary) {
                if (!same_key(entry.first, k)) continue;
                if (entry.second.first == p) continue;
                nb = entry.second.first;
                break; // disjoint cover => at most one other polygon per edge
            }
            if (e < poly.neighbours.size()) poly.neighbours[e] = nb;
        }
    }
}

// ===========================================================================
// Pass 5 — links: authored links snap onto polygons; auto links bridge gaps.
// ===========================================================================

void NavMesh::build_links() {
    m_auto_link_count = 0;
    m_dropped_links = 0;

    // Authored links: snap both endpoints onto polygons. A link whose endpoint
    // is off the mesh is dropped and counted — never silently teleported.
    for (const NavLink& spec : m_authored_links) {
        LinkRecord rec;
        rec.spec = spec;
        Vec3 sp, ep;
        if (!locate(spec.start, rec.start_poly, sp) ||
            !locate(spec.end, rec.end_poly, ep)) {
            ++m_dropped_links;
            continue;
        }
        rec.spec.start = sp;
        rec.spec.end = ep;
        rec.auto_generated = false;
        m_links.push_back(std::move(rec));
    }

    // Auto links between polygons separated by a gap or cliff inside the jump
    // budget. We test each non-neighbour pair and bridge their closest corners
    // when the horizontal reach is within jump_distance and the vertical within
    // jump_height. Bidirectional, and skipped when a link already joins them.
    //
    // Corner XZ is snapped back to the exact grid coordinate before measuring.
    // Polygon corners are reconstructed from grid integers, so a float round
    // trip can leave them off by up to half a cell; without this a gap of
    // exactly jump_distance could be measured as a hair over it and silently
    // dropped. Height (Y) is real geometry and is left untouched.
    const usize n = m_polys.size();
    auto snap_xz = [&](const Vec3& v) {
        const i32 gx = static_cast<i32>(std::lround((v.x - m_area_min.x) / m_cell_size));
        const i32 gz = static_cast<i32>(std::lround((v.z - m_area_min.z) / m_cell_size));
        return Vec3{m_area_min.x + static_cast<f32>(gx) * m_cell_size, v.y,
                    m_area_min.z + static_cast<f32>(gz) * m_cell_size};
    };
    for (u32 a = 0; a < n; ++a) {
        for (u32 b = a + 1; b < n; ++b) {
            if (are_neighbours(a, b)) continue;
            f32 best_h = std::numeric_limits<f32>::max();
            f32 best_dy = 0.0f;
            Vec3 best_a{}, best_b{};
            for (const Vec3& va_raw : m_polys[a].verts) {
                const Vec3 va = snap_xz(va_raw);
                for (const Vec3& vb_raw : m_polys[b].verts) {
                    const Vec3 vb = snap_xz(vb_raw);
                    const f32 hx = va.x - vb.x;
                    const f32 hz = va.z - vb.z;
                    const f32 hdist = std::sqrt(hx * hx + hz * hz);
                    if (hdist < best_h) {
                        best_h = hdist;
                        best_dy = std::fabs(va.y - vb.y);
                        best_a = va;
                        best_b = vb;
                    }
                }
            }
            if (best_h > m_cfg.jump_distance + 1e-4f) continue;
            if (best_dy > m_cfg.jump_height + 1e-4f) continue;
            bool dup = false;
            for (const auto& rec : m_links) {
                const bool fwd = (rec.start_poly == a && rec.end_poly == b);
                const bool rev = (rec.start_poly == b && rec.end_poly == a);
                if (fwd || (rev && rec.spec.bidirectional)) { dup = true; break; }
            }
            if (dup) continue;
            LinkRecord rec;
            rec.spec.start = best_a;
            rec.spec.end = best_b;
            rec.spec.bidirectional = true;
            rec.start_poly = a;
            rec.end_poly = b;
            rec.auto_generated = true;
            m_links.push_back(std::move(rec));
            ++m_auto_link_count;
        }
    }
}

// True when the two polygons share at least one vertex — they touch at a
// single point. A rectangle cover meets at corners wherever two edges cross,
// and a corridor that rounds such a corner passes through exactly that point,
// which is on the mesh. Refusing the point contact would break the very path
// the corner exists to allow; allowing it does not open a shortcut through a
// hole, because the shared point itself is walkable.
bool shares_vertex(const NavPoly& a, const NavPoly& b) {
    for (const Vec3& va : a.verts) {
        for (const Vec3& vb : b.verts) {
            if (va.nearly_equals(vb, 1e-4f)) return true;
        }
    }
    return false;
}

bool NavMesh::are_neighbours(u32 a, u32 b) const {
    for (u32 nb : m_polys[a].neighbours)
        if (nb == b) return true;
    for (u32 nb : m_polys[b].neighbours)
        if (nb == a) return true;
    return false;
}

// ===========================================================================
// Queries
// ===========================================================================

bool NavMesh::locate(Vec3 world, u32& out_poly, Vec3& out_point) const {
    f32 best = std::numeric_limits<f32>::max();
    bool found = false;
    for (u32 p = 0; p < m_polys.size(); ++p) {
        const NavPoly& poly = m_polys[p];
        if (poly.verts.empty()) continue;
        // Point-in-polygon (XZ), ray-crossing test.
        const usize vc = poly.verts.size();
        bool inside = false;
        for (usize i = 0, j = vc - 1; i < vc; j = i++) {
            const Vec3& vi = poly.verts[i];
            const Vec3& vj = poly.verts[j];
            const bool cond =
                ((vi.z > world.z) != (vj.z > world.z)) &&
                (world.x < (vj.x - vi.x) * (world.z - vi.z) / (vj.z - vi.z) + vi.x);
            if (cond) inside = !inside;
        }
        if (!inside) {
            // On the boundary counts as on the mesh (see the helper above):
            // the strict crossing test cannot see a point sitting exactly on
            // an edge, and every path portal is such a point.
            if (!point_on_polygon_boundary_xz(poly, world)) continue;
        }
        // Surface height approximated by the polygon's lowest vertex (a
        // conservative floor for a bilinear patch). Tolerance is walkable_climb:
        // a point a step away is on the mesh, a rooftop above is not.
        f32 surf = poly.verts[0].y;
        for (const Vec3& v : poly.verts) surf = std::min(surf, v.y);
        const f32 dy = std::fabs(world.y - surf);
        if (dy > m_cfg.walkable_climb + 1e-3f) continue;
        if (dy < best) {
            best = dy;
            out_poly = p;
            out_point = Vec3{world.x, surf, world.z};
            found = true;
        }
    }
    return found;
}

bool NavMesh::closest_point(Vec3 world, Vec3& out) const {
    if (m_polys.empty()) return false;
    f32 best = std::numeric_limits<f32>::max();
    bool found = false;
    auto consider = [&](const Vec3& cand) {
        const f32 d = (cand - world).length_sq();
        if (d < best) { best = d; out = cand; found = true; }
    };
    for (const NavPoly& poly : m_polys) {
        if (poly.verts.empty()) continue;
        for (const Vec3& v : poly.verts) consider(v);
        const usize vc = poly.verts.size();
        for (usize i = 0; i < vc; ++i) {
            const Vec3& a = poly.verts[i];
            const Vec3& b = poly.verts[(i + 1) % vc];
            const Vec3 ab = b - a;
            const f32 ab2 = ab.length_sq();
            if (ab2 < 1e-9f) continue;
            f32 t = (Vec3{world.x - a.x, 0.0f, world.z - a.z}).dot(ab) / ab2;
            t = std::clamp(t, 0.0f, 1.0f);
            consider(a + ab * t);
        }
    }
    return found;
}

bool NavMesh::raycast(Vec3 a, Vec3 b) const {
    // Every sampled point lies on a polygon, and consecutive differing
    // polygons are neighbours. This is the smoothing probe and the
    // corner-cutting guard: a shortcut that threads a hole is not a shortcut.
    const Vec3 d = b - a;
    const f32 len = d.length();
    if (len < 1e-6f) {
        u32 p; Vec3 pt;
        return locate(a, p, pt);
    }
    const f32 step = m_cell_size * 0.25f;
    const i32 samples = static_cast<i32>(len / step) + 1;
    u32 prev_poly = kNone;
    for (i32 i = 0; i <= samples; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(samples);
        const Vec3 p = a + d * t;
        u32 poly; Vec3 pt;
        if (!locate(p, poly, pt)) return false;
        if (prev_poly != kNone && poly != prev_poly) {
            // Crossing from one polygon to another is legitimate when they
            // share an edge (the corridor walks through) or a vertex (it
            // rounds a corner). Anything else is a jump across a hole, and the
            // shortcut is not a shortcut.
            if (!are_neighbours(prev_poly, poly) &&
                !shares_vertex(m_polys[prev_poly], m_polys[poly])) {
                return false;
            }
        }
        prev_poly = poly;
    }
    return true;
}

f32 NavMesh::edge_cost(u32 a, u32 b) const {
    // Centre-to-centre distance; robust for the axis-aligned rectangles this
    // mesh produces. Deterministic because centres are vertex-mean.
    auto centre = [&](u32 p) {
        Vec3 c = Vec3{};
        if (!m_polys[p].verts.empty()) {
            for (const Vec3& v : m_polys[p].verts) c += v;
            c = c / static_cast<f32>(m_polys[p].verts.size());
        }
        return c;
    };
    const Vec3 ca = centre(a);
    const Vec3 cb = centre(b);
    const f32 dx = ca.x - cb.x;
    const f32 dz = ca.z - cb.z;
    const f32 dy = ca.y - cb.y;
    return std::sqrt(dx * dx + dz * dz + dy * dy);
}

bool NavMesh::find_path(Vec3 start, Vec3 goal, std::vector<Vec3>& out_waypoints) const {
    out_waypoints.clear();
    u32 s_poly, g_poly;
    Vec3 s_pt, g_pt;
    if (!locate(start, s_poly, s_pt) || !locate(goal, g_poly, g_pt)) return false;
    if (s_poly == g_poly) {
        out_waypoints.push_back(start);
        out_waypoints.push_back(goal);
        return true;
    }

    // A* over polygons + off-mesh links. Deterministic: a hand-rolled binary
    // min-heap with (f, h, sequence) tie-breaks. The std heap algorithms and
    // std::priority_queue do not instantiate cleanly inside nf::ai (they hit the
    // std/UUID two-phase-lookup collision), so — exactly as NavGrid does — the
    // sift ordering is written out by hand. `parent` stores the previous
    // polygon, or a negative encoded link index across an off-mesh link.
    struct Node {
        f32 f = 0.0f;
        f32 h = 0.0f;
        u64 seq = 0;
        u32 poly = 0;
    };
    struct NodeHeap {
        ::std::vector<Node> v;
        bool higher(const Node& a, const Node& b) const { // is `a` worse than `b`?
            if (a.f != b.f) return a.f > b.f;
            if (a.h != b.h) return a.h > b.h;
            return a.seq > b.seq;
        }
        bool empty() const { return v.empty(); }
        void push(const Node& n) {
            v.push_back(n);
            usize i = v.size() - 1;
            while (i > 0) {
                const usize p = (i - 1) / 2;
                if (!higher(v[i], v[p])) break;
                ::std::swap(v[i], v[p]);
                i = p;
            }
        }
        Node pop() {
            const Node top = v.front();
            v.front() = v.back();
            v.pop_back();
            usize i = 0;
            const usize n = v.size();
            for (;;) {
                const usize l = 2 * i + 1;
                const usize r = l + 1;
                usize best = i;
                if (l < n && higher(v[l], v[best])) best = l;
                if (r < n && higher(v[r], v[best])) best = r;
                if (best == i) break;
                ::std::swap(v[i], v[best]);
                i = best;
            }
            return top;
        }
    };
    auto heuristic = [&](u32 p) {
        if (m_polys[p].verts.empty()) return 0.0f;
        const Vec3& v = m_polys[p].verts[0];
        const f32 dx = v.x - g_pt.x;
        const f32 dz = v.z - g_pt.z;
        return std::sqrt(dx * dx + dz * dz);
    };

    std::vector<f32> g(m_polys.size(), std::numeric_limits<f32>::max());
    std::vector<i64> parent(m_polys.size(), -1);
    std::vector<u8> closed(m_polys.size(), 0);
    // Which off-mesh link (if any) each polygon was entered THROUGH. The
    // predecessor polygon alone cannot tell an edge step from a jump: across a
    // link the walkable surface has a gap, and the two endpoints of the link
    // are the only points the route may touch. Reconstruction needs that
    // distinction, so it is recorded here rather than re-derived from the
    // geometry (two polygons joined by both an edge and a link would be
    // ambiguous).
    std::vector<i64> via_link(m_polys.size(), -1);
    NodeHeap open;
    u64 seq = 0;
    g[s_poly] = 0.0f;
    const f32 h0 = heuristic(s_poly);
    open.push(Node{h0, h0, seq++, s_poly});

    bool found = false;
    while (!open.empty()) {
        const Node cur = open.pop();
        if (closed[cur.poly]) continue;
        closed[cur.poly] = 1;
        if (cur.poly == g_poly) { found = true; break; }

        for (u32 nb : m_polys[cur.poly].neighbours) {
            if (nb == kNone || closed[nb]) continue;
            const f32 ng = g[cur.poly] + edge_cost(cur.poly, nb);
            if (ng < g[nb]) {
                g[nb] = ng;
                parent[nb] = static_cast<i64>(cur.poly);
                via_link[nb] = -1;
                const f32 h = heuristic(nb);
                open.push(Node{ng + h, h, seq++, nb});
            }
        }
        // Off-mesh links out of this polygon. The predecessor is stored as the
        // polygon we expanded from, exactly like an edge step: the strict
        // g-improvement invariant keeps the parent graph a tree rooted at the
        // start, so reconstruction cannot cycle (an encoded-link scheme could
        // ping-pong across a bidirectional link).
        for (u32 li = 0; li < m_links.size(); ++li) {
            const LinkRecord& rec = m_links[li];
            u32 to = kNone;
            if (rec.start_poly == cur.poly) to = rec.end_poly;
            else if (rec.end_poly == cur.poly && rec.spec.bidirectional) to = rec.start_poly;
            if (to == kNone || closed[to]) continue;
            const f32 cost = (rec.spec.start - rec.spec.end).length();
            const f32 ng = g[cur.poly] + cost;
            if (ng < g[to]) {
                g[to] = ng;
                parent[to] = static_cast<i64>(cur.poly);
                via_link[to] = static_cast<i64>(li);
                const f32 h = heuristic(to);
                open.push(Node{ng + h, h, seq++, to});
            }
        }
    }
    if (!found) return false;

    // Reconstruct the polygon chain by following the predecessor tree back to
    // the start. The strict g-improvement invariant guarantees termination (the
    // visited guard is belt-and-braces against any pathological zero-cost edge).
    std::vector<u32> polys;
    std::vector<i64> entry_links; // entry_links[i] is the link polys[i] was entered through
    std::vector<u8> seen(m_polys.size(), 0);
    i64 cur = static_cast<i64>(g_poly);
    while (cur >= 0 && !seen[static_cast<usize>(cur)]) {
        seen[static_cast<usize>(cur)] = 1;
        polys.push_back(static_cast<u32>(cur));
        entry_links.push_back(via_link[static_cast<usize>(cur)]);
        cur = parent[static_cast<usize>(cur)];
    }
    std::reverse(polys.begin(), polys.end());
    std::reverse(entry_links.begin(), entry_links.end());

    // String-pull the chain. The points the route must pass through are the
    // PORTALS between consecutive polygons — the shared boundary edge midpoints
    // — plus both ends of an off-mesh link, plus the raw start and goal.
    //
    // Portals, not polygon centres: the straight line between two centres can
    // leave the mesh even when the polygons are adjacent (an L-shaped cover has
    // exactly that corner), which is a shortcut through a hole. A portal sits ON
    // the shared edge, so the leg from one portal to the next stays inside the
    // (convex) polygon between them by construction — the greedy line-of-sight
    // below then removes waypoints the same geometry makes redundant, and every
    // surviving leg is an edge that was actually tested against the mesh.
    //
    // The raw endpoints are part of the smoothed list rather than pinned onto
    // it afterwards: a pinned pair is a shortcut nobody checked, and the line
    // between the raw start and the raw goal can thread a hole the corridor
    // walked around.
    std::vector<Vec3> pts;
    pts.push_back(start);
    for (usize i = 1; i < polys.size(); ++i) {
        const u32 from = polys[i - 1];
        const u32 to = polys[i];
        const i64 li = entry_links[i];
        if (li >= 0 && static_cast<usize>(li) < m_links.size()) {
            // Across a jump: the only two points the walkable surface offers
            // are the link's own endpoints.
            const LinkRecord& rec = m_links[static_cast<usize>(li)];
            pts.push_back(rec.spec.start);
            pts.push_back(rec.spec.end);
            continue;
        }
        const NavPoly& a = m_polys[from];
        const usize vc = a.verts.size();
        for (usize e = 0; e < vc; ++e) {
            if (e >= a.neighbours.size() || a.neighbours[e] != to) continue;
            pts.push_back((a.verts[e] + a.verts[(e + 1) % vc]) * 0.5f);
            break;
        }
    }
    pts.push_back(goal);

    // Consecutive duplicates are dropped: a zero-length auto link (two
    // polygons that meet at a single corner) contributes two identical
    // endpoints, and a waypoint list containing "walk to here, then walk to
    // here" is noise the follower has to special-case.
    std::vector<Vec3> smooth;
    smooth.push_back(pts.front());
    usize anchor = 0;
    while (anchor + 1 < pts.size()) {
        usize far = anchor + 1;
        for (usize k = anchor + 2; k < pts.size(); ++k) {
            if (raycast(pts[anchor], pts[k])) far = k;
            else break;
        }
        if (!smooth.back().nearly_equals(pts[far], 1e-5f)) {
            smooth.push_back(pts[far]);
        }
        anchor = far;
    }
    out_waypoints = std::move(smooth);
    return true;
}

// ===========================================================================
// Determinism assertion — bit-identical mesh comparison.
// ===========================================================================

bool NavMesh::same_as(const NavMesh& other) const {
    if (m_built != other.m_built) return false;
    if (m_grid_w != other.m_grid_w || m_grid_h != other.m_grid_h) return false;
    if (m_region_count != other.m_region_count) return false;
    if (m_auto_link_count != other.m_auto_link_count) return false;
    if (m_dropped_links != other.m_dropped_links) return false;
    if (m_voxel_count != other.m_voxel_count) return false;
    if (m_polys.size() != other.m_polys.size()) return false;
    for (usize i = 0; i < m_polys.size(); ++i)
        if (!(m_polys[i] == other.m_polys[i])) return false;
    if (m_links.size() != other.m_links.size()) return false;
    for (usize i = 0; i < m_links.size(); ++i) {
        if (m_links[i].start_poly != other.m_links[i].start_poly) return false;
        if (m_links[i].end_poly != other.m_links[i].end_poly) return false;
        if (m_links[i].auto_generated != other.m_links[i].auto_generated) return false;
        // NavLink has no operator== (only NavObstacle/NavPoly do), so compare
        // its fields directly with Vec3::nearly_equals for the epsilon.
        const NavLink& la = m_links[i].spec;
        const NavLink& lb = other.m_links[i].spec;
        if (la.bidirectional != lb.bidirectional) return false;
        if (!la.start.nearly_equals(lb.start, 1e-5f)) return false;
        if (!la.end.nearly_equals(lb.end, 1e-5f)) return false;
    }
    return true;
}

} // namespace nf::ai




