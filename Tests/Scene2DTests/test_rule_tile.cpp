// Tests/Scene2DTests/test_rule_tile.cpp — 2D Rule Tile & 8-neighbor bitmask tests (Phase 119)

#include <NF/Test/TestFramework.hpp>
#include <NF/Scene2D/RuleTile.hpp>
#include <NF/Scene2D/Tilemap.hpp>

using namespace nf;
using namespace nf::scene2d;

NF_TEST(rule_tile_initial_state) {
    RuleTile rt(10, "GrassRule");
    NF_CHECK_EQ(rt.name(), "GrassRule");
    NF_CHECK_EQ(rt.default_tile_id(), 10u);
    NF_CHECK(rt.rules().empty());
}

NF_TEST(rule_tile_compute_bitmask_cardinals) {
    // Predicate matches only north (0, -1) and east (1, 0)
    auto match_fn = [](i64 x, i64 y) -> bool {
        return (x == 0 && y == -1) || (x == 1 && y == 0);
    };

    u8 mask = RuleTile::compute_neighbor_bitmask(match_fn, 0, 0);
    // Bit 0 = N, Bit 2 = E -> (1 << 0) | (1 << 2) = 1 | 4 = 5
    NF_CHECK_EQ(mask, 5u);
}

NF_TEST(rule_tile_explicit_rule_matching) {
    RuleTile rt(1);

    RuleTileEntry r1;
    r1.neighbors[Dir_N] = NeighborCondition::ThisTile;
    r1.neighbors[Dir_S] = NeighborCondition::NotThisTile;
    r1.output_tile_id = 42;
    rt.add_rule(r1);

    // Matching: N present (bit 0 = 1), S absent (bit 4 = 0)
    u8 mask_match = (1 << Dir_N);
    NF_CHECK_EQ(rt.resolve_from_bitmask(mask_match), 42u);

    // Non-matching: S present (bit 4 = 1)
    u8 mask_fail = (1 << Dir_N) | (1 << Dir_S);
    NF_CHECK_EQ(rt.resolve_from_bitmask(mask_fail), 1u); // falls back to default
}

NF_TEST(rule_tile_rule_priority_order) {
    RuleTile rt(1);

    RuleTileEntry low_prio;
    low_prio.neighbors[Dir_N] = NeighborCondition::ThisTile;
    low_prio.output_tile_id = 10;
    low_prio.priority = 1;

    RuleTileEntry high_prio;
    high_prio.neighbors[Dir_N] = NeighborCondition::ThisTile;
    high_prio.neighbors[Dir_E] = NeighborCondition::ThisTile;
    high_prio.output_tile_id = 20;
    high_prio.priority = 10;

    rt.add_rule(low_prio);
    rt.add_rule(high_prio);

    // Both rules match N + E, but high_prio (20) must be chosen
    u8 mask = (1 << Dir_N) | (1 << Dir_E);
    NF_CHECK_EQ(rt.resolve_from_bitmask(mask), 20u);

    // Only N matches low_prio (10)
    u8 mask_n = (1 << Dir_N);
    NF_CHECK_EQ(rt.resolve_from_bitmask(mask_n), 10u);
}

NF_TEST(rule_tile_fallback_to_default) {
    RuleTile rt(99);
    RuleTileEntry r;
    r.neighbors[Dir_W] = NeighborCondition::ThisTile;
    r.output_tile_id = 5;
    rt.add_rule(r);

    // Mask with only east present
    u8 mask = (1 << Dir_E);
    NF_CHECK_EQ(rt.resolve_from_bitmask(mask), 99u);
}

NF_TEST(rule_tile_standard_16_tile_cardinals) {
    RuleTile rt(100);
    rt.configure_standard_16_tile(100);

    // Isolated (no cardinals) -> 100
    NF_CHECK_EQ(rt.resolve_from_bitmask(0), 100u);

    // N only (bit 0) -> cardinal 1 -> 101
    NF_CHECK_EQ(rt.resolve_from_bitmask(1 << Dir_N), 101u);

    // E only (bit 2) -> cardinal 2 -> 102
    NF_CHECK_EQ(rt.resolve_from_bitmask(1 << Dir_E), 102u);

    // N + E -> cardinal 1 + 2 = 3 -> 103
    NF_CHECK_EQ(rt.resolve_from_bitmask((1 << Dir_N) | (1 << Dir_E)), 103u);

    // All 4 cardinals -> cardinal 1 + 2 + 4 + 8 = 15 -> 115
    u8 all_cardinals = (1 << Dir_N) | (1 << Dir_E) | (1 << Dir_S) | (1 << Dir_W);
    NF_CHECK_EQ(rt.resolve_from_bitmask(all_cardinals), 115u);
}

NF_TEST(rule_tile_standard_47_tile_blob) {
    RuleTile rt(500);
    rt.configure_standard_47_tile(500);

    // All 256 bitmasks must resolve within [500, 546]
    for (u32 m = 0; m < 256; ++m) {
        u32 id = rt.resolve_from_bitmask(static_cast<u8>(m));
        NF_CHECK(id >= 500u);
        NF_CHECK(id <= 546u);
    }

    // Isolated cell maps to base ID
    NF_CHECK_EQ(rt.resolve_from_bitmask(0), 500u);
}

NF_TEST(rule_tile_47_corner_filtering) {
    RuleTile rt(500);
    rt.configure_standard_47_tile(500);

    // When East is present but North is NOT present, NE corner is geometrically irrelevant
    u8 mask_without_ne = (1 << Dir_E);
    u8 mask_with_ne    = (1 << Dir_E) | (1 << Dir_NE);

    // Both should canonicalize to the exact same tile ID
    NF_CHECK_EQ(rt.resolve_from_bitmask(mask_without_ne), rt.resolve_from_bitmask(mask_with_ne));
}

NF_TEST(rule_tile_apply_autotile_layer) {
    TilemapLayer layer;
    layer.set(0, 0, 10);
    layer.set(1, 0, 10);
    layer.set(0, 1, 10);
    layer.set(1, 1, 10);

    RuleTile rt(100);
    rt.configure_standard_16_tile(100);

    // Update autotile around (0, 0)
    RuleTile::apply_autotile_at(layer, rt, 0, 0, 0);

    // (0, 0) has neighbors at East (1, 0) and South (0, 1) -> E(2) + S(4) = 6 -> 106
    u32 tile_00 = tile_id(layer.get(0, 0));
    NF_CHECK_EQ(tile_00, 106u);
}

NF_TEST(rule_tile_flags_propagation) {
    RuleTile rt(1);

    RuleTileEntry r;
    r.neighbors[Dir_E] = NeighborCondition::ThisTile;
    r.output_tile_id = 7;
    r.tile_flags = TileFlag_FlipX;
    rt.add_rule(r);

    u32 flags = 0;
    u32 id = rt.resolve_from_bitmask(1 << Dir_E, &flags);
    NF_CHECK_EQ(id, 7u);
    NF_CHECK_EQ(flags, static_cast<u32>(TileFlag_FlipX));
}
