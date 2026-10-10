// Samples/CliffStory/main.cpp — حكاية الجرف / The Cliff's Tale.
//
// A 2D story climb, and the first game in the engine to render through
// NFScene2D. The split of labour across the three files here is:
//
//   Render2D.cpp    the GPU half: one alpha-blended sprite pipeline, one
//                   descriptor set per texture page, a runtime glyph atlas.
//   CliffWorld.cpp  the level: the cliff's silhouette function, the ledges,
//                   the contents, and the PhysicsWorld2D they live in.
//   main.cpp        the game: camera, lighting, story, HUD, and the frame loop.
//
// The art is the engine's own. Every texture comes from the medieval village
// kit that already ships in the tree (Samples/MedievalVillage/Content/Medieval/
// Textures), and the text is set in the engine's own Arabic face
// (Resources/fonts/Amiri-Regular.ttf), shaped by NF/UI's ArabicShaper.
//
// Nothing here is loaded through the VFS or a .nfscene: the 2D layer is not
// expressible in the scene format yet (see Docs), so this sample talks to
// NFScene2D and the RHI directly and treats the scene file as the 3D path it is.

#include "CliffWorld.hpp"
#include "Render2D.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/InputSystem.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Scene2D/Light2D.hpp>
#include <NF/Scene2D/SpriteBatcher.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace nf::cliff {

namespace {

using scene2d::Camera2D;
using scene2d::Lighting2D;
using scene2d::Light2D;
using scene2d::LightFalloff2D;
using scene2d::Rect;

// ---------------------------------------------------------------------------
// Assets, all already in the tree
// ---------------------------------------------------------------------------

// The medieval village kit. T_RockTrim is a seamless stone-masonry map and is
// what the whole cliff is built out of; the rest dress the shrine at the top.
constexpr const char* kRockTex =
    "Samples/MedievalVillage/Content/Medieval/Textures/T_RockTrim_BaseColor.png";
constexpr const char* kPlasterTex =
    "Samples/MedievalVillage/Content/Medieval/Textures/T_Plaster_BaseColor.png";
constexpr const char* kBrickTex =
    "Samples/MedievalVillage/Content/Medieval/Textures/T_Brick_BaseColor.png";
constexpr const char* kTileTex =
    "Samples/MedievalVillage/Content/Medieval/Textures/T_RoundTiles_BaseColor.png";
constexpr const char* kVineTex =
    "Samples/MedievalVillage/Content/Medieval/Textures/T_VineLeaf.png";
constexpr const char* kFontFile = "Resources/fonts/Amiri-Regular.ttf";

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

constexpr u32 kWindowW = 1280;
constexpr u32 kWindowH = 720;
/// 40 px per world unit shows 32x18 units of cliff: wide enough to read the
/// silhouette, tall enough that a 2.3-unit ledge gap is a comfortable jump.
constexpr f32 kPixelsPerUnit = 40.0f;

/// Physics runs on a fixed step. PhysicsWorld2D is documented as "call from a
/// fixed-timestep loop, never from the render loop", and the engine treats
/// determinism as a contract (design doc §114), so the accumulator is not
/// optional polish here.
constexpr f32 kFixedStep = 1.0f / 120.0f;
constexpr int kMaxSubSteps = 5;

/// Depth keys. The batcher sorts by (page, depth) with a stable sort, so these
/// only have to be consistently ordered — not pushed in order.
constexpr f32 kDepthSky = 0.0f;
constexpr f32 kDepthStars = 2.0f;
constexpr f32 kDepthFar = 10.0f;
constexpr f32 kDepthMid = 20.0f;
constexpr f32 kDepthMist = 26.0f;
constexpr f32 kDepthCliff = 30.0f;
constexpr f32 kDepthLedge = 40.0f;
constexpr f32 kDepthTuft = 44.0f;
constexpr f32 kDepthShrine = 50.0f;
constexpr f32 kDepthHazard = 56.0f;
constexpr f32 kDepthPickup = 58.0f;
constexpr f32 kDepthEnemy = 60.0f;
constexpr f32 kDepthPlayer = 64.0f;
constexpr f32 kDepthMote = 70.0f;
constexpr f32 kDepthGlow = 80.0f;
constexpr f32 kDepthVignette = 90.0f;
constexpr f32 kDepthHud = 100.0f;

/// Parallax factors. Below 1.0 the layer lags the camera, which is the only
/// thing separating "a wall" from "a cliff with air in front of it".
constexpr f32 kParallaxFar = 0.20f;
constexpr f32 kParallaxMid = 0.44f;
constexpr f32 kParallaxMist = 0.62f;

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

const Vec3 kTintWhite{1.0f, 1.0f, 1.0f};
const Vec3 kTintStone{0.86f, 0.80f, 0.74f};
const Vec3 kTintStoneCool{0.66f, 0.66f, 0.76f};
const Vec3 kTintIvy{0.46f, 0.62f, 0.40f};
const Vec3 kTintFlame{1.0f, 0.86f, 0.52f};
const Vec3 kTintEmber{1.0f, 0.62f, 0.26f};
const Vec3 kTintSpike{0.78f, 0.80f, 0.90f};
const Vec3 kTintCloak{0.20f, 0.22f, 0.34f};
const Vec3 kTintSkin{0.86f, 0.72f, 0.60f};
const Vec3 kTintBat{0.16f, 0.14f, 0.22f};
const Vec3 kTintText{1.0f, 0.96f, 0.88f};

/// Sky colour as a function of world height. y grows downward, so this runs
/// dusk at the valley floor to deep night at the summit — climbing into the dark
/// is the story told by the background, not by a line of text.
Vec3 sky_color(f32 world_y) {
    const f32 t = std::clamp((kCliffBottomY - world_y) / kCliffHeight, 0.0f, 1.0f);
    // brightness it out-shouted the cliff and read as a wall of peach rather than as
    // air. Dimming it here is what lets the lit stone be the brightest thing in frame,
  // which is the entire point of carrying a lantern.
  const Vec3 dusk{0.60f, 0.30f, 0.16f};
    const Vec3 violet{0.27f, 0.16f, 0.30f};
    const Vec3 night{0.04f, 0.05f, 0.15f};
    if (t < 0.45f) {
        const f32 k = t / 0.45f;
        return dusk * (1.0f - k) + violet * k;
    }
    const f32 k = (t - 0.45f) / 0.55f;
    return violet * (1.0f - k) + night * k;
}

/// Deterministic scalar in [0,1) — same avalanche hash the level uses, so stars
/// and dust are reproducible without an RNG.
f32 hash01(u32 seed) {
    u32 h = seed * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return static_cast<f32>(h >> 8) / static_cast<f32>(1u << 24);
}

// ---------------------------------------------------------------------------
// Story
// ---------------------------------------------------------------------------

struct StoryBeat {
    f32 at;          ///< Progress up the cliff that triggers it.
    const char* text;
};

constexpr StoryBeat kStory[] = {
    {0.02f, "في قاع الوادي، حيث تنطفئ آخر القطعان، بُني الجرفُ من حجرٍ وصبر."},
    {0.18f, "قالوا إن فوق قمّته مصباحًا لا ينام، يضيء لمن يعرف طريقه."},
    {0.38f, "الريحُ تسأل عمّن يصعد، ولا تجيب إلا بصخرة."},
    {0.58f, "مررتُ برجلٍ حافرٍ في الحجر. كان يعدّ الأيام لا السنين."},
    {0.78f, "قريبٌ... أدفئُ وجهي من دفءِ مصباحٍ لا أملكه."},
    {0.94f, "آخرُ درجة، ثم الضوء."},
};
constexpr int kStoryCount = static_cast<int>(sizeof(kStory) / sizeof(kStory[0]));

constexpr const char* kTitle = "حكاية الجرف";
constexpr const char* kSubtitle = "صعودٌ إلى حيث لا ينام المصباح";
constexpr const char* kStartHint = "اضغط المسافة لتبدأ الصعود";
constexpr const char* kControls =
    "الأسهم أو A و D للحركة   ·   المسافة للقفز   ·   R لإعادة";
constexpr const char* kFlameLabel = "اللهب";
constexpr const char* kFallLabel = "السقطات";
constexpr const char* kEndingTitle = "أُشعل المصباح";
constexpr const char* kEndingText =
    "وصلتَ. أشعلتَ المصباح، فسقط عن الجرف ضوءٌ، وصار للوادي ما ينتظره.";
constexpr const char* kRestartHint = "اضغط R لتبدأ الحكاية من جديد";

// ---------------------------------------------------------------------------
// Ivy
// ---------------------------------------------------------------------------

/// One auto-cropped leaf from T_VineLeaf.png.
struct LeafCell {
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    f32 aspect = 1.0f;  ///< width / height, so a quad keeps the leaf's shape.
};

/// Turns the vine sheet into an atlas of individual leaves.
///
/// The sheet is 512px of scattered leaves on transparent ground, not a clean
/// grid, so slicing it into fixed cells would produce a third of a leaf in half
/// the cells. Instead each grid cell's *opaque bounding box* is cropped out, so
/// every entry is a whole leaf whatever shape the artist drew. Doing this on the
/// CPU at load, from the engine's own decoder, keeps the sample honest about
/// what the asset actually contains.
struct LeafSheet {
    u32 page = u32_max;
    std::vector<LeafCell> cells;
};

LeafSheet build_leaf_sheet(SpriteRenderer& renderer, const std::filesystem::path& path) {
    LeafSheet sheet;
    std::string err;
    const rendering::DecodedImage img = rendering::decode_image_file(path.string(), err);
    if (!img.ok()) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: cannot read the vine sheet: {}", err);
        return sheet;
    }

    constexpr int kGrid = 4;
    const int cw = img.width / kGrid;
    const int ch = img.height / kGrid;

