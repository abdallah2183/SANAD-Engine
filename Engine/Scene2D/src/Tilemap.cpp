// NF/Scene2D/Tilemap.cpp — chunk storage, auto-tiling, collision merge,
// navigation and streaming.

#include <NF/Scene2D/Tilemap.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::scene2d {

// =========================================================================
// TilemapLayer
// =========================================================================

bool TilemapLayer::is_solid(i64 tx, i64 ty, const Tileset& ts) const {
    const u32 packed = get(tx, ty);
    if (packed == kEmptyTile) return false;
    return ts.material(tile_id(packed)).solid;
}

// =========================================================================
// Tilemap — layers
// =========================================================================

TilemapLayer& Tilemap::add_layer(const std::string& name, f32 parallax,
                                 bool collidable, bool auto_tile) {
    TilemapLayer layer;
    layer.name = name;
    layer.tile_size = tile_size;
    layer.parallax = parallax;
    layer.collidable = collidable;
    layer.auto_tile = auto_tile;
    layers.push_back(std::move(layer));
    return layers.back();
}

TilemapLayer* Tilemap::layer(const std::string& name) {
    for (TilemapLayer& l : layers) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

const TilemapLayer* Tilemap::layer(const std::string& name) const {
    for (const TilemapLayer& l : layers) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

u32 Tilemap::get(usize layer_index, i64 tx, i64 ty) const {
    if (layer_index >= layers.size()) return kEmptyTile;
    return layers[layer_index].get(tx, ty);
}

bool Tilemap::set(usize layer_index, i64 tx, i64 ty, u32 packed) {
    if (layer_index >= layers.size()) return false;
    return layers[layer_index].set(tx, ty, packed);
}

bool Tilemap::is_solid_at(i64 tx, i64 ty, f32 dir_y) const {
    for (const TilemapLayer& l : layers) {
        if (!l.collidable) continue;
        const u32 packed = l.get(tx, ty);
        if (packed == kEmptyTile) continue;
        const TileMaterial& m = tileset.material(tile_id(packed));
        if (!m.solid) continue;
        // One-way platforms only resist a body moving downward onto them; a
        // body moving up passes straight through, which is what makes them
        // jumpable from underneath.
        if (m.one_way && dir_y < 0.0f) continue;
        return true;
    }
    return false;
}

// =========================================================================
// §55 "Auto tiling"
// =========================================================================

u32 Tilemap::auto_tile_mask(const TilemapLayer& layer, i64 tx, i64 ty,
                            u32 id) const {
    u32 mask = 0u;
    // Edge neighbours first: a neighbour counts only when it is the same
    // logical tile. Comparing the resolved id (not the raw packed value) means
    // flipped or rotated tiles of the same kind still chain.
    const auto same = [&](i64 dx, i64 dy) -> bool {
        const u32 other = layer.get(tx + dx, ty + dy);
        return other != kEmptyTile && tile_id(other) == id;
    };

    const bool n = same(0, -1);
    const bool e = same(1, 0);
    const bool s = same(0, 1);
    const bool w = same(-1, 0);
    if (n) mask |= AT_N;
    if (e) mask |= AT_E;
    if (s) mask |= AT_S;
    if (w) mask |= AT_W;

    // Corners are only read when both adjoining edges are present. Filling a
    // corner next to an empty edge would draw a rounded diagonal into a gap.
    if (n && e && same(1, -1)) mask |= AT_NE;
    if (s && e && same(1, 1)) mask |= AT_SE;
    if (s && w && same(-1, 1)) mask |= AT_SW;
    if (n && w && same(-1, -1)) mask |= AT_NW;
    return mask;
}

u32 Tilemap::resolve_variant(const TilemapLayer& layer, i64 tx, i64 ty) const {
    const u32 packed = layer.get(tx, ty);
    if (packed == kEmptyTile) return kEmptyTile;

    const u32 id = tile_id(packed);
    if (!layer.auto_tile || !tileset.is_auto_tile(id)) return packed;

    const u32 variants = tileset.variant_count(id);
    if (variants == 0u) return packed;

    const u32 mask = auto_tile_mask(layer, tx, ty, id);

    // Marching-squares index over the four edges (0..15): a lone tile is 0, a
    // fully surrounded tile is 15, and the in-betweens pick edges and corners.
    const u32 edges = ((mask & AT_N) ? 1u : 0u) | ((mask & AT_E) ? 2u : 0u) |
                      ((mask & AT_S) ? 4u : 0u) | ((mask & AT_W) ? 8u : 0u);
    // Corner bits widen the index past 16 when the tileset actually has the
    // art for it. A tileset with 16 variants ignores corners and still renders
    // correctly, just with less diagonal detail.
    const u32 corners = ((mask & AT_NE) ? 1u : 0u) | ((mask & AT_SE) ? 2u : 0u) |
                        ((mask & AT_SW) ? 4u : 0u) | ((mask & AT_NW) ? 8u : 0u);

    u32 index = edges;
    if (variants > 16u) index = edges | (corners << 4);
    index %= variants;

    // Variants are stored as consecutive tile ids starting at the base id, so
    // resolving is an offset rather than a lookup.
    return pack_tile(id + index, tile_flags(packed));
}

// =========================================================================
// §55 "Collision"
// =========================================================================

namespace {

/// Returns the chunk bounds that actually contain data, so collision
/// extraction walks a finite rectangle even on a nominally infinite map.
void layer_chunk_bounds(const TilemapLayer& layer, i64& out_cx0, i64& out_cy0,
                        i64& out_cx1, i64& out_cy1) {
    bool any = false;
    for (const auto& kv : layer.chunks) {
        if (!any) {
            out_cx0 = out_cx1 = kv.first.x;
            out_cy0 = out_cy1 = kv.first.y;
            any = true;
            continue;
        }
        out_cx0 = out_cx0 < kv.first.x ? out_cx0 : kv.first.x;
        out_cx1 = out_cx1 > kv.first.x ? out_cx1 : kv.first.x;
        out_cy0 = out_cy0 < kv.first.y ? out_cy0 : kv.first.y;
        out_cy1 = out_cy1 > kv.first.y ? out_cy1 : kv.first.y;
    }
    if (!any) {
        out_cx0 = out_cy0 = 0;
        out_cx1 = out_cy1 = -1;
    }
}

} // namespace

CollisionMesh Tilemap::extract_collision(const TilemapLayer& layer) const {
    CollisionMesh mesh;
    if (!layer.collidable || layer.chunks.empty()) return mesh;

    i64 cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    layer_chunk_bounds(layer, cx0, cy0, cx1, cy1);

    const i64 tx0 = cx0 * Chunk::kSize;
    const i64 ty0 = cy0 * Chunk::kSize;
    const i64 tx1 = (cx1 + 1) * Chunk::kSize - 1;
    const i64 ty1 = (cy1 + 1) * Chunk::kSize - 1;

    const f32 ts = layer.tile_size;

    // Row-major scan emitting maximal horizontal runs, then stacked runs of
    // identical width and flags fuse into taller rectangles. `open` holds
    // indices into mesh.rects for rects whose bottom edge sits one tile above
    // the current row and which can therefore still grow downward.
    std::vector<usize> open;

    for (i64 ty = ty0; ty <= ty1; ++ty) {
        std::vector<usize> next_open;

        i64 tx = tx0;
        while (tx <= tx1) {
            const u32 packed = layer.get(tx, ty);
            if (packed == kEmptyTile ||
                !tileset.material(tile_id(packed)).solid) {
                ++tx;
                continue;
            }
            const bool one_way = tileset.material(tile_id(packed)).one_way;

            // Run end: same solidity AND same one-way-ness, so a one-way
            // platform never fuses with the solid floor beside it.
            i64 tx2 = tx + 1;
            while (tx2 <= tx1) {
                const u32 p2 = layer.get(tx2, ty);
                if (p2 == kEmptyTile) break;
                const TileMaterial& m2 = tileset.material(tile_id(p2));
                if (!m2.solid || m2.one_way != one_way) break;
                ++tx2;
            }

            const Rect seg{static_cast<f32>(tx) * ts, static_cast<f32>(ty) * ts,
                           static_cast<f32>(tx2 - tx) * ts, ts};

            // Extend a rect from the previous row if it aligns exactly — same
            // left edge, same width, same flag. Both lists are in ascending x
            // order, so the scan terminates early in practice.
            bool extended = false;
            for (usize oi : open) {
                const Rect& r = mesh.rects[oi];
                if (r.x != seg.x || r.w != seg.w) continue;
                const bool r_one_way =
                    (mesh.flags[oi] & kCollisionOneWayBit) != 0u;
                if (r_one_way != one_way) continue;
                mesh.rects[oi].h += seg.h;
                next_open.push_back(oi);
                extended = true;
                break;
            }

            if (!extended) {
                const u32 bit = one_way ? kCollisionOneWayBit : 0u;
                mesh.rects.push_back(seg);
                mesh.flags.push_back(bit);
                next_open.push_back(mesh.rects.size() - 1u);
            }

            tx = tx2;
        }

        open.swap(next_open);
    }

    return mesh;
}

std::vector<Rect> Tilemap::extract_all_collision() const {
    std::vector<Rect> all;
    for (const TilemapLayer& l : layers) {
        if (!l.collidable) continue;
        const CollisionMesh mesh = extract_collision(l);
        all.insert(all.end(), mesh.rects.begin(), mesh.rects.end());
    }
    // (y, x) order so a cooked collision file compares equal between runs
    // regardless of layer construction order.
    std::stable_sort(all.begin(), all.end(), [](const Rect& a, const Rect& b) {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    return all;
}

// =========================================================================
// §55 "Navigation"
// =========================================================================

NavGrid2D Tilemap::build_nav_grid(Rect bounds, f32 cell_size) const {
    NavGrid2D grid;
    if (cell_size <= 0.0f) return grid;

    const i64 w = static_cast<i64>(std::ceil(bounds.w / cell_size));
    const i64 h = static_cast<i64>(std::ceil(bounds.h / cell_size));
    if (w <= 0 || h <= 0) return grid;

    grid.cell_size = cell_size;
    grid.origin = Vec2{bounds.x, bounds.y};
    grid.width = w;
    grid.height = h;
    grid.walkable.assign(static_cast<usize>(w * h), true);

    const std::vector<Rect> blockers = extract_all_collision();
    for (i64 cy = 0; cy < h; ++cy) {
        for (i64 cx = 0; cx < w; ++cx) {
            const Vec2 centre = grid.cell_centre(cx, cy);
            for (const Rect& r : blockers) {
                if (r.contains(centre)) {
                    grid.walkable[static_cast<usize>(cy * w + cx)] = false;
                    break;
                }
            }
        }
    }
    return grid;
}

namespace {

/// Bresenham walk from (ax,ay) to (bx,by). Every cell the line passes through
/// must be walkable for the segment to be usable as a path shortcut — this is
/// what stops straightening from cutting a corner through a wall.
bool line_of_sight(const NavGrid2D& grid, i64 ax, i64 ay, i64 bx, i64 by) {
    const i64 dx = std::abs(bx - ax);
    const i64 dy = std::abs(by - ay);
    const i64 sx = ax < bx ? 1 : -1;
    const i64 sy = ay < by ? 1 : -1;
    i64 err = dx - dy;
    i64 x = ax;
    i64 y = ay;

    while (true) {
        if (!grid.is_walkable(x, y)) return false;
        if (x == bx && y == by) return true;
        const i64 e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

} // namespace

NavPath Tilemap::find_path(const NavGrid2D& grid, Vec2 start_world,
                           Vec2 goal_world, f32 goal_radius) const {
    NavPath path;
    if (grid.width <= 0 || grid.height <= 0) return path;

    i64 sx = 0, sy = 0, gx = 0, gy = 0;
    grid.cell_at_world(start_world, sx, sy);
    grid.cell_at_world(goal_world, gx, gy);

    // The goal may sit inside an obstruction; fall back to the nearest
    // walkable cell on an expanding ring so "click on the wall" still routes
    // to its base instead of failing outright.
    if (!grid.is_walkable(gx, gy)) {
        bool found = false;
        for (i64 r = 1; r <= 8 && !found; ++r) {
            for (i64 dy = -r; dy <= r && !found; ++dy) {
                for (i64 dx = -r; dx <= r && !found; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    if (grid.is_walkable(gx + dx, gy + dy)) {
                        gx += dx;
                        gy += dy;
                        found = true;
                    }
                }
            }
        }
        if (!found) return path;
    }
    if (!grid.is_walkable(sx, sy)) return path;
    if (sx == gx && sy == gy) {
        path.found = true;
        path.waypoints.push_back(grid.cell_centre(gx, gy));
        return path;
    }

    const i64 w = grid.width;
    const auto idx = [w](i64 cx, i64 cy) -> u64 {
        return static_cast<u64>(cy * w + cx);
    };

    struct Node {
        u64 cell = 0;
        f32 f = 0.0f;
    };
    struct NodeGreater {
        // Lexicographic (f, cell) as a min-heap: equal-cost paths always
        // resolve to the same one, which is what makes a recorded replay's
        // route reproducible (design doc Section 114).
        bool operator()(const Node& a, const Node& b) const {
            if (a.f != b.f) return a.f > b.f;
            return a.cell > b.cell;
        }
    };

    const usize cell_count = static_cast<usize>(grid.width) * static_cast<usize>(grid.height);
    std::vector<f32> g_score(cell_count, std::numeric_limits<f32>::infinity());
    std::vector<u64> came_from(cell_count, u64_max);
    std::vector<bool> closed(cell_count, false);

    std::priority_queue<Node, std::vector<Node>, NodeGreater> open;
    g_score[static_cast<usize>(idx(sx, sy))] = 0.0f;
    open.push({idx(sx, sy), 0.0f});

    const f32 cell = grid.cell_size;
    // Octile distances: straight 1 cell, diagonal sqrt(2) cells.
    const f32 diag = static_cast<f32>(std::sqrt(2.0));
    const auto heuristic = [&](i64 cx, i64 cy) -> f32 {
        const i64 dx = std::abs(cx - gx);
        const i64 dy = std::abs(cy - gy);
        const i64 lo = dx < dy ? dx : dy;
        const i64 hi = dx > dy ? dx : dy;
        return static_cast<f32>(hi - lo) * cell +
               static_cast<f32>(lo) * diag * cell;
    };

    const f32 arrive_sq = goal_radius * goal_radius;
    bool reached = false;

    while (!open.empty()) {
        const Node cur = open.top();
        open.pop();
        const usize cur_i = static_cast<usize>(cur.cell);
        if (closed[cur_i]) continue;
        closed[cur_i] = true;

        const i64 cx = static_cast<i64>(cur.cell % static_cast<u64>(w));
        const i64 cy = static_cast<i64>(cur.cell / static_cast<u64>(w));

        if (cx == gx && cy == gy) {
            reached = true;
            break;
        }
        // Arrival tolerance: any cell inside goal_radius ends the search, so a
        // click lands the body next to the target rather than exactly on it.
        if (arrive_sq > 0.0f) {
            const f32 ex = (static_cast<f32>(cx - gx)) * cell;
            const f32 ey = (static_cast<f32>(cy - gy)) * cell;
            if (ex * ex + ey * ey <= arrive_sq) {
                gx = cx;
                gy = cy;
                reached = true;
                break;
            }
        }

        for (i64 dy = -1; dy <= 1; ++dy) {
            for (i64 dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const i64 nx = cx + dx;
                const i64 ny = cy + dy;
                if (!grid.is_walkable(nx, ny)) continue;
                if (dx != 0 && dy != 0) {
                    // No corner cutting: a diagonal move needs both orthogonal
                    // neighbours free, or the body clips through the tile it
                    // slides past.
                    if (!grid.is_walkable(cx + dx, cy)) continue;
                    if (!grid.is_walkable(cx, cy + dy)) continue;
                }
                const f32 step = (dx != 0 && dy != 0) ? diag : 1.0f;
                const f32 tentative =
                    g_score[cur_i] + step * cell;
                const u64 ni = idx(nx, ny);
                if (tentative >= g_score[static_cast<usize>(ni)]) continue;
                g_score[static_cast<usize>(ni)] = tentative;
                came_from[static_cast<usize>(ni)] = cur.cell;
                open.push({ni, tentative + heuristic(nx, ny)});
            }
        }
    }

    if (!reached) return path;

    // Walk back from the goal, then reverse. Waypoints are cell centres in
    // world space; the start cell is dropped because the body is already there.
    std::vector<u64> chain;
    u64 cursor = idx(gx, gy);
    while (cursor != u64_max) {
        chain.push_back(cursor);
        if (cursor == idx(sx, sy)) break;
        cursor = came_from[static_cast<usize>(cursor)];
    }
    if (chain.empty()) return path;

    // Path straightening: from each kept waypoint, jump as far down the chain
    // as line-of-sight allows, so the route is not a staircase of cell centres
    // when a straight line would do. Greedy and order-preserving.
    std::vector<u64> simplified;
    simplified.push_back(chain.back());
    usize i = chain.size() - 1;
    while (i > 0) {
        usize next = i - 1;
        for (usize k = i - 1; k > 0; --k) {
            const i64 ax = static_cast<i64>(simplified.back() % static_cast<u64>(w));
            const i64 ay = static_cast<i64>(simplified.back() / static_cast<u64>(w));
            const i64 bx = static_cast<i64>(chain[k - 1] % static_cast<u64>(w));
            const i64 by = static_cast<i64>(chain[k - 1] / static_cast<u64>(w));
            if (line_of_sight(grid, ax, ay, bx, by)) {
                next = k - 1;
            } else {
                break;
            }
        }
        simplified.push_back(chain[next]);
        i = next;
    }

    for (u64 c : simplified) {
        const i64 cx = static_cast<i64>(c % static_cast<u64>(w));
        const i64 cy = static_cast<i64>(c / static_cast<u64>(w));
        path.waypoints.push_back(grid.cell_centre(cx, cy));
    }
    path.cost = g_score[static_cast<usize>(idx(gx, gy))];
    path.found = true;
    return path;
}

// =========================================================================
// §55 "Large maps" / "Streaming"
// =========================================================================

namespace {

/// Chunk coordinate of a tile coordinate, flooring toward negative infinity so
/// tile -1 lives in chunk -1.
i64 chunk_of(i64 t) {
    return t >= 0 ? t / Chunk::kSize
                  : (t + 1) / static_cast<i64>(Chunk::kSize) - 1;
}

} // namespace

void Tilemap::update_streaming(i64 centre_tx, i64 centre_ty, u32 radius_chunks) {
    if (!m_generator) return;

    const i64 ccx = chunk_of(centre_tx);
    const i64 ccy = chunk_of(centre_ty);
    const i64 r = static_cast<i64>(radius_chunks);
    // Circular footprint: a square radius would load ~27% more chunks than the
    // screen ever sees at the corners.
    const i64 r_sq = r * r + r;

    for (i64 dy = -r; dy <= r; ++dy) {
        for (i64 dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r_sq) continue;
            const i64 cx = ccx + dx;
            const i64 cy = ccy + dy;

            for (TilemapLayer& layer : layers) {
                const ChunkKey key{cx, cy};
                auto found = layer.chunks.find(key);
                if (found != layer.chunks.end() && found->second.generated) {
                    continue;
                }
                Chunk chunk;
                m_generator(layer.name, cx, cy, chunk);
                chunk.generated = true;
                layer.chunks[key] = std::move(chunk);
            }
        }
    }
}

void Tilemap::unload_outside(i64 centre_tx, i64 centre_ty, u32 radius_chunks,
                             u32 extra) {
    const i64 ccx = chunk_of(centre_tx);
    const i64 ccy = chunk_of(centre_ty);
    const i64 r = static_cast<i64>(radius_chunks);
    const i64 keep_sq = r * r + r +
                        static_cast<i64>(extra) * static_cast<i64>(extra) * 4;

    for (TilemapLayer& layer : layers) {
        for (auto it = layer.chunks.begin(); it != layer.chunks.end();) {
            const i64 dx = it->first.x - ccx;
            const i64 dy = it->first.y - ccy;
            if (dx * dx + dy * dy <= keep_sq) {
                ++it;
                continue;
            }
            it = layer.chunks.erase(it);
        }
    }
}

usize Tilemap::loaded_chunk_count() const {
    usize count = 0;
    for (const TilemapLayer& l : layers) count += l.chunks.size();
    return count;
}

bool Tilemap::has_chunk(usize layer_index, i64 cx, i64 cy) const {
    if (layer_index >= layers.size()) return false;
    return layers[layer_index].chunks.find(ChunkKey{cx, cy}) !=
           layers[layer_index].chunks.end();
}

} // namespace nf::scene2d
