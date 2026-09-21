#pragma once

// NF/Scene2D/Tilemap.hpp — layered tile maps: chunks, auto-tiling, collision
// extraction, navigation and streaming.
// Design doc Section 55 (Tilemap): "Layers / Auto tiling / Chunking / Collision
// / Navigation / Large maps / Streaming".
//
// The map is a sparse grid of chunks, not a dense 2D array. Chunk coordinates
// are signed and hashed, so a map that spans a few million tiles costs memory
// only where it has content, and a chunk at x = -5000 is as addressable as one
// at x = 3. That is what "large maps" means here: no origin, no borders, no
// wraparound surprises.
//
// Determinism (design doc Section 114): every routine that can vary output
// between runs — auto-tiling, collision merging, A* — sorts or hashes its
// inputs the same way every time, so cooking a map twice yields byte-identical
// results.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include "Math2D.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <queue>
#include <unordered_map>
#include <vector>

namespace nf::scene2d {

/// Identifies a tile inside the tileset. 0 is reserved for "nothing here":
/// every empty cell in every layer returns kEmptyTile, so a caller never has to
/// distinguish "unloaded chunk" from "air" while rendering or colliding.
enum : u32 { kEmptyTile = 0 };

/// Bit-packed flavour of a tile. Flags live above the id so a caller can store
/// "grass tile, flipped horizontally, one-way" in one u32 and still read the id
/// back with a mask. Keeping this flat keeps a whole chunk in 1 KB instead of
/// the 16+ bytes a struct-per-tile would cost.
enum TileFlags : u32 {
    TileFlag_None = 0,
    TileFlag_FlipX = 1u << 24,
    TileFlag_FlipY = 1u << 25,
    TileFlag_Rot90 = 1u << 26,
    /// Solid only from above (one-way platform): the player jumps up through it
    /// and lands on top. Collision extraction emits these as sensor-ish rects
    /// the physics layer treats specially.
    TileFlag_OneWay = 1u << 27,
    TileFlag_Damage = 1u << 28,
};

inline constexpr u32 tile_id(u32 packed) { return packed & 0x00FFFFFFu; }
inline constexpr u32 tile_flags(u32 packed) { return packed & 0xFF000000u; }
inline constexpr u32 pack_tile(u32 id, u32 flags = 0) {
    return (flags & 0xFF000000u) | (id & 0x00FFFFFFu);
}

/// Per-tile material for the physics and navigation queries. Cheap lookup table
/// indexed by tile id rather than a per-tile struct, since 99% of tiles in a
/// map share a material with their neighbours.
struct TileMaterial {
    bool solid = true;       ///< Blocks movement.
    bool one_way = false;    ///< Solid from the top only.
    f32 friction = 0.7f;     ///< Tangential damping for bodies resting on it.
    f32 restitution = 0.0f;  ///< Bounciness.
    /// Traversal cost for pathfinding. 1.0 = normal floor; higher = slower
    /// (mud); kBlocked makes the tile impassable despite being non-solid
    /// (a tile the AI refuses to walk through, like a fire hazard).
    f32 nav_cost = 1.0f;
    static constexpr f32 kBlocked = -1.0f;
};

/// A square block of tiles. 16x16 at 1 world unit per tile is 256x256 world
/// units per chunk, sized to be a single friendly cache page of vertex data and
/// a natural streaming unit: one chunk is what the streaming ring loads or
/// unloads at a time.
struct Chunk {
    static constexpr u32 kSize = 16;

    /// kSize*kSize packed tiles, row-major, row 0 at the top (y grows down).
    u32 tiles[kSize * kSize] = {};

    /// True once a generator (or a loader) has filled this chunk. An unfilled
    /// chunk in the map reports kEmptyTile for every cell, which is exactly
    /// what an unloaded chunk should look like to a renderer that has not asked
    /// for streaming yet — the distinction only matters to the streamer itself.
    bool generated = false;

    u32& at(u32 lx, u32 ly) { return tiles[ly * kSize + lx]; }
    u32 at(u32 lx, u32 ly) const { return tiles[ly * kSize + lx]; }
};

/// Spatial hash key for a chunk coordinate. Signed coords fold into u64 so
/// negative chunks hash distinctly from positive ones — a naive cast would make
/// (-1, 0) collide with (4294967295, 0).
struct ChunkKey {
    i64 x = 0;
    i64 y = 0;

