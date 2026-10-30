#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Scene2D/Tilemap.hpp>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace nf::scene2d {

/// Requirement for a neighboring cell in rule pattern matching
enum class NeighborCondition : u8 {
    DontCare = 0, ///< Any tile or empty cell matches
    ThisTile,     ///< Neighbor must match this rule tile (is present/same type)
    NotThisTile   ///< Neighbor must NOT match (empty or different tile)
};

/// 8 neighbor directions around a center tile (x, y)
/// Bits correspond to AutoTileBits:
/// Bit 0: N  ( 0, -1)
/// Bit 1: NE ( 1, -1)
/// Bit 2: E  ( 1,  0)
/// Bit 3: SE ( 1,  1)
/// Bit 4: S  ( 0,  1)
/// Bit 5: SW (-1,  1)
/// Bit 6: W  (-1,  0)
/// Bit 7: NW (-1, -1)
enum NeighborDirection : u8 {
    Dir_N  = 0,
    Dir_NE = 1,
    Dir_E  = 2,
    Dir_SE = 3,
    Dir_S  = 4,
    Dir_SW = 5,
    Dir_W  = 6,
    Dir_NW = 7,
    Dir_Count = 8
};

/// A single rule matching an 8-neighbor neighborhood to a specific tile variant
struct RuleTileEntry {
    std::array<NeighborCondition, 8> neighbors{
        NeighborCondition::DontCare, NeighborCondition::DontCare,
        NeighborCondition::DontCare, NeighborCondition::DontCare,
        NeighborCondition::DontCare, NeighborCondition::DontCare,
        NeighborCondition::DontCare, NeighborCondition::DontCare
    };
    u32 output_tile_id = 0;
    u32 tile_flags = 0;  ///< Optional TileFlags (FlipX, FlipY, etc.)
    u32 priority = 0;    ///< Higher priority tested first
};

/// 2D Rule Tile engine: 8-neighbor bitmask and pattern-based auto-tiling
class RuleTile {
public:
    explicit RuleTile(u32 default_tile_id = 0, std::string name = "RuleTile");

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void set_name(std::string name) { m_name = std::move(name); }

    [[nodiscard]] u32 default_tile_id() const noexcept { return m_default_tile_id; }
    void set_default_tile_id(u32 id) noexcept { m_default_tile_id = id; }

    /// Add an explicit pattern rule
    void add_rule(const RuleTileEntry& rule);
    void clear_rules();
    [[nodiscard]] const std::vector<RuleTileEntry>& rules() const noexcept { return m_rules; }

    /// Configure canonical 16-tile 4-neighbor autotile set (Cardinal N, E, S, W)
    void configure_standard_16_tile(u32 base_tile_id);

    /// Configure canonical 47-tile 8-neighbor blob autotile set (Wang 3x3 minimal set)
    void configure_standard_47_tile(u32 base_tile_id);

    /// Compute 8-neighbor bitmask around (cx, cy) using a match predicate
    [[nodiscard]] static u8 compute_neighbor_bitmask(
        const std::function<bool(i64, i64)>& match_fn,
        i64 cx, i64 cy
    );

    /// Resolve tile ID and flags around (cx, cy) using a match predicate
    [[nodiscard]] u32 resolve_tile(
        const std::function<bool(i64, i64)>& match_fn,
        i64 cx, i64 cy,
        u32* out_flags = nullptr
    ) const;

    /// Resolve tile ID directly from an 8-bit neighbor bitmask
    [[nodiscard]] u32 resolve_from_bitmask(u8 bitmask, u32* out_flags = nullptr) const;

    /// Update a tile and all its 8 neighbors in a TilemapLayer automatically
    static void apply_autotile_at(
        TilemapLayer& layer,
        const RuleTile& rule_tile,
        i64 cx, i64 cy,
        u32 target_id
    );

private:
    std::string m_name;
    u32 m_default_tile_id = 0;
    std::vector<RuleTileEntry> m_rules;
    std::array<u32, 256> m_bitmask_lookup{};
    bool m_has_lookup_table = false;
};

} // namespace nf::scene2d