    struct Box {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool empty = true;
    };
    std::vector<Box> boxes;
    boxes.reserve(kGrid * kGrid);

    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            Box box;
            box.x0 = cw;
            box.y0 = ch;
            for (int y = 0; y < ch; ++y) {
                for (int x = 0; x < cw; ++x) {
                    const int px = ((gy * ch + y) * img.width + (gx * cw + x)) * 4;
                    if (img.rgba[px + 3] <= 16) continue;
                    const int ax = gx * cw + x;
                    const int ay = gy * ch + y;
                    box.x0 = std::min(box.x0, ax);
                    box.y0 = std::min(box.y0, ay);
                    box.x1 = std::max(box.x1, ax);
                    box.y1 = std::max(box.y1, ay);
                    box.empty = false;
                }
            }
            if (box.empty || box.x1 - box.x0 < 8 || box.y1 - box.y0 < 8) continue;
            boxes.push_back(box);
        }
    }
    if (boxes.empty()) {
        NF_LOG_WARN(LogCategory::RHI, "CliffStory: the vine sheet had no opaque cells");
        return sheet;
    }

    // Shelf-pack the crops into a fresh page.
    constexpr u32 kSheet = 512;
    std::vector<u8> atlas(static_cast<usize>(kSheet) * kSheet * 4, 0u);
    f32 pen_x = 1.0f;
    f32 pen_y = 1.0f;
    f32 shelf_h = 0.0f;

    for (const Box& box : boxes) {
        const int w = box.x1 - box.x0 + 1;
        const int h = box.y1 - box.y0 + 1;
        if (pen_x + w + 1.0f > static_cast<f32>(kSheet)) {
            pen_x = 1.0f;
            pen_y += shelf_h + 1.0f;
            shelf_h = 0.0f;
        }
        if (pen_y + h + 1.0f > static_cast<f32>(kSheet)) break;

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int sx = (box.y0 + y) * img.width + (box.x0 + x);
                const int dx = static_cast<int>(pen_y + y) * static_cast<int>(kSheet) +
                               static_cast<int>(pen_x + x);
                const usize s = static_cast<usize>(sx) * 4;
                const usize d = (static_cast<usize>(dx)) * 4;
                atlas[d + 0] = img.rgba[s + 0];
                atlas[d + 1] = img.rgba[s + 1];
                atlas[d + 2] = img.rgba[s + 2];
                atlas[d + 3] = img.rgba[s + 3];
            }
        }

        LeafCell cell;
        cell.u0 = pen_x / static_cast<f32>(kSheet);
        cell.v0 = pen_y / static_cast<f32>(kSheet);
        cell.u1 = (pen_x + w) / static_cast<f32>(kSheet);
        cell.v1 = (pen_y + h) / static_cast<f32>(kSheet);
        // Clamped: an unbounded aspect lets one wide crop in the sheet become a
        // leaf wider than the ledge it is rooted on, which reads as a green smear
        // rather than as ivy.
        cell.aspect = std::clamp(static_cast<f32>(w) / static_cast<f32>(h), 0.45f, 1.30f);
        sheet.cells.push_back(cell);

        pen_x += w + 1.0f;
        shelf_h = std::max(shelf_h, static_cast<f32>(h));
    }

    sheet.page = renderer.add_page("ivy", kSheet, kSheet, atlas.data(), PageWrap::Clamp, true);
    NF_LOG_INFO(LogCategory::RHI, "CliffStory: {} ivy leaves cropped from the vine sheet",
                sheet.cells.size());
    return sheet;
}

/// Component-wise product. nf::Vec3 deliberately offers no Vec3*Vec3 — the
/// engine only ever scales a colour by a scalar — but folding a light into a
/// tint is per channel, so this is where that happens.
Vec3 mul(Vec3 a, Vec3 b) {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}

/// Component-wise floor. Keeps a sprite readable when the light on it drops to
/// almost nothing; Lighting2D keeps an ambient floor for the same reason.
Vec3 floor_to(Vec3 v, f32 value) {
    return Vec3{std::max(v.x, value), std::max(v.y, value), std::max(v.z, value)};
}

// ---------------------------------------------------------------------------
// Sprite helpers
// ---------------------------------------------------------------------------

/// Page 0 is the flat white page; the rest are filled in by Game::load_assets.
u32 kFlatPage = 0;

/// A textured quad from `top_left`. `uv_scale` > 0 tiles the page by
/// size / uv_scale, which is how the rock faces get their masonry out of one
/// seamless texture; otherwise the quad samples the whole page.
SpriteDraw quad(u32 page, Vec2 top_left, Vec2 size, Vec3 tint, f32 alpha, f32 depth,
                f32 parallax = 1.0f, f32 uv_scale = 0.0f) {
    SpriteDraw d{};
    d.page = page;
    d.position = top_left;
    d.size = size;
    d.anchor = Vec2{0.0f, 0.0f};
    d.color = scene2d::pack_color_vec(tint, alpha);
    d.depth = depth;
    d.parallax = parallax;
    if (uv_scale > 0.0f) {
        d.u0 = 0.0f;
        d.v0 = 0.0f;
        d.u1 = size.x / uv_scale;
        d.v1 = size.y / uv_scale;
    } else {
        d.u0 = 0.0f;
        d.v0 = 0.0f;
        d.u1 = 1.0f;
        d.v1 = 1.0f;
    }
    return d;
}

/// A solid-colour rectangle on the flat white page.
SpriteDraw solid(Vec2 top_left, Vec2 size, Vec3 tint, f32 alpha, f32 depth,
                 f32 parallax = 1.0f) {
    SpriteDraw d{};
    d.page = kFlatPage;
    d.position = top_left;
    d.size = size;
    d.anchor = Vec2{0.0f, 0.0f};
    d.color = scene2d::pack_color_vec(tint, alpha);
    d.depth = depth;
    d.parallax = parallax;
    // A 1/3 inset into an 8x8 page: bilinear filtering can never reach the edge
    // texels, so a "white" quad cannot pick up a seam.
    d.u0 = 0.34f;
    d.v0 = 0.34f;
    d.u1 = 0.66f;
    d.v1 = 0.66f;
    return d;
}

struct GameConfig {
    u32 max_frames = 0;
    bool validation = false;
    /// Writes a PNG of the final frame and exits. The readback goes through the
    /// RHI, so this captures what the GPU presented rather than whatever a screen
    /// grab happens to catch.
    std::string screenshot;
    /// Start the climb instead of showing the title card. The title waits on a
    /// keypress a scripted run cannot deliver, so this is how the game itself
    /// gets inspected without a human at the keyboard.
    bool skip_title = false;
    /// Drop the climber at a given point up the cliff (0 = valley, 1 = summit).
 /// Inspecting the top of a 130-unit climb by hand is not a reasonable thing to
    /// ask of anyone working on the level.
    f32 warp = -1.0f;
};

/// Minimal PNG writer (stored deflate, one filter pass).
///
/// The RHI can hand back raw RGBA, but the engine has no image *encoder* — stb_image
/// only decodes — so a 200-line zlib-free PNG writer beats adding a dependency for
/// a screenshot path. Stored (uncompressed) deflate blocks are valid PNG: the
/// format does not require compression, only the zlib framing around it.
bool write_png(const std::string& path, u32 width, u32 height, const std::vector<u8>& rgba) {
    std::error_code ec;
    const std::filesystem::path out_path(path);
    if (out_path.has_parent_path() && !std::filesystem::exists(out_path.parent_path())) {
        std::filesystem::create_directories(out_path.parent_path(), ec);
    }

    std::vector<u8> raw;
    raw.reserve(static_cast<usize>(height) * (1 + width * 4));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);  // filter type 0 (None)
        const u8* row = rgba.data() + static_cast<usize>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<usize>(width) * 4);
    }

    auto adler32 = [](const u8* data, usize len) -> u32 {
        u32 a = 1;
        u32 b = 0;
        for (usize i = 0; i < len; ++i) {
            a = (a + data[i]) % 65521u;
            b = (b + a) % 65521u;
        }
        return (b << 16) | a;
    };
    auto crc32 = [](const u8* data, usize len) -> u32 {
        static u32 table[256];
        static bool built = false;
        if (!built) {
            for (u32 n = 0; n < 256; ++n) {
                u32 c = n;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[n] = c;
            }
            built = true;
        }
        u32 c = 0xFFFFFFFFu;
        for (usize i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };

    // zlib stream: header, stored deflate blocks, adler32.
    std::vector<u8> z;
    z.push_back(0x78);
    z.push_back(0x01);
    for (usize off = 0; off < raw.size();) {
        const usize chunk = std::min<usize>(65535u, raw.size() - off);
        const bool last = (off + chunk) >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<u8>(chunk & 0xFFu));
        z.push_back(static_cast<u8>((chunk >> 8) & 0xFFu));
        z.push_back(static_cast<u8>(~chunk & 0xFFu));
        z.push_back(static_cast<u8>((~chunk >> 8) & 0xFFu));
        z.insert(z.end(), raw.begin() + static_cast<isize>(off),
                 raw.begin() + static_cast<isize>(off + chunk));
        off += chunk;
    }
    const u32 adler = adler32(raw.data(), raw.size());
    for (int shift = 24; shift >= 0; shift -= 8) z.push_back(static_cast<u8>((adler >> shift) & 0xFFu));

    auto put_be32 = [](std::vector<u8>& v, u32 value) {
        v.push_back(static_cast<u8>((value >> 24) & 0xFFu));
        v.push_back(static_cast<u8>((value >> 16) & 0xFFu));
        v.push_back(static_cast<u8>((value >> 8) & 0xFFu));
        v.push_back(static_cast<u8>(value & 0xFFu));
    };
    std::vector<u8> file{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    auto chunk = [&](const char type[5], const std::vector<u8>& data) {
        put_be32(file, static_cast<u32>(data.size()));
        std::vector<u8> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        file.insert(file.end(), body.begin(), body.end());
        put_be32(file, crc32(body.data(), body.size()));
    };

    std::vector<u8> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // colour type: RGBA
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return out.good();
}

// ---------------------------------------------------------------------------
// Game
// ---------------------------------------------------------------------------

class Game {
public:
    i32 run(const GameConfig& config);

private:
    bool load_assets();
    void reset();
    void fixed_step(f32 dt);
    void step_player(f32 dt);
    void step_contents(f32 dt);
    void respawn_player();

    void rebuild_lighting();
    void follow_camera(f32 dt);

    void build_frame();
    void draw_sky();
    void draw_parallax_layers();
    void draw_cliff();
    void draw_ledges();
    void draw_shrine();
    void draw_contents();
    void draw_player();
    void draw_motes();
    void draw_glows();
    void draw_hud();

  /// Screen pixel -> world, the exact inverse of the camera forward transform.
    Vec2 px_to_world(Vec2 px) const;

    void text(SpriteBatcher& b, const std::string& visual, Vec2 px, f32 size_px,
          Vec3 tint, f32 alpha = 1.0f, bool centre = false);

    /// A solid rectangle laid out in pixels, pushed as a world-space sprite.
    SpriteDraw hud_solid(Vec2 px, Vec2 px_size, Vec3 tint, f32 alpha, f32 depth);

