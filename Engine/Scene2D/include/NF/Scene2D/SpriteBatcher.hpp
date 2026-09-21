#pragma once

// NF/Scene2D/SpriteBatcher.hpp — sprite collection, sorting and vertex baking.
// Design doc Section 54 (2D Engine): "Sprite batching".
//
// The renderer-facing half of the 2D pipeline. Sprites are pushed during the
// simulation, then `end()` sorts them into draw batches and bakes a single
// interleaved vertex/index array per texture page, so a screen full of
// hundreds of tiles costs a handful of draw calls instead of hundreds.
//
// Determinism (design doc Section 114): the sort is std::stable_sort over
// (page, depth), so equal-key sprites keep submission order and two identical
// update sequences bake identical buffers.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include "Camera2D.hpp"
#include "Math2D.hpp"

#include <cstdint>
#include <vector>

namespace nf::scene2d {

/// One sprite's renderable description. Positions are in world units; the
/// batcher applies the camera, so this struct stays view-independent and a
/// recorded sprite list replays correctly under any camera.
struct SpriteDraw {
    /// Texture page; sprites sharing a page share a batch (one draw call).
    u32 page = 0;
    /// UV bounds already resolved from the atlas. Keeping UVs here rather than
    /// an atlas reference means the batcher has no atlas dependency and the
    /// renderer does not need one either.
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 1.0f;
    /// World-space position of the sprite's anchor point.
    Vec2 position{0.0f, 0.0f};
    /// World-space width/height.
    Vec2 size{1.0f, 1.0f};
    /// Rotation about the anchor, degrees, clockwise.
    f32 rotation_deg = 0.0f;
    /// Anchor as a fraction of size: (0,0) top-left, (0.5,0.5) centre.
    Vec2 anchor{0.5f, 0.5f};
    /// Packed RGBA8 (0xAABBGGRR on little-endian). 0xFFFFFFFF = opaque white.
    u32 color = 0xFFFFFFFFu;
    /// Sort key inside a page: lower is drawn first (further back).
    f32 depth = 0.0f;
    bool flip_x = false;
    bool flip_y = false;
    /// Parallax scroll factor for this sprite's layer (Camera2D applies it).
    f32 parallax = 1.0f;
};

/// Interleaved vertex. Deliberately small (20 bytes): position + UV + colour,
/// no normals or tangents — a 2D renderer's only lighting input is the light
/// buffer, sampled in the pixel shader.
struct SpriteVertex {
    f32 x;
    f32 y;
    f32 u;
    f32 v;
    u32 color;
};

/// A contiguous run of vertices sharing a texture page — one draw call.
struct SpriteBatch {
    u32 page = 0;
    u32 vertex_offset = 0;
    u32 vertex_count = 0;
    u32 index_offset = 0;
    u32 index_count = 0;
    f32 min_depth = 0.0f;
    f32 max_depth = 0.0f;
};

/// Packs RGBA8. R lives in the low byte so `color = 0x000000FF` is opaque red,
/// matching how the engine's Vec3 colours map to channels.
inline u32 pack_color(u8 r, u8 g, u8 b, u8 a) {
    return (static_cast<u32>(a) << 24) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(g) << 8) | static_cast<u32>(r);
}

inline u32 pack_color_vec(Vec3 rgb, f32 alpha = 1.0f) {
    const auto cl = [](f32 c) {
        return c < 0.0f ? 0 : (c > 1.0f ? 255 : static_cast<int>(c * 255.0f + 0.5f));
    };
    return pack_color(static_cast<u8>(cl(rgb.x)), static_cast<u8>(cl(rgb.y)),
                      static_cast<u8>(cl(rgb.z)),
                      static_cast<u8>(cl(alpha)));
}

class SpriteBatcher {
public:
    /// Clears sprites and baked buffers. Call at the start of a frame, before
    /// any push.
    void begin();

    /// Queues a sprite. Cheap: copies into a staging vector. Nothing is
    /// transformed or sorted until `end`.
    void push(const SpriteDraw& sprite) { m_staging.push_back(sprite); }

    /// Sorts, transforms and bakes. After this the vertex/index arrays are
    /// ready to upload; nothing else may be pushed until the next `begin`.
    void end(const Camera2D& camera);

    /// Bakes without a camera transform — for offscreen tools and tests that
    /// want vertices in the same space the sprites were submitted in.
    void end_identity();

    const std::vector<SpriteVertex>& vertices() const { return m_vertices; }
    const std::vector<u32>& indices() const { return m_indices; }
    const std::vector<SpriteBatch>& batches() const { return m_batches; }
    usize sprite_count() const { return m_staging.size(); }

    /// Stats for the profiler: how many sprites were collapsed into how many
    /// draw calls. A 1000-sprite scene reporting 1000 batches means the atlas
    /// is fragmenting.
    usize draw_call_count() const { return m_batches.size(); }

    void clear() {
        m_staging.clear();
        m_vertices.clear();
        m_indices.clear();
        m_batches.clear();
    }

private:
    void bake(const Camera2D* camera);
    void emit_quad(const SpriteDraw& s, const Mat3x2& view);

    std::vector<SpriteDraw> m_staging;
    std::vector<SpriteVertex> m_vertices;
    std::vector<u32> m_indices;
    std::vector<SpriteBatch> m_batches;
};

} // namespace nf::scene2d
