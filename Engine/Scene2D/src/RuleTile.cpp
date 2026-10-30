#include <NF/Scene2D/RuleTile.hpp>

#include <algorithm>
#include <map>

namespace nf::scene2d {

namespace {

struct Offset {
    i64 dx;
    i64 dy;
};

constexpr Offset k_neighbor_offsets[8] = {
    { 0, -1}, // Dir_N
    { 1, -1}, // Dir_NE
    { 1,  0}, // Dir_E
    { 1,  1}, // Dir_SE
    { 0,  1}, // Dir_S
    {-1,  1}, // Dir_SW
    {-1,  0}, // Dir_W
    {-1, -1}  // Dir_NW
};

// Filters out irrelevant corners if adjacent cardinals are missing
u8 canonicalize_blob_47(u8 mask) {
    bool n  = (mask & (1 << Dir_N)) != 0;
    bool e  = (mask & (1 << Dir_E)) != 0;
    bool s  = (mask & (1 << Dir_S)) != 0;
    bool w  = (mask & (1 << Dir_W)) != 0;

    u8 clean = static_cast<u8>(mask & 0x55); // Keep cardinals: 0b01010101 (N, E, S, W)

    if (n && e && (mask & (1 << Dir_NE))) clean |= static_cast<u8>(1 << Dir_NE);
    if (s && e && (mask & (1 << Dir_SE))) clean |= static_cast<u8>(1 << Dir_SE);
    if (s && w && (mask & (1 << Dir_SW))) clean |= static_cast<u8>(1 << Dir_SW);
    if (n && w && (mask & (1 << Dir_NW))) clean |= static_cast<u8>(1 << Dir_NW);

    return clean;
}

} // namespace

RuleTile::RuleTile(u32 default_tile_id, std::string name)
    : m_name(std::move(name))
    , m_default_tile_id(default_tile_id) {
    m_bitmask_lookup.fill(default_tile_id);
}

void RuleTile::add_rule(const RuleTileEntry& rule) {
    m_rules.push_back(rule);
    std::stable_sort(m_rules.begin(), m_rules.end(), [](const RuleTileEntry& a, const RuleTileEntry& b) {
        return a.priority > b.priority;
    });
    m_has_lookup_table = false;
}

void RuleTile::clear_rules() {
    m_rules.clear();
    m_has_lookup_table = false;
    m_bitmask_lookup.fill(m_default_tile_id);
}

void RuleTile::configure_standard_16_tile(u32 base_tile_id) {
    for (u32 m = 0; m < 256; ++m) {
        u32 cardinal_index = 0;
        if (m & (1 << Dir_N)) cardinal_index |= 1u;
        if (m & (1 << Dir_E)) cardinal_index |= 2u;
        if (m & (1 << Dir_S)) cardinal_index |= 4u;
        if (m & (1 << Dir_W)) cardinal_index |= 8u;

        m_bitmask_lookup[m] = base_tile_id + cardinal_index;
    }
    m_has_lookup_table = true;
}

void RuleTile::configure_standard_47_tile(u32 base_tile_id) {
    // Collect all canonical 47 patterns
    std::map<u8, u32> canonical_map;
    u32 current_index = 0;

    for (u32 m = 0; m < 256; ++m) {
        u8 canon = canonicalize_blob_47(static_cast<u8>(m));
        if (canonical_map.find(canon) == canonical_map.end()) {
            canonical_map[canon] = current_index++;
        }
    }

    // Populate lookup table for all 256 masks
    for (u32 m = 0; m < 256; ++m) {
        u8 canon = canonicalize_blob_47(static_cast<u8>(m));
        u32 idx = canonical_map[canon];
        m_bitmask_lookup[m] = base_tile_id + idx;
    }

    m_has_lookup_table = true;
}

u8 RuleTile::compute_neighbor_bitmask(
    const std::function<bool(i64, i64)>& match_fn,
    i64 cx, i64 cy
) {
    u8 mask = 0;
    for (u8 d = 0; d < 8; ++d) {
        if (match_fn(cx + k_neighbor_offsets[d].dx, cy + k_neighbor_offsets[d].dy)) {
            mask |= static_cast<u8>(1u << d);
        }
    }
    return mask;
}

u32 RuleTile::resolve_from_bitmask(u8 bitmask, u32* out_flags) const {
    if (out_flags) {
        *out_flags = 0;
    }

    if (m_has_lookup_table) {
        return m_bitmask_lookup[bitmask];
    }

    for (const auto& rule : m_rules) {
        bool matches = true;
        for (u8 d = 0; d < 8; ++d) {
            bool neighbor_present = (bitmask & (1 << d)) != 0;
            if (rule.neighbors[d] == NeighborCondition::ThisTile && !neighbor_present) {
                matches = false;
                break;
            }
            if (rule.neighbors[d] == NeighborCondition::NotThisTile && neighbor_present) {
                matches = false;
                break;
            }
        }

        if (matches) {
            if (out_flags) {
                *out_flags = rule.tile_flags;
            }
            return rule.output_tile_id;
        }
    }

    return m_default_tile_id;
}

u32 RuleTile::resolve_tile(
    const std::function<bool(i64, i64)>& match_fn,
    i64 cx, i64 cy,
    u32* out_flags
) const {
    u8 mask = compute_neighbor_bitmask(match_fn, cx, cy);
    return resolve_from_bitmask(mask, out_flags);
}

void RuleTile::apply_autotile_at(
    TilemapLayer& layer,
    const RuleTile& rule_tile,
    i64 cx, i64 cy,
    u32 target_id
) {
    auto match_predicate = [&](i64 nx, i64 ny) -> bool {
        u32 raw = layer.get(nx, ny);
        if (raw == kEmptyTile) {
            return false;
        }
        u32 id = tile_id(raw);
        if (target_id == 0) {
            return id != kEmptyTile;
        }
        return id == target_id;
    };

    // Update center and 8 neighbors
    for (i64 dy = -1; dy <= 1; ++dy) {
        for (i64 dx = -1; dx <= 1; ++dx) {
            i64 tx = cx + dx;
            i64 ty = cy + dy;
            u32 current_raw = layer.get(tx, ty);
            if (current_raw != kEmptyTile) {
                u32 flags = 0;
                u32 resolved_id = rule_tile.resolve_tile(match_predicate, tx, ty, &flags);
                layer.set(tx, ty, pack_tile(resolved_id, flags));
            }
        }
    }
}

} // namespace nf::scene2d