    // --- resources ---
    Window m_window;
    std::unique_ptr<rhi::IGraphicsDevice> m_device;
    std::unique_ptr<rhi::Swapchain> m_swapchain;
    SpriteRenderer m_renderer;
    GlyphAtlas m_atlas;
    std::unique_ptr<rhi::CommandBuffer> m_cmd;
    std::unique_ptr<rhi::Semaphore> m_image_available;
    std::unique_ptr<rhi::Fence> m_frame_fence;
    std::vector<std::unique_ptr<rhi::Semaphore>> m_render_finished;

    // --- texture pages ---
    u32 m_page_rock = u32_max;
    u32 m_page_plaster = u32_max;
    u32 m_page_brick = u32_max;
    u32 m_page_tile = u32_max;
    LeafSheet m_ivy;

    // --- world ---
    CliffWorld m_world;
    SpriteBatcher m_batcher;
    Camera2D m_camera;
    Lighting2D m_lighting;
    BodyHandle m_player;
    Vec2 m_respawn{0.0f, 0.0f};

    // --- player state ---
    f32 m_coyote = 0.0f;
    f32 m_jump_buffer = 0.0f;
    f32 m_facing = 1.0f;
    bool m_on_ground = false;
    bool m_on_ground_prev = false;
    bool m_jump_held = false;

    // --- progression ---
    enum class Screen { Title, Play, Ending };
    Screen m_screen = Screen::Title;
    f32 m_time = 0.0f;
    f32 m_play_time = 0.0f;
    int m_flames = 0;
    int m_flames_total = 0;
    int m_falls = 0;
    int m_beat = 0;
    f32 m_beat_timer = 0.0f;
    f32 m_best_altitude = 0.0f;

    // --- pre-shaped text ---
    std::string m_title_visual;
    std::string m_subtitle_visual;
    std::string m_start_visual;
    std::string m_controls_visual;
    std::string m_flame_visual;
    std::string m_fall_visual;
    std::string m_ending_title_visual;
    std::string m_ending_text_visual;
    std::string m_restart_visual;
    std::vector<std::string> m_beat_visual;

    // Small cache so a HUD line only re-shapes when its text actually changes.
    std::string m_flame_line_logical;
    std::string m_flame_line_visual;
    std::string m_fall_line_logical;
    std::string m_fall_line_visual;
    std::string m_alt_line_logical;
    std::string m_alt_line_visual;
    f32 m_beats_shown = 0.0f;

    f32 m_accumulator = 0.0f;
    u32 m_frames = 0;
};

bool Game::load_assets() {
    const auto roots = content_roots();

    // A flat white page. Solid rectangles, glows and the sky bands are all a
    // tinted quad on this one texture, which is why the game needs no second
    // pipeline and no untextured path.
    {
        std::vector<u8> white(8 * 8 * 4, 255u);
        kFlatPage = m_renderer.add_page("flat", 8, 8, white.data(), PageWrap::Clamp, true);
    }

    const auto load = [&](const char* relative, PageWrap wrap) {
        const auto path = find_asset(roots, relative);
        if (path.empty()) {
            NF_LOG_ERROR(LogCategory::RHI, "CliffStory: missing asset '{}'", relative);
            return u32_max;
        }
        return m_renderer.add_page_from_file(path, wrap, false);
    };

    // Rock, plaster, brick and the glazed tiles all repeat: the cliff faces and
    // the shrine are built from tiled quads, so they need Repeat addressing.
    m_page_rock = load(kRockTex, PageWrap::Repeat);
    m_page_plaster = load(kPlasterTex, PageWrap::Repeat);
    m_page_brick = load(kBrickTex, PageWrap::Repeat);
    m_page_tile = load(kTileTex, PageWrap::Repeat);

    if (const auto vine = find_asset(roots, kVineTex); !vine.empty()) {
        m_ivy = build_leaf_sheet(m_renderer, vine);
    }

    const auto font = find_asset(roots, kFontFile);
    if (font.empty() || !m_atlas.create(m_renderer, font, 48, PageWrap::Clamp, 2048)) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: no usable Arabic font at '{}'", kFontFile);
        return false;
    }

    // Shape every line of the game once, at startup. Shaping on demand would
    // reallocate and re-walk the string on every frame that a line changed.
    m_title_visual = m_atlas.prepare(kTitle);
    m_subtitle_visual = m_atlas.prepare(kSubtitle);
    m_start_visual = m_atlas.prepare(kStartHint);
    m_controls_visual = m_atlas.prepare(kControls);
    m_flame_visual = m_atlas.prepare(kFlameLabel);
    m_fall_visual = m_atlas.prepare(kFallLabel);
    m_ending_title_visual = m_atlas.prepare(kEndingTitle);
    m_ending_text_visual = m_atlas.prepare(kEndingText);
    m_restart_visual = m_atlas.prepare(kRestartHint);
    m_beat_visual.reserve(kStoryCount);
    for (int i = 0; i < kStoryCount; ++i) {
        m_beat_visual.push_back(m_atlas.prepare(kStory[i].text));
    }

    if (m_page_rock == u32_max) {
   NF_LOG_FATAL(LogCategory::RHI, "CliffStory: the stone texture is required");
  return false;
    }

    // The atlas is filled on demand, so nothing above proves a single glyph was
    // actually rasterised — and a glyph atlas that rasterised nothing is
    // invisible rather than broken, which is the worst kind of bug to chase from
    // a screenshot. Report the count and the shaped length of the title.
    usize non_blank = 0;
    for (usize i = 0; i < m_title_visual.size();) {
        const u32 cp = utf8_next(m_title_visual, i);
        if (cp >= 0x600) ++non_blank;
    }
    NF_LOG_INFO(LogCategory::Core,
       "CliffStory: {} glyphs rasterised; title shaped to {} bytes "
     "({} Arabic code points), atlas page {}",
    m_atlas.glyph_count(), m_title_visual.size(), non_blank, m_atlas.page());
    return true;
}

void Game::reset() {
    m_world.generate();
    m_flames_total = static_cast<int>(m_world.flames().size());
    m_flames = 0;
    m_falls = 0;
    m_beat = 0;
    m_beat_timer = 0.0f;
    m_play_time = 0.0f;
    m_best_altitude = 0.0f;
    m_accumulator = 0.0f;
    m_respawn = m_world.spawn_pos();
    respawn_player();

    m_camera.position = m_respawn + Vec2{0.0f, 2.0f};
    m_camera.zoom = 1.0f;
    m_camera.rotation_deg = 0.0f;
    // Not pixel art: these are 2048px textures, so snapping the camera to the
    // texel grid would buy nothing and would judder at fractional zoom.
    m_camera.pixel_perfect = false;
    m_camera.pixels_per_unit = static_cast<u32>(kPixelsPerUnit);
    m_camera.viewport_w = kWindowW;
    m_camera.viewport_h = kWindowH;
}

void Game::respawn_player() {
    if (m_player.valid()) m_world.physics().destroy_body(m_player);
    m_player = m_world.create_player(m_respawn);
    m_coyote = 0.0f;
    m_jump_buffer = 0.0f;
    m_on_ground = false;
}

void Game::step_player(f32 dt) {
    Body2D* body = m_world.physics().body(m_player);
    if (!body) return;

    auto& input = InputSystem::instance();
    const bool left = input.is_key_down(KeyCode::A) || input.is_key_down(KeyCode::Left);
    const bool right = input.is_key_down(KeyCode::D) || input.is_key_down(KeyCode::Right);
    const bool jump_held = input.is_key_down(KeyCode::Space) || input.is_key_down(KeyCode::Up) ||
                           input.is_key_down(KeyCode::W);
    const bool jump_pressed = input.is_key_pressed(KeyCode::Space) ||
                              input.is_key_pressed(KeyCode::Up) ||
                              input.is_key_pressed(KeyCode::W);

    const f32 dir = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
    if (dir != 0.0f) m_facing = dir;

    // Horizontal: accelerate towards the target and clamp the step so the
    // approach never overshoots and oscillates.
    const f32 target_v = dir * kRunSpeed;
    const f32 accel = m_on_ground ? kRunAccel : kAirAccel;
    const f32 step = accel * dt;
    body->velocity.x += std::clamp(target_v - body->velocity.x, -step, step);

    // Coyote time and jump buffering are both small and both matter: without
    // the first, walking off a ledge eats a jump the player clearly meant; with
    // the second, pressing jump a frame early silently does nothing.
    m_coyote = m_on_ground ? kCoyoteTime : std::max(0.0f, m_coyote - dt);
    m_jump_buffer = jump_pressed ? kJumpBuffer : std::max(0.0f, m_jump_buffer - dt);

    if (m_jump_buffer > 0.0f && m_coyote > 0.0f) {
        body->velocity.y = -kJumpSpeed;
        m_jump_buffer = 0.0f;
        m_coyote = 0.0f;
        m_on_ground = false;
    }

    // Variable jump height: releasing the button on the way up clips the rise,
    // which is what makes a short hop and a full jump the same button.
    if (!jump_held && m_jump_held && body->velocity.y < 0.0f) {
        body->velocity.y *= kJumpCutFactor;
    }
    m_jump_held = jump_held;

    // Landing dust and a little camera shake on a hard arrival.
    if (!m_on_ground_prev && m_on_ground) {
        const f32 impact = std::clamp(body->velocity.y * 0.04f, 0.0f, 1.0f);
        if (impact > 0.25f) {
            m_camera.add_trauma(impact * 0.45f);
            for (int i = 0; i < 5; ++i) {
                Mote mote;
                mote.pos = body->position + Vec2{(hash01(static_cast<u32>(i) * 71u) - 0.5f) * 0.7f,
                                                kPlayerHalf.y};
                mote.vel = Vec2{(hash01(static_cast<u32>(i) * 97u) - 0.5f) * 3.2f, -0.6f};
                mote.max_life = 0.45f;
                mote.life = mote.max_life;
                mote.size = 0.10f + hash01(static_cast<u32>(i) * 53u) * 0.08f;
                mote.color = scene2d::pack_color_vec(Vec3{0.78f, 0.74f, 0.68f}, 0.55f);
                m_world.motes().push_back(mote);
            }
        }
    }
    m_on_ground_prev = m_on_ground;
}