    bool operator==(const ChunkKey& o) const { return x == o.x && y == o.y; }

    static ChunkKey from_tile(i64 tx, i64 ty) {
        // Floor division: tile -1 is in chunk -1, not chunk 0. C++ division
        // truncates toward zero, so negative offsets need the correction.
        const i64 cx = tx >= 0 ? tx / Chunk::kSize
                               : (tx + 1) / static_cast<i64>(Chunk::kSize) - 1;
        const i64 cy = ty >= 0 ? ty / Chunk::kSize
                               : (ty + 1) / static_cast<i64>(Chunk::kSize) - 1;
        return {cx, cy};
    }
};

struct ChunkKeyHash {
    u64 operator()(const ChunkKey& k) const {
        const u64 a = static_cast<u64>(k.x) * 0x9E3779B97F4A7C15ull;
        const u64 b = static_cast<u64>(k.y) * 0xC2B2AE3D27D4EB4Full;
        return a ^ (b + 0x165667B19E3779F9ull + (a << 6) + (a >> 2));
    }
};

/// One horizontal slice of the map: background, terrain, foreground, each with
/// its own tiles, its own collision and its own parallax. Layers are what makes
/// a tilemap read as a place rather than a grid.
struct TilemapLayer {
    std::string name;
    /// World units per tile. Usually 1.0; smaller for far background layers
    /// that tile more finely than the gameplay grid.
    f32 tile_size = 1.0f;
    /// Parallax factor handed to Camera2D. 1.0 locks the layer to the world.
    f32 parallax = 1.0f;
    /// Collision geometry is extracted from this layer only when true — a
    /// decorative layer never produces rects.
    bool collidable = true;
    /// Tiles auto-tile against their neighbours when true (Section "Auto
    /// tiling" below). Off for hand-authored layers where the artist picked
    /// every variant.
    bool auto_tile = false;
    /// Atlas page + the id of the tile whose neighbourhood decides the variant.
    /// Kept here rather than per-tile because a whole layer shares one tileset.
    u32 atlas_page = 0;

    std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> chunks;

    /// Tile access. Out-of-range local coordinates and missing chunks return
    /// kEmptyTile, so callers never need to bounds-check against the map.
    u32 get(i64 tx, i64 ty) const {
        const ChunkKey key = ChunkKey::from_tile(tx, ty);
        auto found = chunks.find(key);
        if (found == chunks.end()) return kEmptyTile;
        const Chunk& c = found->second;
        const u32 lx = static_cast<u32>(tx - key.x * Chunk::kSize);
        const u32 ly = static_cast<u32>(ty - key.y * Chunk::kSize);
        return c.at(lx, ly);
    }

    /// Writes a tile, creating the chunk if needed. Returns whether the cell
    /// actually changed — write coalescing in the editor uses this to skip
    /// rebakes.
    bool set(i64 tx, i64 ty, u32 packed) {
        const ChunkKey key = ChunkKey::from_tile(tx, ty);
        Chunk& c = chunks[key];
        const u32 lx = static_cast<u32>(tx - key.x * Chunk::kSize);
        const u32 ly = static_cast<u32>(ty - key.y * Chunk::kSize);
        u32& slot = c.at(lx, ly);
        if (slot == packed) return false;
        slot = packed;
        c.generated = true;
        return true;
    }

    bool is_solid(i64 tx, i64 ty, const class Tileset& ts) const;
};

/// Tileset: id -> material, plus the auto-tile neighbour table.
class Tileset {
public:
    void set_material(u32 id, const TileMaterial& material) {
        if (id >= m_materials.size()) m_materials.resize(id + 1u);
        m_materials[id] = material;
    }

    const TileMaterial& material(u32 id) const {
        if (id < m_materials.size()) return m_materials[id];
        return m_default;
    }

    /// Marks `id` as an auto-tile base whose lookups use bitmask-of-8-neighbours
    /// variant selection, and records how many variants the tileset holds for
    /// it. §55 "Auto tiling": the rendered variant is chosen from the tile's
    /// neighbourhood, so a single logical tile draws as a corner, an edge, or
    /// an interior without the author painting each one.
    void add_auto_tile(u32 id, u32 variant_count) {
        if (id >= m_auto_tiles.size()) m_auto_tiles.resize(id + 1u, 0u);
        m_auto_tiles[id] = variant_count;
    }

