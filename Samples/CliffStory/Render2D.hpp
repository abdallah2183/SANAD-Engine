#pragma once

// Samples/CliffStory/Render2D.hpp — the GPU half of CliffStory.
//
// NFScene2D owns the 2D half of a frame: Camera2D transforms world to screen
// pixels, SpriteBatcher sorts and bakes, Lighting2D folds lights into sprite
// tints. None of it can talk to the GPU — SpriteBatcher::vertices() is a CPU
// array by design, so the same bake stays verifiable in a headless test.
//
// This file is the missing other half, and it is the only place in the project
// that knows about RHI. It does three jobs:
//
//   1. SpriteRenderer — one pipeline, one alpha-blended render pass, one
//      descriptor set per texture page. Takes a baked batch and issues a draw
//      call per page.
//   2. TexturePage — PNG/JPG from disk (or raw pixels) to a sampled texture,
//      decoded through the engine's own stb_image facade.
//   3. GlyphAtlas — a runtime glyph atlas over Amiri, drawn as ordinary sprites
//      so Arabic text rides the same batcher as everything else. Arabic is
//      shaped first through NF/UI's ArabicShaper, because a stock TrueType
//      rasteriser draws logical Arabic letter by letter: disconnected, and in
//      the wrong order.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Platform/InputSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Material.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Scene2D/Camera2D.hpp>
#include <NF/Scene2D/SpriteBatcher.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::cliff {

using scene2d::SpriteBatcher;
using scene2d::SpriteDraw;

/// How a page samples: repeat tiles a seamless texture across a quad (the stone
/// cliff), clamp holds a sprite inside its atlas cell (glyphs, ivy leaves).
enum class PageWrap {
    Repeat,
    Clamp,
};

/// One sampled image. A page index is the batcher's texture-page field, so a
/// sprite naming page 3 is drawn in the same call as every other page-3 sprite.
struct TexturePage {
    std::string name;
    std::unique_ptr<rhi::Texture> texture;
    std::unique_ptr<rhi::TextureView> view;
    std::unique_ptr<rhi::Sampler> sampler;
    u32 width = 0;
    u32 height = 0;
};

/// Vertex as uploaded. SpriteBatcher::SpriteVertex packs its tint into a u32,
/// and the RHI has no integer vertex formats (Format offers R32_SFloat and
/// nothing beside it), so the copy that converts screen pixels to NDC unpacks
/// the tint on the way through — one pass doing both jobs.
struct RenderVertex {
    f32 x = 0.0f, y = 0.0f;   // NDC
    f32 u = 0.0f, v = 0.0f;
    f32 r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

/// Replaces the alpha byte of a packed tint (see scene2d::pack_color), keeping
/// the RGB channels intact.
u32 apply_alpha(u32 packed, f32 alpha);

/// Alpha-blended 2D renderer over the swapchain.
class SpriteRenderer {
public:
    SpriteRenderer() = default;
    ~SpriteRenderer();

    SpriteRenderer(const SpriteRenderer&) = delete;
    SpriteRenderer& operator=(const SpriteRenderer&) = delete;

    /// Builds the pipeline, the pass and the framebuffers. Framebuffers need the
    /// swapchain, so they are built here rather than deferred to first frame.
    bool create(rhi::IGraphicsDevice& device, rhi::Swapchain& swapchain, u32 width,
                u32 height);
    void destroy();

    /// An offscreen colour copy of the frame, readable from the CPU.
    ///
    /// A swapchain image is created with COLOR_ATTACHMENT and nothing else
    /// (Swapchain_Vk.cpp), so it can never be the source of a
    /// copy_texture_to_buffer. The sample therefore keeps its own target, and on
    /// the frame it was asked to capture, draws the same batch into both. That is
    /// one extra draw on one frame, and it is what makes a screenshot proof of the
    /// render rather than a photograph of whatever was on the user's desktop.
    struct CaptureTarget {
        rhi::RenderPass* pass = nullptr;
        rhi::Framebuffer* framebuffer = nullptr;
        bool ready = false;
    };

    /// Creates the offscreen target at the given size, if it is not already.
    bool ensure_capture_target(u32 width, u32 height);

    const CaptureTarget& capture_target() const { return m_capture; }

    /// Reads the offscreen target back as RGBA8.
    bool read_back_capture(std::vector<u8>& rgba_out);