void Game::step_contents(f32 dt) {
    const Body2D* player = m_world.physics().body(m_player);
    if (!player) return;
    const Vec2 pp = player->position;

    // Flames
    for (Flame& flame : m_world.flames()) {
        if (flame.taken) continue;
        const Vec2 d = flame.pos - pp;
        if (d.length_sq() < 1.5f) {
            flame.taken = true;
            ++m_flames;
            for (int i = 0; i < 10; ++i) {
                Mote mote;
                mote.pos = flame.pos;
                mote.vel = Vec2{(hash01(static_cast<u32>(m_flames) * 131u + i) - 0.5f) * 5.0f,
                                (hash01(static_cast<u32>(m_flames) * 61u + i) - 0.5f) * 5.0f};
                mote.max_life = 0.7f;
                mote.life = mote.max_life;
                mote.size = 0.09f;
                mote.color = scene2d::pack_color_vec(kTintFlame, 0.9f);
                mote.glow = true;
                m_world.motes().push_back(mote);
            }
        }
    }

    // Bats always animate. Their sweep is stepped before the overlap test rather
    // than inside it, so a bat the player is nowhere near still moves — an
    // enemy that froze until it was on screen would read as a bug.
    for (Bat& bat : m_world.bats()) {
        if (!bat.alive) continue;
        bat.phase += dt * bat.speed;
        bat.pos = bat.home + Vec2{std::sin(bat.phase) * bat.range, 0.0f};
        bat.wing += dt * 11.0f;
    }

    // Spikes and bats are overlap tests, not contacts: a spike that resolved a
    // contact would be a wall, and a bat that pushed the player would be a lift.
    // Both should only ever cost the player a climb.
    const Rect player_box = Rect::from_center(pp, kPlayerHalf * 0.85f);
    bool hit = false;
    for (const Spike& spike : m_world.spikes()) {
        const Rect box{spike.x - spike.w * 0.5f, spike.y - 0.75f, spike.w, 1.05f};
        if (player_box.overlaps(box)) {
            hit = true;
            break;
        }
    }
    if (!hit) {
        for (const Bat& bat : m_world.bats()) {
            if (!bat.alive) continue;
            if (Rect::from_center(bat.pos, Vec2{0.42f, 0.30f}).overlaps(player_box)) {
                hit = true;
                break;
            }
        }
    }
    if (hit) {
        ++m_falls;
        m_camera.add_trauma(0.7f);
        respawn_player();
        return;
    }

    // Shrine lanterns: the respawn point climbs with the player.
    for (Lantern& lantern : m_world.lanterns()) {
        if (!lantern.lit && (lantern.pos - pp).length_sq() < 2.6f) {
            lantern.lit = true;
            m_respawn = lantern.pos + Vec2{0.0f, -1.1f};
            m_camera.add_trauma(0.25f);
        }
        if (lantern.lit) lantern.flame = std::min(1.0f, lantern.flame + dt * 2.6f);
    }

    // Off the bottom of the world.
    if (pp.y > kCliffBottomY + 5.0f) {
        m_falls++;
        m_camera.add_trauma(0.6f);
        respawn_player();
    }

    // Story beats, keyed to progress rather than to a timer.
    const f32 progress = m_world.progress(pp.y);
    m_best_altitude = std::max(m_best_altitude, progress);
    if (m_beat < kStoryCount && progress >= kStory[m_beat].at) {
        m_beat_timer = 6.0f;
        ++m_beat;
        ++m_beats_shown;
    }
    m_beat_timer = std::max(0.0f, m_beat_timer - dt);
}

void Game::fixed_step(f32 dt) {
    m_world.step_movers(m_time);
    step_player(dt);
    m_world.physics().step(dt, 8, 4);
    m_on_ground = m_world.grounded(m_player);
    step_contents(dt);
    m_world.step_motes(dt, m_camera.position, kWindowW / kPixelsPerUnit,
                       kWindowH / kPixelsPerUnit);
}

void Game::rebuild_lighting() {
    m_lighting.clear_lights();

    const Body2D* player = m_world.physics().body(m_player);
    const Vec2 pp = player ? player->position : m_camera.position;

    // The ambient cools and dims with altitude, so the climb visibly leaves the
    // evening behind. Lighting2D keeps a floor under it: a fully shadowed sprite
    // would be unreadable, which is the engine's own reasoning for the default.
    const f32 alt = m_world.progress(pp.y);
    m_lighting.set_ambient(Vec3{0.30f - 0.13f * alt, 0.28f - 0.13f * alt,
                                0.42f - 0.16f * alt});

    // The lantern the climber carries. This is the light that does most of the
    // work: it is what makes the rock read as a surface with a shape rather than
    // a flat wall of texture.
    Light2D lantern;
    lantern.position = pp + Vec2{m_facing * 0.45f, 0.25f};
    lantern.radius = 17.0f;
    lantern.color = Vec3{1.0f, 0.72f, 0.38f};
    lantern.intensity = 1.75f;
    lantern.falloff = LightFalloff2D::Smooth;
    lantern.flicker_amount = 0.10f;
    lantern.flicker_speed = 5.5f;
    lantern.flicker_phase = 0.37f;
    lantern.cast_shadows = false;
    m_lighting.add_light(lantern);

    for (const Lantern& shrine : m_world.lanterns()) {
        if (shrine.flame <= 0.01f) continue;
        Light2D light = lantern;
        light.position = shrine.pos;
        light.radius = 9.5f;
        light.intensity = 1.1f;
        light.flicker_amount = 0.16f;
        light.flicker_phase = 0.11f;
        m_lighting.add_light(light);
    }

    // The shrine at the summit, visible from far below as the goal.
    Light2D summit;
    summit.position = m_world.shrine_pos();
    summit.radius = 30.0f;
    summit.color = Vec3{1.0f, 0.80f, 0.45f};
    summit.intensity = 1.6f;
    summit.flicker_amount = 0.06f;
    summit.cast_shadows = false;
    m_lighting.add_light(summit);

    // No occluders: Lighting2D bakes shadow wedges per light, and a 130-unit
    // cliff has too many edges to be worth the cost at this scale. Flicker is
    // hash-seeded, so the bake stays deterministic regardless.
    m_lighting.bake(m_time);
}