    bool is_auto_tile(u32 id) const {
        return id < m_auto_tiles.size() && m_auto_tiles[id] > 0u;
    }

    /// Number of variants the tileset stores for this auto-tile.
    u32 variant_count(u32 id) const {
        return id < m_auto_tiles.size() ? m_auto_tiles[id] : 0u;
    }

    void clear() {
        m_materials.clear();
        m_auto_tiles.clear();
    }

private:
    std::vector<TileMaterial> m_materials;
    std::vector<u32> m_auto_tiles;
    TileMaterial m_default{};
};

/// Neighbour bitmask for auto-tiling. Bit set = matching neighbour present.
/// The 8-bit mask indexes a 256-entry variant table; the tileset's job is to
/// say which of its variants covers which mask, the tilemap's job is only to
/// compute the mask cheaply.
enum AutoTileBits : u32 {
    AT_N = 1u << 0, AT_NE = 1u << 1, AT_E = 1u << 2, AT_SE = 1u << 3,
    AT_S = 1u << 4, AT_SW = 1u << 5, AT_W = 1u << 6, AT_NW = 1u << 7,
};

/// Collision rect list extracted from a layer's solid tiles.
struct CollisionMesh {
    /// World-space rectangles. merged greedily along rows then columns so a
    /// 64-tile floor is one rect rather than 64 — the physics broadphase and
    /// the navigation grid both scale with rect count, not tile count.
    std::vector<Rect> rects;
    /// One entry per rect, bit TileFlag_OneWay set when the rect came from
    /// one-way tiles. Parallel rather than folded into Rect to keep Rect a pure
    /// geometry type.
    std::vector<u32> flags;
};

/// Bit in CollisionMesh::flags for a rect built from one-way tiles.
inline constexpr u32 kCollisionOneWayBit = 1u;

/// Navigation grid derived from the collidable layers. Cell (cx, cy) is
/// walkable when no solid tile covers its centre.
struct NavGrid2D {
    /// Grid resolution in world units per cell. 1.0 = one nav cell per tile,
    /// which matches how tilemaps are actually authored.
    f32 cell_size = 1.0f;
    /// Origin offset: nav cell 0,0 covers world [origin.x, origin.x + cell).
    Vec2 origin{0.0f, 0.0f};
    i64 width = 0;
    i64 height = 0;

    /// Row-major walkability: true = passable. Sized width*height; empty grid
    /// = nothing walkable, which makes a fresh NavGrid2D safe to query.
    std::vector<bool> walkable;

    bool in_bounds(i64 cx, i64 cy) const {
        return cx >= 0 && cy >= 0 && cx < width && cy < height;
    }

    bool is_walkable(i64 cx, i64 cy) const {
        if (!in_bounds(cx, cy)) return false;
        return walkable[static_cast<usize>(cy * width + cx)];
    }

    Vec2 cell_centre(i64 cx, i64 cy) const {
        return Vec2{origin.x + (static_cast<f32>(cx) + 0.5f) * cell_size,
                    origin.y + (static_cast<f32>(cy) + 0.5f) * cell_size};
    }

    void cell_at_world(Vec2 world, i64& out_cx, i64& out_cy) const {
        const f32 fx = (world.x - origin.x) / cell_size;
        const f32 fy = (world.y - origin.y) / cell_size;
        // Floor toward negative infinity: world left of the origin is cell -1.
        out_cx = static_cast<i64>(std::floor(fx));
        out_cy = static_cast<i64>(std::floor(fy));
    }
};

/// A found path, world-space, start excluded and end included. Empty when no
/// route exists.
struct NavPath {
    std::vector<Vec2> waypoints;
    f32 cost = 0.0f;
    bool found = false;
};

class Tilemap {
public:
    std::vector<TilemapLayer> layers;
    Tileset tileset;

    /// World units per tile for the gameplay layer. Kept on the map (not the
    /// layer) because physics, navigation and the camera all reason in the
    /// gameplay grid's units; decorative layers override theirs locally.
    f32 tile_size = 1.0f;

    TilemapLayer& add_layer(const std::string& name, f32 parallax = 1.0f,
                            bool collidable = true, bool auto_tile = false);

    TilemapLayer* layer(const std::string& name);
    const TilemapLayer* layer(const std::string& name) const;

    // ------------------------------------------------------------------
    // Tile access across layers
    // ------------------------------------------------------------------

