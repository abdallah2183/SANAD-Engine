// NF/Scene2D/SpriteAtlas.cpp — atlas region table and deterministic shelf packer.

#include <NF/Scene2D/SpriteAtlas.hpp>

#include <algorithm>

namespace nf::scene2d {

namespace {

/// Ordering for the packer: tallest first (shelves stay shallow), then widest,
/// then by name. The name is the determinism guarantee — two 16x16 sprites
/// named "a" and "b" always take the same slots no matter how the caller built
/// the list.
bool packer_less(const PackingEntry& a, const PackingEntry& b) {
    if (a.h != b.h) return a.h > b.h;
    if (a.w != b.w) return a.w > b.w;
    return a.name < b.name;
}

} // namespace

u32 SpriteAtlas::add_region(const std::string& name, u32 x, u32 y, u32 w, u32 h,
                            u32 page, bool rotated) {
    const u32 index = static_cast<u32>(m_regions.size());
    auto found = m_by_name.find(name);
    if (found != m_by_name.end()) {
        // Re-registering an existing name rewrites it in place: the index is
        // what batchers index by, so reusing it keeps stale vertices from
        // referring to a dead region.
        const u32 existing = found->second;
        m_regions[existing] = AtlasRegion{x, y, w, h, page, rotated};
        m_names[existing] = name;
        return existing;
    }
    m_by_name[name] = index;
    m_regions.push_back(AtlasRegion{x, y, w, h, page, rotated});
    m_names.push_back(name);
    return index;
}

u32 SpriteAtlas::add_pending(const std::string& name, u32 w, u32 h) {
    const u32 index = add_region(name, 0, 0, w, h, 0, false);
    m_pending.push_back(PackingEntry{index, w, h, name});
    return index;
}

u32 SpriteAtlas::pack(u32 page_w, u32 page_h, u32 padding,
                      std::vector<std::string>* out_unplaced) {
    if (m_pending.empty()) return 0;

    std::vector<PackingEntry> entries = m_pending;
    std::stable_sort(entries.begin(), entries.end(), packer_less);

    u32 pages = 1;
    // Current shelf state on the active page.
    u32 shelf_x = padding;
    u32 shelf_y = padding;
    u32 shelf_h = 0;

    for (const PackingEntry& e : entries) {
        const u32 w = e.w + padding * 2u;
        const u32 h = e.h + padding * 2u;

        if (w > page_w || h > page_h) {
            // Never fits on any page; report it instead of wrapping forever.
            if (out_unplaced) out_unplaced->push_back(e.name);
            continue;
        }

        // Close the current shelf if this rect does not fit beside the others.
        if (shelf_x + w > page_w) {
            shelf_y += shelf_h;
            shelf_x = padding;
            shelf_h = 0;
        }
        // Start a new page if the shelf no longer fits vertically.
        if (shelf_y + h > page_h) {
            ++pages;
            shelf_y = padding;
            shelf_x = padding;
            shelf_h = 0;
        }

        AtlasRegion& r = m_regions[e.index];
        r.x = shelf_x + padding;
        r.y = shelf_y + padding;
        r.page = pages - 1u;
        r.rotated = false;

        shelf_x += w;
        shelf_h = shelf_h > h ? shelf_h : h;
    }

    m_pending.clear();
    return pages;
}

const AtlasRegion* SpriteAtlas::region(const std::string& name) const {
    auto it = m_by_name.find(name);
    return it == m_by_name.end() ? nullptr : &m_regions[it->second];
}

AtlasRegion* SpriteAtlas::region(const std::string& name) {
    auto it = m_by_name.find(name);
    return it == m_by_name.end() ? nullptr : &m_regions[it->second];
}

const AtlasRegion* SpriteAtlas::region_by_index(u32 index) const {
    return index < m_regions.size() ? &m_regions[index] : nullptr;
}

u32 SpriteAtlas::region_index(const std::string& name) const {
    auto it = m_by_name.find(name);
    return it == m_by_name.end() ? u32_max : it->second;
}

void SpriteAtlas::uv_bounds(u32 index, u32 page_w, u32 page_h, f32& out_u0,
                            f32& out_v0, f32& out_u1, f32& out_v1,
                            bool half_texel_inset) const {
    const AtlasRegion* r = region_by_index(index);
    if (!r) {
        out_u0 = out_v0 = 0.0f;
        out_u1 = out_v1 = 1.0f;
        return;
    }
    const f32 inv_w = 1.0f / static_cast<f32>(page_w);
    const f32 inv_h = 1.0f / static_cast<f32>(page_h);
    const f32 inset = half_texel_inset ? 0.5f : 0.0f;

    f32 u0 = (static_cast<f32>(r->x) + inset) * inv_w;
    f32 u1 = (static_cast<f32>(r->x + r->w) - inset) * inv_w;
    f32 v0 = (static_cast<f32>(r->y) + inset) * inv_h;
    f32 v1 = (static_cast<f32>(r->y + r->h) - inset) * inv_h;

    if (r->rotated) {
        // Art is stored turned 90 degrees clockwise: the region's width in
        // texels is the sprite's height, so swap the UV axes to display it
        // upright. Swapping endpoints (not just axes) keeps the winding order
        // consistent with an unrotated sprite.
        std::swap(u0, v0);
        std::swap(u1, v1);
    }

    out_u0 = u0;
    out_v0 = v0;
    out_u1 = u1;
    out_v1 = v1;
}

} // namespace nf::scene2d