void Game::follow_camera(f32 dt) {
    const Body2D* player = m_world.physics().body(m_player);
    if (!player) return;

    // Follow in Y with a soft lag; hold X mostly still so the cliff face stays
    // put and the player reads their own movement against the rock.
    const f32 half_h = kWindowH / (2.0f * kPixelsPerUnit);
    const f32 target_y = player->position.y + 1.0f;
    const f32 k = 1.0f - std::exp(-6.5f * dt);
    m_camera.position.y += (target_y - m_camera.position.y) * k;

    // Clamp so the summit is never scrolled past and the valley floor never
    // fills the screen with nothing to climb.
    const f32 min_y = kCliffTopY + half_h - 4.0f;
    const f32 max_y = kCliffBottomY - half_h + 3.0f;
    m_camera.position.y = std::clamp(m_camera.position.y, min_y, max_y);
    m_camera.position.x = 2.5f;

    m_camera.shake_phase += dt;
    m_camera.update(dt);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Game::draw_sky() {
    // Banded gradient tied to world height, so the sky changes as the player
    // climbs instead of being a backdrop that scrolls.
    // 0.34 units, not 3. The gradient is sampled once per band and drawn flat, so
    // six 3-unit bands read as six hard horizontal bands across the widest part of
    // the frame. At 0.34 the step between neighbours is well under a percent and
    // the ramp reads as continuous. 53 quads is not a cost.
    constexpr f32 kBand = 0.34f;
    const f32 top = m_camera.position.y - kWindowH / (2.0f * kPixelsPerUnit);
    const f32 bottom = m_camera.position.y + kWindowH / (2.0f * kPixelsPerUnit);
    const f32 left = m_camera.position.x - kWindowW / (2.0f * kPixelsPerUnit) - 2.0f;
    const f32 width = kWindowW / kPixelsPerUnit + 4.0f;

    for (f32 y = std::floor(top / kBand) * kBand; y < bottom + kBand; y += kBand) {
        const Vec3 tint = sky_color(y + kBand * 0.5f);
        m_batcher.push(solid(Vec2{left, y}, Vec2{width, kBand + 0.1f}, tint, 1.0f, kDepthSky));
    }

    // Stars fade in with altitude: none at dusk, plenty at the summit.
    const f32 star_alpha = std::clamp((m_world.progress(m_camera.position.y) - 0.25f) * 1.9f,
                                       0.0f, 1.0f);
    if (star_alpha > 0.01f) {
        for (int i = 0; i < 70; ++i) {
            const f32 r0 = hash01(static_cast<u32>(i) * 2654435761u);
            const f32 r1 = hash01(static_cast<u32>(i) * 40503u + 7u);
            const f32 r2 = hash01(static_cast<u32>(i) * 22695477u + 13u);
            // Distribute over a band that follows the camera, so the field is
            // endless without any per-star bookkeeping.
            const f32 x = m_camera.position.x + (r0 - 0.5f) * 90.0f;
            const f32 y = top + r1 * (bottom - top);
            const f32 size = 0.06f + r2 * 0.09f;
            const f32 twinkle = 0.72f + 0.28f * std::sin(m_time * 1.7f + r0 * 31.0f);
            m_batcher.push(solid(Vec2{x, y}, Vec2{size, size}, kTintWhite,
                                 star_alpha * twinkle * (0.35f + r2 * 0.65f), kDepthStars));
        }
    }
}

void Game::draw_parallax_layers() {
    // Two silhouette layers behind the playable rock. Each is the same row-slab
    // construction as the near cliff but with its own profile and its own tint
    // pulled towards the sky colour at that height — aerial perspective, which
    // is what actually sells depth.
    struct Layer {
        f32 parallax;
        f32 depth;
        f32 offset;
        f32 amp;
        f32 freq;
        Vec3 tint;
        f32 alpha;
    };
    const Layer layers[] = {
        // The offsets sit to the RIGHT of the playable face on purpose. A distant
        // ridge should appear past the edge of the cliff, not fill the frame: with
        // both layers straddling the centre the three walls overlapped into one
        // purple slab and the sky â€” the thing that gives the cliff its height â€”
        // disappeared entirely.
        {kParallaxFar, kDepthFar, 14.0f, 5.0f, 0.075f, Vec3{0.50f, 0.45f, 0.66f}, 0.95f},
        {kParallaxMid, kDepthMid, 7.0f, 3.4f, 0.165f, Vec3{0.42f, 0.38f, 0.56f}, 0.97f},
    };

    constexpr f32 kStep = 2.6f;
    for (const Layer& layer : layers) {
        const Rect view = m_camera.visible_bounds(layer.parallax);
        // The layer's own coordinates are parallax-scaled, so its visible band
        // has to be divided back out before it is walked.
        const f32 top = view.top() - 4.0f;
        const f32 bottom = view.bottom() + 4.0f;
        const f32 right = view.right() + 12.0f;

        for (f32 y = std::floor(top / kStep) * kStep; y < bottom + kStep; y += kStep) {
            const f32 face_a = layer.offset + layer.amp * std::sin(y * layer.freq) +
                               layer.amp * 0.4f * std::sin(y * layer.freq * 2.7f + 1.1f);
            const f32 face_b = layer.offset + layer.amp * std::sin((y + kStep) * layer.freq) +
                               layer.amp * 0.4f * std::sin((y + kStep) * layer.freq * 2.7f + 1.1f);
            const f32 x = std::min(face_a, face_b);
            const f32 width = right - x;
            if (width <= 0.0f) continue;

            // Blend the layer's own tint with the sky behind it, so the tops of
            // the distant ridges dissolve into the evening.
            const Vec3 sky = sky_color(y);
            const Vec3 tint = layer.tint * 0.55f + sky * 0.45f;
            m_batcher.push(quad(m_page_rock, Vec2{x, y}, Vec2{width, kStep + 0.2f}, tint,
                                layer.alpha, layer.depth, layer.parallax, kRockUvScale));
        }
    }

    // Haze between the layers and the playfield. This started as a row of
    // low-alpha bars, which is the cheapest depth cue there is and also the worst
    // looking one: hard horizontal edges across the whole frame read as banding
    // artefacts rather than as air. A gradient that is thin in the middle of the
    // view and zero at the edges has no edges to notice.
  {
        const Rect view = m_camera.visible_bounds(kParallaxMist);
        constexpr int kSteps = 14;
        const f32 h = (view.h + 14.0f) / static_cast<f32>(kSteps);
        for (int i = 0; i < kSteps; ++i) {
   const f32 f = (static_cast<f32>(i) + 0.5f) / kSteps;
       // Gaussian-ish falloff, peak slightly below centre like real valley mist.
       const f32 d = (f - 0.62f) / 0.34f;
         const f32 a = 0.085f * std::exp(-d * d);
     if (a < 0.004f) continue;
    m_batcher.push(solid(Vec2{view.left() - 8.0f, view.top() - 7.0f + f * (view.h + 14.0f)},
       Vec2{view.w + 16.0f, h * 1.15f},
          Vec3{0.62f, 0.60f, 0.66f}, a, kDepthMist, kParallaxMist));
        }
    }
}

void Game::draw_cliff() {
    // The playable rock, built as horizontal slabs following cliff_face_x(). Each
    // slab starts at the leftmost of its two ends so consecutive slabs overlap
    // and no seam can open along a slope.
    constexpr f32 kStep = 1.1f;
    const Rect view = m_camera.visible_bounds();
    const f32 top = std::max(view.top() - 2.0f, kCliffTopY - 4.0f);
    const f32 bottom = view.bottom() + 2.0f;
    const f32 right = kCliffRightX;

    for (f32 y = std::floor(top / kStep) * kStep; y < bottom + kStep; y += kStep) {
        const f32 face_a = cliff_face_x(y);
        const f32 face_b = cliff_face_x(y + kStep);
        const f32 x = std::min(face_a, face_b);
        const f32 width = right - x;
        if (width <= 0.0f) continue;

        // One lighting sample per slab: the cliff is 40 units wide and the light
        // falls off over 13, so sampling the face rather than the whole slab is
        // both cheaper and more accurate to what the player can see.
        const Vec3 lit = m_lighting.sample(Vec2{x + 0.6f, y + kStep * 0.5f});
        m_batcher.push(quad(m_page_rock, Vec2{x, y}, Vec2{width, kStep + 0.15f},
                            mul(kTintStone, lit), 1.0f, kDepthCliff, 1.0f, kRockUvScale));

    }

    // Ivy and grass rooted on the ledge tops.
    for (const Tuft& tuft : m_world.tufts()) {
        if (tuft.pos.y < view.top() - 2.0f || tuft.pos.y > view.bottom() + 2.0f) continue;
        if (m_ivy.page == u32_max || m_ivy.cells.empty()) continue;
        const LeafCell& leaf = m_ivy.cells[tuft.cell % m_ivy.cells.size()];
        const Vec3 lit = m_lighting.sample(tuft.pos);
        SpriteDraw d{};
        d.page = m_ivy.page;
        d.u0 = leaf.u0;
        d.v0 = leaf.v0;
        d.u1 = leaf.u1;
        d.v1 = leaf.v1;
        // The leaf's tip hangs below the anchor, so the quad is raised by half its
        // height to sit on the ledge.
        const f32 h = tuft.size;
        const f32 w = h * leaf.aspect;
        d.size = Vec2{w, h};
        d.position = Vec2{tuft.pos.x - w * 0.5f, tuft.pos.y - h * 0.92f};
        d.anchor = Vec2{0.0f, 0.0f};
        d.rotation_deg = tuft.rotation;
        d.color = scene2d::pack_color_vec(mul(kTintIvy, floor_to(lit, 0.25f)), 0.95f);
        d.depth = kDepthTuft;
        d.parallax = tuft.parallax;
        m_batcher.push(d);
    }
}

void Game::draw_ledges() {
    const Rect view = m_camera.visible_bounds();
    for (const Ledge& ledge : m_world.ledges()) {
        if (ledge.y + ledge.h < view.top() - 2.0f || ledge.y > view.bottom() + 2.0f) continue;
        const f32 x = ledge.x + ledge.offset;

        // Body: stone, sampled with the cliff's lighting.
        const Vec3 lit = m_lighting.sample(Vec2{x + ledge.w * 0.5f, ledge.y});
        m_batcher.push(quad(m_page_rock, Vec2{x, ledge.y}, Vec2{ledge.w, ledge.h},
                            mul(kTintStone, lit), 1.0f, kDepthLedge, 1.0f, kRockUvScale));

        // Top surface: a band that catches the light, hotter at the two ends. This is
        // the game's only affordance for "you can stand here", so it is drawn
        // emphatic â€” but it is lit like everything else. Unlit, these strips read as
        // glowing white bars pasted over the rock.
  m_batcher.push(solid(Vec2{x, ledge.y - 0.16f}, Vec2{ledge.w, 0.26f},
   mul(kTintStone, lit) * 1.45f, 0.92f, kDepthLedge + 0.5f));
        const Vec3 cap = mul(kTintStone, lit) * 1.9f;
        m_batcher.push(solid(Vec2{x, ledge.y - 0.16f}, Vec2{0.20f, 0.26f}, cap, 0.92f,
           kDepthLedge + 0.6f));
        m_batcher.push(solid(Vec2{x + ledge.w - 0.20f, ledge.y - 0.16f}, Vec2{0.20f, 0.26f},
   cap, 0.92f, kDepthLedge + 0.6f));

        // Underside shadow, so a ledge reads as a ledge and not a painted stripe.
        m_batcher.push(solid(Vec2{x, ledge.y + ledge.h}, Vec2{ledge.w, 0.45f},
                             Vec3{0.0f, 0.0f, 0.0f}, 0.32f, kDepthLedge + 0.4f));

        // Sliders get a hint of motion: two stone pins and a worn track.
        if (ledge.moving) {
            m_batcher.push(solid(Vec2{x + ledge.w * 0.5f - 0.12f, ledge.y - 0.34f},
                                 Vec2{0.24f, 0.34f}, kTintStoneCool * 1.4f, 0.9f,
                                 kDepthLedge + 0.7f));
            m_batcher.push(solid(Vec2{x - ledge.travel - 0.5f, ledge.y + ledge.h * 0.5f - 0.05f},
                                 Vec2{ledge.w + ledge.travel * 2.0f + 1.0f, 0.10f},
                                 Vec3{0.0f, 0.0f, 0.0f}, 0.22f, kDepthLedge - 0.5f));
        }
    }
}

void Game::draw_shrine() {
    const Vec2 s = m_world.shrine_pos();
    const Rect view = m_camera.visible_bounds();
    if (s.y < view.top() - 14.0f || s.y > view.bottom() + 14.0f) return;

    const Vec3 lit = m_lighting.sample(s);

    // Stepped base in glazed tile.
    m_batcher.push(quad(m_page_tile, Vec2{s.x - 3.0f, s.y + 1.0f}, Vec2{6.0f, 0.55f},
                        mul(kTintStoneCool, lit), 1.0f, kDepthShrine, 1.0f, 4.0f));
    m_batcher.push(quad(m_page_tile, Vec2{s.x - 2.4f, s.y + 0.45f}, Vec2{4.8f, 0.55f},
                        mul(kTintStoneCool, lit) * 1.1f, 1.0f, kDepthShrine + 0.1f, 1.0f,
                        4.0f));

    // Two columns and a lintel, in brick.
    for (const f32 side : {-1.9f, 1.9f}) {
        m_batcher.push(quad(m_page_brick, Vec2{s.x + side - 0.30f, s.y - 3.4f},
                            Vec2{0.60f, 4.0f}, mul(kTintStone, lit), 1.0f,
                            kDepthShrine + 0.2f, 1.0f, 3.0f));
    }
    m_batcher.push(quad(m_page_brick, Vec2{s.x - 2.6f, s.y - 4.1f}, Vec2{5.2f, 0.75f},
                        mul(kTintStone, lit) * 1.15f, 1.0f, kDepthShrine + 0.3f, 1.0f,
                        3.0f));

    // Plaster back wall, lit from the front by its own flame.
    m_batcher.push(quad(m_page_plaster, Vec2{s.x - 2.2f, s.y - 3.4f}, Vec2{4.4f, 3.5f},
                        mul(kTintStone, lit) * 1.1f, 0.92f, kDepthShrine + 0.15f, 1.0f,
                        5.0f));

    // The brazier: a bowl, and the flame it holds.
    m_batcher.push(quad(m_page_tile, Vec2{s.x - 0.85f, s.y - 0.95f}, Vec2{1.7f, 0.75f},
                        mul(kTintStoneCool, lit) * 1.2f, 1.0f, kDepthShrine + 0.4f, 1.0f,
                        2.0f));
    m_batcher.push(solid(Vec2{s.x - 0.52f, s.y - 1.85f}, Vec2{1.04f, 0.95f}, kTintFlame,
                         0.95f, kDepthShrine + 0.5f));
    m_batcher.push(solid(Vec2{s.x - 0.28f, s.y - 2.45f}, Vec2{0.56f, 0.65f},
                         Vec3{1.0f, 0.96f, 0.80f}, 0.95f, kDepthShrine + 0.6f));

    // A halo so the shrine is a beacon from the valley floor.
    const f32 pulse = 1.0f + 0.05f * std::sin(m_time * 2.1f);
    m_batcher.push(solid(Vec2{s.x - 3.4f * pulse, s.y - 4.6f * pulse},
                         Vec2{6.8f * pulse, 6.8f * pulse}, kTintFlame, 0.10f, kDepthGlow));
}

void Game::draw_contents() {
    const Rect view = m_camera.visible_bounds();

    // Spikes.
    for (const Spike& spike : m_world.spikes()) {
        if (spike.y < view.top() - 2.0f || spike.y > view.bottom() + 2.0f) continue;
        const int teeth = std::max(1, static_cast<int>(spike.w / 0.26f));
        const f32 tw = spike.w / static_cast<f32>(teeth);
        for (int i = 0; i < teeth; ++i) {
            const f32 x = spike.x - spike.w * 0.5f + tw * i;
            m_batcher.push(solid(Vec2{x + tw * 0.12f, spike.y - 0.72f},
                                 Vec2{tw * 0.76f, 0.72f}, kTintSpike * 0.55f, 0.95f,
                                 kDepthHazard));
            m_batcher.push(solid(Vec2{x + tw * 0.12f, spike.y - 0.72f},
                                 Vec2{tw * 0.30f, 0.72f}, kTintSpike, 0.95f,
                                 kDepthHazard + 0.1f));
        }
    }

    // Flames: a teardrop of light with a slow bob and a halo.
    for (const Flame& flame : m_world.flames()) {
        if (flame.taken) continue;
        if (flame.pos.y < view.top() - 2.0f || flame.pos.y > view.bottom() + 2.0f) continue;
        const f32 bob = std::sin(m_time * 2.4f + flame.phase) * 0.14f;
        const Vec2 p = flame.pos + Vec2{0.0f, bob};
        const f32 pulse = 1.0f + 0.10f * std::sin(m_time * 3.4f + flame.phase);
        m_batcher.push(solid(Vec2{p.x - 0.16f, p.y - 0.34f}, Vec2{0.32f, 0.46f},
                             kTintFlame, 0.95f, kDepthPickup));
        m_batcher.push(solid(Vec2{p.x - 0.08f, p.y - 0.52f}, Vec2{0.16f, 0.22f},
                             Vec3{1.0f, 0.97f, 0.86f}, 0.95f, kDepthPickup + 0.1f));
        m_batcher.push(solid(Vec2{p.x - 0.9f * pulse, p.y - 1.1f * pulse},
                             Vec2{1.8f * pulse, 1.8f * pulse}, kTintFlame, 0.16f, kDepthGlow));
    }

    // Bats.
    for (const Bat& bat : m_world.bats()) {
        if (!bat.alive) continue;
        if (bat.pos.y < view.top() - 2.0f || bat.pos.y > view.bottom() + 2.0f) continue;
        const f32 flap = std::sin(bat.wing);
        m_batcher.push(solid(Vec2{bat.pos.x - 0.16f, bat.pos.y - 0.12f}, Vec2{0.32f, 0.30f},
                             kTintBat * 1.5f, 1.0f, kDepthEnemy));
        // Wings as two rotated slabs.
        SpriteDraw wing = solid(Vec2{bat.pos.x - 0.52f, bat.pos.y - 0.08f},
                                Vec2{0.44f, 0.13f}, kTintBat, 1.0f, kDepthEnemy - 0.1f);
        wing.rotation_deg = flap * 26.0f;
        m_batcher.push(wing);
        SpriteDraw wing_r = solid(Vec2{bat.pos.x + 0.08f, bat.pos.y - 0.08f},
                                  Vec2{0.44f, 0.13f}, kTintBat, 1.0f, kDepthEnemy - 0.1f);
        wing_r.rotation_deg = -flap * 26.0f;
        m_batcher.push(wing_r);
    }

    // Shrine lanterns.
    for (const Lantern& lantern : m_world.lanterns()) {
        if (lantern.pos.y < view.top() - 3.0f || lantern.pos.y > view.bottom() + 3.0f) continue;
        // Post.
        m_batcher.push(solid(Vec2{lantern.pos.x - 0.09f, lantern.pos.y}, Vec2{0.18f, 1.15f},
                             kTintStoneCool * 0.8f, 1.0f, kDepthPickup - 0.1f));
        // Cage.
        m_batcher.push(solid(Vec2{lantern.pos.x - 0.28f, lantern.pos.y - 0.78f},
                             Vec2{0.56f, 0.80f}, kTintStoneCool, 0.95f, kDepthPickup));
        const f32 pulse = 0.85f + 0.15f * std::sin(m_time * 5.0f + lantern.pos.x);
        const Vec3 core = lantern.lit ? kTintFlame * (1.15f * pulse) : kTintStoneCool * 0.7f;
        m_batcher.push(solid(Vec2{lantern.pos.x - 0.19f, lantern.pos.y - 0.68f},
                             Vec2{0.38f, 0.60f}, core, 1.0f, kDepthPickup + 0.1f));
        if (lantern.lit) {
            m_batcher.push(solid(Vec2{lantern.pos.x - 1.5f, lantern.pos.y - 1.9f},
                                 Vec2{3.0f, 3.0f}, kTintFlame, 0.14f * pulse, kDepthGlow));
        }
    }
}

void Game::draw_player() {
    const Body2D* body = m_world.physics().body(m_player);
    if (!body) return;
    const Vec2 p = body->position;

    // A climbing figure, built from seven tinted rectangles. It reads as a
    // silhouette with a lantern, which is all the game needs, and it inherits the
    // cliff's lighting so it never floats free of the scene.
    //
    // The lit value is floored hard here. The figure is one unit tall in a
    // thirty-unit-wide frame and it is the one thing the player must always be
    // able to find; letting the lantern's falloff dim it to a smudge at the edge
    // of the light made the character harder to see than the rock behind it.
    const Vec3 lit = floor_to(m_lighting.sample(p), 0.62f);
    const f32 stretch = std::clamp(body->velocity.y * -0.006f, -0.10f, 0.14f);

    // A soft dark under-shadow, so the figure separates from any cliff behind it.
    m_batcher.push(solid(Vec2{p.x - 0.32f, p.y - 0.68f + stretch},
                         Vec2{0.64f, 1.28f - stretch}, Vec3{0.0f, 0.0f, 0.0f}, 0.40f,
                         kDepthPlayer - 0.1f));

    // Cloak.
    m_batcher.push(solid(Vec2{p.x - 0.24f, p.y - 0.34f + stretch},
                         Vec2{0.48f, 0.86f - stretch}, mul(kTintCloak, lit) * 2.1f, 1.0f,
                         kDepthPlayer));
    // Head and hood.
    m_batcher.push(solid(Vec2{p.x - 0.17f, p.y - 0.60f + stretch}, Vec2{0.34f, 0.30f},
                         mul(kTintCloak, lit) * 2.7f, 1.0f, kDepthPlayer + 0.1f));
    m_batcher.push(solid(Vec2{p.x - m_facing * 0.10f - 0.09f, p.y - 0.52f + stretch},
                         Vec2{0.18f, 0.16f}, mul(kTintSkin, lit) * 1.4f, 1.0f,
                         kDepthPlayer + 0.2f));
    // Legs.
    const f32 stride = m_on_ground ? std::sin(m_time * 13.0f) * 0.07f * std::fabs(body->velocity.x)
                                   : 0.0f;
    m_batcher.push(solid(Vec2{p.x - 0.20f, p.y + 0.42f}, Vec2{0.16f, 0.34f + stride},
                         mul(kTintCloak, lit) * 1.6f, 1.0f, kDepthPlayer));
    m_batcher.push(solid(Vec2{p.x + 0.04f, p.y + 0.42f}, Vec2{0.16f, 0.34f - stride},
                         mul(kTintCloak, lit) * 1.6f, 1.0f, kDepthPlayer));
    // The lantern, held out on the facing side, plus its halo.
    const f32 lx = p.x + m_facing * 0.34f;
    const f32 ly = p.y + 0.18f;
    m_batcher.push(solid(Vec2{lx - 0.10f, ly - 0.14f}, Vec2{0.20f, 0.24f}, kTintFlame, 1.0f,
                         kDepthPlayer + 0.3f));
    m_batcher.push(solid(Vec2{lx - 1.3f, ly - 1.5f}, Vec2{2.6f, 2.6f}, kTintFlame, 0.18f,
                         kDepthGlow));
}

void Game::draw_motes() {
    const Rect view = m_camera.visible_bounds();
    for (const Mote& mote : m_world.motes()) {
        if (mote.pos.x < view.left() - 1.0f || mote.pos.x > view.right() + 1.0f) continue;
        if (mote.pos.y < view.top() - 1.0f || mote.pos.y > view.bottom() + 1.0f) continue;
        const f32 fade = std::clamp(mote.life / mote.max_life, 0.0f, 1.0f);
        // Fade in as well as out, so a wrapped mote does not pop.
        const f32 alpha = std::min(fade, 1.0f - fade) * 2.0f;
        m_batcher.push(solid(Vec2{mote.pos.x, mote.pos.y}, Vec2{mote.size, mote.size},
                             Vec3{1.0f, 1.0f, 1.0f}, alpha, kDepthMote));
        (void)mote.color;
    }
}

void Game::draw_glows() {
    // Vignette: four bars, not a shader. It costs nothing, needs no post-process
    // chain, and frames the climb the way a printed plate would.
    //
    // Laid out in pixels through hud_solid. Writing these as fixed world-space
    // numbers looked fine until the numbers stopped matching the framing, at
    // which point the bars silently grew past the viewport and laid a 45% black
    // wash over the entire frame.
    const Vec3 edge{0.02f, 0.02f, 0.05f};
    const f32 t = 64.0f;  // Bar thickness, in pixels.
    const f32 alpha = 0.55f;
    const f32 w = static_cast<f32>(kWindowW);
    const f32 h = static_cast<f32>(kWindowH);

    m_batcher.push(hud_solid(Vec2{0.0f, 0.0f}, Vec2{w, t}, edge, alpha, kDepthVignette));
    m_batcher.push(hud_solid(Vec2{0.0f, h - t}, Vec2{w, t}, edge, alpha, kDepthVignette));
    m_batcher.push(hud_solid(Vec2{0.0f, 0.0f}, Vec2{t, h}, edge, alpha, kDepthVignette));
    m_batcher.push(hud_solid(Vec2{w - t, 0.0f}, Vec2{t, h}, edge, alpha, kDepthVignette));
}

/// Screen pixel -> world, the exact inverse of Camera2D's forward transform.
///
/// This is written out rather than delegated to Camera2D::screen_to_world
/// because that helper does not round-trip in Y: a marker drawn at pixel row 30
/// came out at row ~690, mirrored about the viewport centre. Every HUD element
/// went through it, so the whole interface â€” labels, gauge, story line â€” was
/// upside-down relative to the world, which was drawn correctly all along and
/// made the bug look like a font problem.
///
/// Camera2D documents world->screen as `screen = (world - origin) * ppu + centre`,
/// so the inverse is `world = origin + (screen - centre) / ppu`. Writing that
/// down makes the round trip identity by construction, and because the HUD's
/// quads are converted with this same function it stays pinned to the viewport
/// whatever the camera does.
Vec2 Game::px_to_world(Vec2 px) const {
    const f32 s = m_camera.pixels_per_world_unit();
    const Vec2 centre{m_camera.viewport_w * 0.5f, m_camera.viewport_h * 0.5f};
    return m_camera.position + (px - centre) / s;
}

void Game::text(SpriteBatcher& b, const std::string& visual, Vec2 px, f32 size_px,
                Vec3 tint, f32 alpha, bool centre) {
    // The HUD is laid out in pixels but drawn through the same batcher and camera
    // as the world, so both the origin and the size are converted to world units.
    const f32 s = m_camera.pixels_per_world_unit();
    const f32 world_size = size_px / s;
    // draw() places text by its baseline; shift by the ascender so the argument
    // means "top of the line", the way the rest of the HUD reads.
    const f32 baseline_px = px.y + m_atlas.ascent_at(size_px);
    const Vec2 world = px_to_world(Vec2{px.x, baseline_px});
    m_atlas.draw(b, visual, world, world_size, scene2d::pack_color_vec(tint, alpha),
                 kDepthHud, alpha, centre);
}

/// A solid rectangle laid out in pixels, pushed as a world-space sprite.
SpriteDraw Game::hud_solid(Vec2 px, Vec2 px_size, Vec3 tint, f32 alpha, f32 depth) {
    const f32 s = m_camera.pixels_per_world_unit();
    return solid(px_to_world(px), px_size / s, tint, alpha, depth);
}
void Game::draw_hud() {
    const Body2D* body = m_world.physics().body(m_player);
    const Vec2 pp = body ? body->position : m_world.spawn_pos();
    const f32 progress = m_world.progress(pp.y);
    const f32 w = static_cast<f32>(kWindowW);
    const f32 h = static_cast<f32>(kWindowH);

    if (m_screen == Screen::Title) {
        // Dim the cliff behind the title so the text has contrast to sit on.
   m_batcher.push(hud_solid(Vec2{0.0f, 0.0f}, Vec2{w, h},
     Vec3{0.02f, 0.02f, 0.06f}, 0.62f, kDepthVignette));


        // Title, centred, in three sizes. `pulse` breathes so the prompt reads as
        // "press now" without needing a separate animation state.
        const f32 pulse = 0.62f + 0.38f * std::sin(m_time * 2.6f);
        text(m_batcher, m_title_visual, Vec2{w * 0.5f, 200.0f}, 92.0f, kTintFlame, 1.0f,
             true);
        text(m_batcher, m_subtitle_visual, Vec2{w * 0.5f, 322.0f}, 33.0f, kTintText, 0.88f,
        true);
        text(m_batcher, m_start_visual, Vec2{w * 0.5f, 420.0f}, 30.0f, kTintText, pulse, true);
        text(m_batcher, m_controls_visual, Vec2{w * 0.5f, 520.0f}, 20.0f, kTintText, 0.58f,
     true);
        return;
    }

    if (m_screen == Screen::Ending) {
        m_batcher.push(hud_solid(Vec2{0.0f, 0.0f}, Vec2{w, h}, Vec3{0.02f, 0.02f, 0.06f},
       0.66f, kDepthVignette));
      text(m_batcher, m_ending_title_visual, Vec2{w * 0.5f, 190.0f}, 72.0f, kTintFlame,
             1.0f, true);
        text(m_batcher, m_ending_text_visual, Vec2{w * 0.5f, 306.0f}, 29.0f, kTintText, 0.95f,
             true);

    // Closing stats, so the ending reports the climb rather than just ending.
        char line[128];
        std::snprintf(line, sizeof(line), "%s  %d / %d", kFlameLabel, m_flames,
   m_flames_total);
        text(m_batcher, m_atlas.prepare(line), Vec2{w * 0.5f, 396.0f}, 26.0f, kTintText,
             0.85f, true);
std::snprintf(line, sizeof(line), "%s  %d", kFallLabel, m_falls);
        text(m_batcher, m_atlas.prepare(line), Vec2{w * 0.5f, 438.0f}, 26.0f, kTintText,
   0.85f, true);

        const f32 pulse = 0.62f + 0.38f * std::sin(m_time * 2.6f);
        text(m_batcher, m_restart_visual, Vec2{w * 0.5f, 528.0f}, 26.0f, kTintText, pulse,
             true);
        return;
    }

    // --- playing ---

    // HUD lines re-shape only when their text actually changes; shaping allocates
    // and walking the string, and doing that every frame for four short labels is
    // waste that buys nothing.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s  %d / %d", kFlameLabel, m_flames, m_flames_total);
    if (m_flame_line_logical != buf) {
  m_flame_line_logical = buf;
        m_flame_line_visual = m_atlas.prepare(buf);
    }
    std::snprintf(buf, sizeof(buf), "%s  %d", kFallLabel, m_falls);
    if (m_fall_line_logical != buf) {
        m_fall_line_logical = buf;
        m_fall_line_visual = m_atlas.prepare(buf);
    }
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(progress * 100.0f)));
    if (m_alt_line_logical != buf) {
  m_alt_line_logical = buf;
   m_alt_line_visual = m_atlas.prepare(buf);
    }

    text(m_batcher, m_flame_line_visual, Vec2{44.0f, 34.0f}, 28.0f, kTintFlame, 0.95f);
    text(m_batcher, m_fall_line_visual, Vec2{44.0f, 74.0f}, 24.0f, kTintText, 0.72f);

    // Altitude gauge down the right edge: the whole climb in one glance.
    const f32 gauge_x = w - 62.0f;
    const f32 gauge_top = 128.0f;
    const f32 gauge_h = h - 260.0f;
    m_batcher.push(hud_solid(Vec2{gauge_x, gauge_top}, Vec2{7.0f, gauge_h}, kTintWhite, 0.13f,
  kDepthHud));

    // Flames collected along the route, as tick marks.
    for (const Flame& flame : m_world.flames()) {
  const f32 y = gauge_top + (1.0f - m_world.progress(flame.pos.y)) * gauge_h;
        m_batcher.push(hud_solid(Vec2{gauge_x - 5.0f, y}, Vec2{17.0f, 2.0f}, kTintFlame,
  flame.taken ? 0.95f : 0.26f, kDepthHud + 0.1f));
    }
    // Shrine lanterns.
    for (const Lantern& lantern : m_world.lanterns()) {
const f32 y = gauge_top + (1.0f - m_world.progress(lantern.pos.y)) * gauge_h;
     m_batcher.push(hud_solid(Vec2{gauge_x - 9.0f, y - 3.0f}, Vec2{25.0f, 6.0f},
   kTintText, lantern.lit ? 0.85f : 0.24f, kDepthHud + 0.2f));
    }
    // The climber.
    const f32 mark_y = gauge_top + (1.0f - progress) * gauge_h;
    m_batcher.push(hud_solid(Vec2{gauge_x - 12.0f, mark_y - 6.0f}, Vec2{31.0f, 12.0f},
    kTintText, 0.95f, kDepthHud + 0.3f));
    text(m_batcher, m_alt_line_visual, Vec2{gauge_x - 14.0f, mark_y - 42.0f}, 23.0f, kTintText,
         0.9f, false);

    // The story line, fading in and out over its beat.
    if (m_beat_timer > 0.0f && m_beat > 0 && m_beat <= kStoryCount) {
        const f32 age = 6.0f - m_beat_timer;
        const f32 alpha = std::min(std::min(age * 2.5f, m_beat_timer * 1.2f), 1.0f) * 0.95f;
        // A bar behind it, so text over a busy cliff is still readable.
        m_batcher.push(hud_solid(Vec2{0.0f, h - 134.0f}, Vec2{w, 76.0f},
    Vec3{0.02f, 0.02f, 0.06f}, 0.66f, kDepthHud - 1.0f));
        text(m_batcher, m_beat_visual[static_cast<usize>(m_beat - 1)],
             Vec2{w * 0.5f, h - 112.0f}, 28.0f, kTintText, alpha, true);
    }

    // The controls reminder, until the player has clearly got the idea.
    if (m_play_time < 10.0f) {
        const f32 alpha = std::min(1.0f, (10.0f - m_play_time) * 0.8f) * 0.55f;
        text(m_batcher, m_controls_visual, Vec2{w * 0.5f, h - 48.0f}, 20.0f, kTintText, alpha,
   true);
    }
}
void Game::build_frame() {
    m_batcher.begin();

    draw_sky();
    draw_parallax_layers();
    draw_cliff();
    draw_ledges();
    draw_shrine();
    draw_contents();
    draw_player();
    draw_motes();
    draw_glows();
    {
    }
    draw_hud();

    m_batcher.end(m_camera);
}

