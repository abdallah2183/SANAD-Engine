#pragma once

// NF/Scene2D/SpriteAtlas.hpp — texture atlas: named regions + deterministic
// rect packing.
// Design doc Section 54 (2D Engine): "Atlas".

#include <NF/Core/Types.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nf::scene2d {

/// A rectangle inside a texture page, in texels. `rotated` means the art is
/// stored turned 90 degrees clockwise and the batcher must transpose the UVs to
/// display it upright — packing rotates to fit, and the atlas records it so the
/// caller never has to reason about it.
struct AtlasRegion {
    u32 x = 0;
    u32 y = 0;
    u32 w = 0;
    u32 h = 0;
    u32 page = 0;
    bool rotated = false;
};

/// A rectangle awaiting placement, kept as a struct so the packer can sort a
/// vector of them without touching the region table.
struct PackingEntry {
    u32 index = 0;        ///< Index into the region list this entry refers to.
    u32 w = 0;
    u32 h = 0;
    std::string name;
};

class SpriteAtlas {
public:
    /// Adds a region placed by hand (artist-authored atlas). Returns its index.
    u32 add_region(const std::string& name, u32 x, u32 y, u32 w, u32 h,
                   u32 page = 0, bool rotated = false);

    /// Queues a region for `pack`. Nothing is placed until pack runs; the
    /// returned index is stable and refers to the region once packed.
    u32 add_pending(const std::string& name, u32 w, u32 h);

    /// Shelf-packs everything queued by `add_pending` into pages of the given
    /// size, assigning x/y/page to those regions.
    ///
    /// Determinism is the point of this routine: entries are sorted by
    /// (height desc, width desc, name asc) — the name tie-break is what makes
    /// two runs over the same input produce identical layouts, since equal-size
    /// rects are otherwise at the mercy of allocation order.
    ///
    /// `padding` keeps sprites from bleeding into each other at mipmap
    /// boundaries. Returns the number of pages used. Regions that do not fit
    /// are listed in `out_unplaced` and stay unplaced (x=y=0) rather than
    /// silently overlapping another sprite.
    u32 pack(u32 page_w, u32 page_h, u32 padding = 1,
             std::vector<std::string>* out_unplaced = nullptr);

    void clear() {
        m_regions.clear();
        m_by_name.clear();
        m_pending.clear();
    }

    const AtlasRegion* region(const std::string& name) const;
    AtlasRegion* region(const std::string& name);
    const AtlasRegion* region_by_index(u32 index) const;
    u32 region_index(const std::string& name) const;
    usize region_count() const { return m_regions.size(); }
    const std::string& region_name(u32 index) const { return m_names[index]; }

    /// UV bounds for a region on a page of the given pixel size, ready for a
    /// vertex buffer. Half-texel inset is optional: it is what stops bilinear
    /// filtering from picking up the neighbouring sprite's edge, and it is left
    /// to the caller because a nearest-filtered pixel-art atlas does not want
    /// it (and would show a one-texel shift if it got it).
    void uv_bounds(u32 index, u32 page_w, u32 page_h, f32& out_u0, f32& out_v0,
                   f32& out_u1, f32& out_v1, bool half_texel_inset = false) const;

    u32 pending_count() const { return static_cast<u32>(m_pending.size()); }

private:
    std::vector<AtlasRegion> m_regions;
    std::vector<std::string> m_names;
    std::unordered_map<std::string, u32> m_by_name;
    std::vector<PackingEntry> m_pending;
};

} // namespace nf::scene2d