    u32 get(usize layer_index, i64 tx, i64 ty) const;
    bool set(usize layer_index, i64 tx, i64 ty, u32 packed);

    /// True when any collidable layer at this tile is solid for a body moving
    /// in `direction` (one-way tiles are only solid when the body comes from
    /// above). Direction is the body's movement: y > 0 means moving down.
    bool is_solid_at(i64 tx, i64 ty, f32 dir_y = 1.0f) const;

    // ------------------------------------------------------------------
    // §55 "Auto tiling"
    // ------------------------------------------------------------------

    /// Computes the 8-neighbour mask for a tile, where a neighbour counts when
    /// it is the same logical tile id. Corner bits are only set when both
    /// adjacent edges are — a diagonal next to a gap should not draw a corner.
    u32 auto_tile_mask(const TilemapLayer& layer, i64 tx, i64 ty, u32 id) const;

    /// Resolves a tile to its rendered variant: the auto-tile id plus the mask
    /// folded into a variant index in [0, variant_count). Returns the original
    /// id when it is not an auto-tile.
    u32 resolve_variant(const TilemapLayer& layer, i64 tx, i64 ty) const;

    // ------------------------------------------------------------------
    // §55 "Collision"
    // ------------------------------------------------------------------

    /// Builds the collision mesh for a layer: runs of solid tiles merge into
    /// row segments, then stacked segments of identical width merge into
    /// rectangles. Deterministic — same tiles in, same rects out, in the same
    /// order, which is what lets a cooked collision file be checked into the
    //  repo and byte-compared.
    CollisionMesh extract_collision(const TilemapLayer& layer) const;

    /// Extracts collision from every collidable layer into one list, sorted by
    /// (y, x) for reproducibility. This is the list the physics world consumes.
    std::vector<Rect> extract_all_collision() const;

    // ------------------------------------------------------------------
    // §55 "Navigation"
    // ------------------------------------------------------------------

    /// Rasterises the collidable layers into a nav grid covering `bounds` at
    /// `cell_size` units per cell. A cell is walkable when its centre is not
    /// inside a solid rect.
    NavGrid2D build_nav_grid(Rect bounds, f32 cell_size = 1.0f) const;

    /// A* over the nav grid with octile movement, a binary-heap open set and a
    /// lexicographic tie-break on (cost, then cell) — the tie-break is the
    //  determinism guarantee, since an unbroken A* returns whichever of two
    //  equal-cost paths the heap happened to pop first.
    NavPath find_path(const NavGrid2D& grid, Vec2 start_world,
                      Vec2 goal_world, f32 goal_radius = 0.5f) const;

    // ------------------------------------------------------------------
    // §55 "Large maps" / "Streaming"
    // ------------------------------------------------------------------

    /// Signature of a chunk generator: given a layer and chunk coordinates,
    /// fill the chunk. Deterministic by contract — the same (layer, cx, cy)
    /// must always produce the same tiles, so a chunk can be unloaded and
    /// regenerated later without the world visibly changing.
    using ChunkGenerator =
        std::function<void(const std::string& layer_name, i64 cx, i64 cy, Chunk&)>;

    /// Installs the generator used to fill chunks on demand.
    void set_chunk_generator(ChunkGenerator generator) {
        m_generator = std::move(generator);
    }

    /// Ensures every chunk within `radius` chunks of (centre_tx, centre_ty) is
    /// generated and present. Chunks outside the radius are *not* dropped
    /// here — see `unload_outside` — because a caller may want to pin regions.
    void update_streaming(i64 centre_tx, i64 centre_ty, u32 radius_chunks);

    /// Drops chunks outside the radius, keeping at most `extra` recently-used
    /// chunks beyond it. Eviction is by distance from the centre, ties broken
    /// by chunk coordinate, so the same camera position always evicts the same
    /// chunks and memory use is stable across runs.
    void unload_outside(i64 centre_tx, i64 centre_ty, u32 radius_chunks,
                        u32 extra = 0);

    /// Number of loaded chunks across all layers (profiler stat: a streaming
    /// leak shows up here long before it shows up as a frame hitch).
    usize loaded_chunk_count() const;

    bool has_chunk(usize layer_index, i64 cx, i64 cy) const;

private:
    ChunkGenerator m_generator;
};

} // namespace nf::scene2d