i32 Game::run(const GameConfig& config) {
    WindowDesc wd{};
    wd.width = kWindowW;
    wd.height = kWindowH;
    wd.title = "SANAD — حكاية الجرف (The Cliff's Tale)";
    wd.vsync = true;
    if (!m_window.create(wd)) {
        NF_LOG_FATAL(LogCategory::Platform, "CliffStory: failed to create the window");
        return -1;
    }
    if (m_window.width() == 0 || m_window.height() == 0) {
        NF_LOG_FATAL(LogCategory::Platform, "CliffStory: zero-sized window");
        m_window.destroy();
        return -1;
    }

    m_device = rhi::create_device();
    rhi::DeviceDesc dd{};
    dd.window_handle = m_window.native_handle();
    dd.enable_validation = config.validation;
    if (!m_device || !m_device->init(dd)) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to initialise the graphics device");
        m_window.destroy();
        return -1;
    }
    NF_LOG_INFO(LogCategory::RHI, "CliffStory: device '{}'", m_device->backend_name());

    rhi::SwapchainDesc sd{};
    sd.width = m_window.width();
    sd.height = m_window.height();
    sd.format = rhi::Format::B8G8R8A8_UNorm;
    sd.present = rhi::PresentMode::FIFO;
    sd.image_count = 2;
    m_swapchain = m_device->create_swapchain(sd);
    if (!m_swapchain) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create the swapchain");
        m_device->shutdown();
        m_window.destroy();
        return -1;
    }

    if (!m_renderer.create(*m_device, *m_swapchain, m_window.width(), m_window.height())) {
        m_swapchain.reset();
        m_device->shutdown();
        m_window.destroy();
        return -1;
    }
    if (!load_assets()) {
        m_renderer.destroy();
        m_swapchain.reset();
        m_device->shutdown();
        m_window.destroy();
        return -1;
    }

    m_cmd = m_device->create_command_buffer();
    m_image_available = m_device->create_semaphore();
    m_frame_fence = m_device->create_fence(true);
    m_render_finished.resize(m_swapchain->image_count());
    for (auto& s : m_render_finished) s = m_device->create_semaphore();
    if (!m_cmd || !m_image_available || !m_frame_fence) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create sync primitives");
        return -1;
    }

    InputSystem::instance().init();
    reset();
    if (config.skip_title) {
        m_screen = Screen::Play;
        m_play_time = 12.0f;  // past the controls reminder
    }
    if (config.warp >= 0.0f) {
        // Drop the climber onto the nearest ledge at that height rather than into
        // whatever air happens to be there, so --warp lands somewhere playable.
        const f32 target = std::clamp(config.warp, 0.0f, 1.0f);
        const f32 want_y = kCliffBottomY - target * kCliffHeight;
        const Ledge* best = nullptr;
        f32 best_d = 1e9f;
        for (const Ledge& ledge : m_world.ledges()) {
            const f32 d = std::fabs(ledge.y - want_y);
            if (d < best_d) {
                best_d = d;
                best = &ledge;
            }
        }
        m_respawn = best ? Vec2{best->x + best->w * 0.5f, best->y - 1.0f}
                         : Vec2{-4.0f, want_y};
        respawn_player();
        m_camera.position = m_respawn;
        m_screen = Screen::Play;
        m_play_time = 12.0f;
    }

    // Only pay for the offscreen target when a screenshot was actually asked for.
    if (!config.screenshot.empty()) {
        m_renderer.ensure_capture_target(m_window.width(), m_window.height());
    }

    const std::array<rhi::ClearValue, 1> clear{rhi::ClearValue{0.02f, 0.02f, 0.05f, 1.0f}};

    Clock clock;
    Timer timer;
    bool running = true;

    while (!m_window.should_close() && running) {
        InputSystem::instance().begin_frame();
        m_window.poll_events();
        timer.tick();

        // A long frame must not teleport the player through the level, and the
        // physics step must stay fixed, so dt is clamped and accumulated.
        const f32 dt = std::min(timer.delta(), 0.10f);
        m_time += dt;

        const auto& input = InputSystem::instance();
        if (input.is_key_pressed(KeyCode::Escape)) running = false;
        if (input.is_key_pressed(KeyCode::R)) reset();

        if (m_screen == Screen::Title) {
            if (input.is_key_pressed(KeyCode::Space) || input.is_key_pressed(KeyCode::Enter)) {
                m_screen = Screen::Play;
                m_play_time = 0.0f;
            }
        } else if (m_screen == Screen::Play) {
            m_play_time += dt;
            m_accumulator += dt;
            int steps = 0;
            while (m_accumulator >= kFixedStep && steps < kMaxSubSteps) {
                m_accumulator -= kFixedStep;
                fixed_step(kFixedStep);
                ++steps;
            }
            if (steps >= kMaxSubSteps) m_accumulator = 0.0f;

            const Body2D* body = m_world.physics().body(m_player);
            if (body && body->position.y <= m_world.summit_y() + 4.0f) {
                m_screen = Screen::Ending;
            }
        }

        follow_camera(dt);
        rebuild_lighting();
        build_frame();

        // --- submit ---
        m_frame_fence->wait();
        m_frame_fence->reset();

      // True only on the last frame of a --screenshot run, so the extra pass is
        // paid once rather than every frame.
        const bool want_capture = !config.screenshot.empty() && config.max_frames > 0 &&
                                  m_frames + 1 >= config.max_frames;

        const u32 image_index = m_swapchain->acquire_next_image(*m_image_available);
        if (image_index == u32_max) {
            NF_LOG_WARN(LogCategory::RHI, "CliffStory: swapchain out of date");
            break;
        }

        m_cmd->reset();
        m_cmd->begin();
        m_cmd->begin_render_pass(*m_renderer.render_pass(), *m_renderer.framebuffer(image_index),
                                 std::span<const rhi::ClearValue>(clear));
        m_cmd->set_viewport(0, 0, m_window.width(), m_window.height());
        m_cmd->set_scissor(0, 0, m_window.width(), m_window.height());
        m_renderer.draw(m_batcher, *m_cmd);
  m_cmd->end_render_pass();

    // The capture frame draws the same batch a second time into the offscreen
        // target. The swapchain image is COLOR_ATTACHMENT-only and cannot be a
        // copy_texture_to_buffer source, so this is the only route to a
  // screenshot that is evidence of the render rather than a photograph of
        // whatever was on the user's desktop.
        if (want_capture && m_renderer.capture_target().ready) {
   const auto& target = m_renderer.capture_target();
            m_cmd->begin_render_pass(*target.pass, *target.framebuffer,
   std::span<const rhi::ClearValue>(clear));
  m_cmd->set_viewport(0, 0, m_window.width(), m_window.height());
    m_cmd->set_scissor(0, 0, m_window.width(), m_window.height());
    m_renderer.draw(m_batcher, *m_cmd);
         m_cmd->end_render_pass();
        }

        m_cmd->end();

        const std::array<const rhi::Semaphore*, 1> wait_sems{m_image_available.get()};
        const std::array<rhi::PipelineStage, 1> wait_stages{
            rhi::PipelineStage::ColorAttachmentOutput};
        const std::array<const rhi::Semaphore*, 1> signal_sems{
            m_render_finished[image_index].get()};
        rhi::SubmitInfo si{};
        si.wait_semaphores = std::span<const rhi::Semaphore* const>(wait_sems);
        si.wait_stages = std::span<const rhi::PipelineStage>(wait_stages);
        si.signal_semaphores = std::span<const rhi::Semaphore* const>(signal_sems);
        si.signal_fence = m_frame_fence.get();
        m_device->submit(*m_cmd, si);

        const std::array<const rhi::Semaphore*, 1> present_sems{
            m_render_finished[image_index].get()};
        m_swapchain->present(image_index, std::span<const rhi::Semaphore* const>(present_sems));

        ++m_frames;

        // The batcher's payoff, made visible: hundreds of sprites, a handful of
        // draw calls. Logged on a frame count rather than a timer, so the line
        // appears at the same point in every run.
        if (m_frames % 60 == 0) {
            const Body2D* body = m_world.physics().body(m_player);
            NF_LOG_INFO(LogCategory::RHI,
                        "CliffStory: {} sprites -> {} draws ({} tris), {} pages, "
                        "altitude {}%, {} flames",
                        m_renderer.last_sprite_count(), m_renderer.last_draw_calls(),
                        m_renderer.last_triangle_count(), m_renderer.page_count(),
                        body ? static_cast<int>(m_world.progress(body->position.y) * 100.0f) : 0,
                        m_flames);
        }

        if (config.max_frames > 0 && m_frames >= config.max_frames) {
            NF_LOG_INFO(LogCategory::RHI, "CliffStory: reached the {} frame budget",
                        config.max_frames);
            break;
        }
    }

    // A screenshot has to happen before the swapchain is torn down, and after a
    // wait_idle so the image being read is a completed one.
    if (!config.screenshot.empty()) {
        m_device->wait_idle();
        std::vector<u8> pixels;
        if (m_renderer.read_back_capture(pixels)) {
            const bool ok = write_png(config.screenshot, m_swapchain->width(),
                                      m_swapchain->height(), pixels);
            NF_LOG_INFO(LogCategory::RHI, "CliffStory: wrote {} ({}x{})", config.screenshot,
                        m_swapchain->width(), m_swapchain->height(), ok ? "ok" : "FAILED");
        } else {
            NF_LOG_ERROR(LogCategory::RHI, "CliffStory: screenshot readback failed");
        }
    }

    const u32 frames = m_frames;

    // Drain the queue before anything the last submission referenced is
    // destroyed. Vulkan is explicit that a semaphore, a fence, a command buffer
    // and a swapchain may not be torn down while the queue is still using them,
    // and every one of those errors above is exactly that.
    m_device->wait_idle();

    InputSystem::instance().shutdown();

    m_render_finished.clear();
    m_frame_fence.reset();
    m_image_available.reset();
    m_cmd.reset();

    // Every Rendering object must die before the device shuts down.
    m_renderer.destroy();
    m_swapchain.reset();
    m_device->shutdown();
    m_device.reset();
    m_window.destroy();

    NF_LOG_INFO(LogCategory::Core, "CliffStory: exited cleanly after {} frames", frames);
    return 0;
}