    /// Uploads raw RGBA8 pixels as a new page and returns its index.
    u32 add_page(const std::string& name, u32 width, u32 height, const u8* rgba,
                 PageWrap wrap, bool nearest);

    /// Re-uploads an existing page's pixels in place.
    ///
    /// The glyph atlas is the reason this exists: it is registered with the
    /// renderer as an empty page and then filled in as glyphs are demanded, so
    /// without a way to push the new pixels the atlas would reach the GPU once,
    /// blank, and no text would ever appear.
    bool refresh_page(u32 index, u32 width, u32 height, const u8* rgba);

    /// Decodes an image file through NF/Rendering's stb_image facade. Returns
    /// u32_max and logs on failure, so a missing asset degrades to "one page
    /// short" rather than taking the frame down.
    u32 add_page_from_file(const std::filesystem::path& path, PageWrap wrap,
                           bool nearest);

    /// Converts the baked batch to NDC, uploads it, and issues one draw call per
    /// texture page. Must be called inside a begun render pass.
    void draw(const SpriteBatcher& batcher, rhi::CommandBuffer& cmd);

    u32 page_count() const { return static_cast<u32>(m_pages.size()); }
    const TexturePage* page(u32 index) const;

    /// The pass and framebuffer the draw() calls belong inside. The sample owns
    /// the swapchain, so it also owns the frame loop around this renderer.
    rhi::RenderPass* render_pass() const { return m_render_pass.get(); }
    rhi::Framebuffer* framebuffer(u32 index) const {
        return index < m_framebuffers.size() ? m_framebuffers[index].get() : nullptr;
    }

    /// Sprites submitted and draw calls issued last frame — the batcher's whole
    /// reason for existing. Logged once a second so the number is observable
    /// without a profiler.
    u32 last_sprite_count() const { return m_last_sprites; }
    u32 last_draw_calls() const { return m_last_draw_calls; }
    u32 last_triangle_count() const { return m_last_triangles; }

private:
    struct PageEntry {
        std::unique_ptr<TexturePage> page;
        std::unique_ptr<rendering::MaterialInstance> instance;
    };

    /// Grows a buffer only when the frame outgrew the last one. Recreating a
    /// vertex buffer every frame is the classic way to make a 2D game stutter.
    bool ensure_capacity(usize vertex_bytes, usize index_bytes);

    // Offscreen copy used only for --screenshot. Destroyed before the swapchain.
    std::unique_ptr<rhi::Texture> m_capture_texture;
    std::unique_ptr<rhi::RenderPass> m_capture_pass;
    std::unique_ptr<rhi::Framebuffer> m_capture_framebuffer;
    CaptureTarget m_capture;

    rhi::IGraphicsDevice* m_device = nullptr;
    rhi::Swapchain* m_swapchain = nullptr;
    std::unique_ptr<rendering::PipelineCache> m_pipeline_cache;
    std::unique_ptr<rendering::Material> m_material;
    std::unique_ptr<rhi::DescriptorSetLayout> m_set_layout;
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptor_allocator;
    std::unique_ptr<rhi::ShaderModule> m_vertex_shader;
    std::unique_ptr<rhi::ShaderModule> m_fragment_shader;
    std::unique_ptr<rhi::RenderPass> m_render_pass;
    std::unique_ptr<rhi::Buffer> m_vertex_buffer;
    std::unique_ptr<rhi::Buffer> m_index_buffer;
    std::vector<std::unique_ptr<rhi::Framebuffer>> m_framebuffers;

    std::vector<PageEntry> m_pages;