GameConfig parse_args(int argc, char** argv) {
    GameConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if ((arg == "--frames" || arg == "--frames=") && i + 1 < argc) {
            config.max_frames = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--validation") {
            config.validation = true;
  } else if (arg == "--skip-title") {
      config.skip_title = true;
        } else if (arg == "--warp" && i + 1 < argc) {
      config.warp = static_cast<f32>(std::atof(argv[++i]));
        } else if (arg == "--screenshot" && i + 1 < argc) {
            config.screenshot = argv[++i];
        } else if (arg.rfind("--screenshot=", 0) == 0) {
            config.screenshot = std::string(arg.substr(13));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "SANAD CliffStory — حكاية الجرف\n"
                         "  --frames N        Render N frames then exit (0 = until closed)\n"
         "  --screenshot P   Write the final frame to a PNG, then exit\n"
   "  --skip-title     Begin the climb instead of the title card\n"
      "  --warp P         Start at progress P up the cliff (0..1)\n"
"  --validation      Request Vulkan validation layers\n"
                         "\nControls: arrows or A/D to move, Space to jump, R to restart.\n";
            std::exit(0);
        }
    }
    return config;
}

} // namespace

} // namespace nf::cliff

int main(int argc, char** argv) {
    using namespace nf;
    Logger& logger = Logger::instance();
    logger.add_sink(Logger::make_console_sink());
    logger.set_min_level(LogLevel::Info);

    NF_LOG_INFO(LogCategory::Core, "=== SANAD — CliffStory (2D, NFScene2D) ===");
    platform_init();
    const cliff::GameConfig config = cliff::parse_args(argc, argv);
    cliff::Game game;
    const i32 result = game.run(config);
    platform_shutdown();
    NF_LOG_INFO(LogCategory::Core, "=== CliffStory finished (code {}) ===", result);
    return result;
}