    std::vector<RenderVertex> m_upload_vertices;
    u32 m_viewport_w = 1;
    u32 m_viewport_h = 1;
    u32 m_last_sprites = 0;
    u32 m_last_draw_calls = 0;
    u32 m_last_triangles = 0;
};

/// A rasterised glyph, positioned in the atlas.
struct Glyph {
    u32 codepoint = 0;
    u16 width = 0;       ///< Coverage bitmap size in texels.
    u16 height = 0;
    /// Bearings from the pen, in texels. Signed on purpose: stb_truetype reports
    /// xoff negative for most glyphs and yoff negative for anything that rises
    /// above the baseline, and storing either in an unsigned type wraps a -5 into
    /// a 65531 and places the glyph thousands of pixels off-screen.
    i16 x_offset = 0;
    i16 y_offset = 0;
    f32 advance = 0.0f;  ///< Pen advance in atlas texels.
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

/// Runtime glyph atlas over a TTF, drawn through the ordinary sprite batcher.
///
/// Glyphs rasterise on demand into a shelf-packed page and are cached by
/// codepoint, so a game whose text is a few hundred lines of Arabic pays for a
/// few hundred glyphs once. The atlas is an ordinary texture page, which is the
/// point: story text, the HUD and the world land in one sorted batch instead of
/// needing a second renderer and a second pipeline.
class GlyphAtlas {
public:
    GlyphAtlas() = default;
    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    /// `pixels_per_em` is the rasterisation size; glyph quads are scaled to the
    /// requested draw size, so one atlas serves every text size in the game.
    /// The page is sized up front rather than grown: a shelf repack would have
    /// to re-rasterise every cached glyph and re-upload the page, and the atlas
    /// is built once at startup anyway.
    bool create(SpriteRenderer& renderer, const std::filesystem::path& font_path,
                u32 pixels_per_em, PageWrap wrap, u32 page_size = 2048);

    /// Registers a UTF-8 string's glyphs and returns the shaped visual form to
    /// draw. Arabic input goes through NF/UI::shape_arabic first.
    std::string prepare(const std::string& utf8);

    /// Measures a string already passed through prepare(), in atlas texels.
    f32 measure(const std::string& visual) const;

    /// Emits one tinted sprite per visible glyph. `centre` aligns the run on
    /// pos.x. The visual string is already in left-to-right display order, so
    /// advancing the pen forwards draws Arabic correctly rather than merely
    /// spelling it correctly.
    void draw(SpriteBatcher& batcher, const std::string& visual, Vec2 pos, f32 size,
              u32 color, f32 depth, f32 alpha = 1.0f, bool centre = false,
              f32 parallax = 1.0f) const;

    f32 line_height() const { return m_line_height; }
    f32 ascender() const { return m_ascender; }

    /// Ascender and line height at a draw size. draw() places text by its
    /// baseline, so a caller laying out from the top of a box needs these to
    /// convert between the two.
    f32 ascent_at(f32 size) const {
        return m_px > 0 ? m_ascender * size / static_cast<f32>(m_px) : 0.0f;
    }
    f32 line_at(f32 size) const {
        return m_px > 0 ? m_line_height * size / static_cast<f32>(m_px) : 0.0f;
    }

    u32 page() const { return m_page; }
    u32 glyph_count() const { return static_cast<u32>(m_glyphs.size()); }

private:
    bool ensure_glyph(u32 codepoint);

    SpriteRenderer* m_renderer = nullptr;
    u32 m_page = u32_max;
    PageWrap m_wrap = PageWrap::Clamp;
    u32 m_px = 48;
    u32 m_page_w = 0;
    u32 m_page_h = 0;
    f32 m_line_height = 0.0f;
    f32 m_ascender = 0.0f;

    std::vector<u8> m_font_bytes;
    std::vector<u8> m_page_pixels;
    void* m_font_data = nullptr;   ///< Raw TTF bytes handed to stb_truetype.
    void* m_font_info = nullptr;   ///< stbtt_fontinfo*
    std::unordered_map<u32, Glyph> m_glyphs;

    struct Shelf {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 height = 0.0f;
    };
    std::vector<Shelf> m_shelves;
    bool m_full = false;
    /// Set when a glyph wrote pixels, cleared once the page has been re-uploaded.
    bool m_pixels_dirty = false;
};

/// Decodes one UTF-8 code point at `index`, advancing it past the sequence.
/// Returns U+FFFD for malformed input rather than reading past the end.
u32 utf8_next(const std::string& s, usize& index);

/// Resolves a relative asset path against candidate roots, returning the first
/// that exists.
std::filesystem::path find_asset(const std::vector<std::filesystem::path>& roots,
                                 const std::string& relative);

/// The directories the sample searches for content: the executable's own
/// directory, the working directory, and its parents up to the engine root.
std::vector<std::filesystem::path> content_roots();

/// Expands a path's parent chain, so "a/b/c" yields a/b/c, a/b, a, "".
std::vector<std::filesystem::path> ancestor_roots(const std::filesystem::path& start);

} // namespace nf::cliff