#pragma once

// NF/Editor/Viewport.hpp — offscreen viewport target lifecycle.
//
// The editor viewport never renders to the swapchain directly: each frame the
// shell renders the Runtime scene into this target via
// Runtime::render_offscreen() and presents the swapchain separately.
// ensure_target() recreates the texture on resize; the caller must wait_idle()
// first (documented at the call site), so no in-flight command buffer ever
// references the destroyed texture.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>

#include <cmath>
#include <cstdint>
#include <memory>

namespace nf::editor {

struct ViewportState {
    uint32_t width = 1280;
    uint32_t height = 720;
};

// Owns the offscreen color target plus its sampleable view/sampler (the pair
// ImGui::Image() resolves through UiRenderer::kViewportTextureId). Destroy
// before the device shuts down.
struct ViewportResources {
    std::unique_ptr<rhi::Texture> target;
    std::unique_ptr<rhi::TextureView> view;
    std::unique_ptr<rhi::Sampler> sampler;
    uint32_t width = 0;
    uint32_t height = 0;

    bool valid() const { return target != nullptr && width != 0 && height != 0; }
    void reset() {
        view.reset();
        sampler.reset();
        target.reset();
        width = 0;
        height = 0;
    }
};

// Creates/recreates the target when the size changed. Returns false on
// invalid size or device failure (target left empty).
bool ensure_viewport_target(rhi::IGraphicsDevice& device, const ViewportState& state,
                            ViewportResources& res);

// E1 — ground-grid line thinning.
//
// The grid is a scale reference, not a feature. Drawn at full density it is
// both too loud up close and pure moire in the far field, where the projected
// lines converge toward the horizon. So beyond `dense_radius` world units from
// the origin only every OTHER line is kept. The two axes (cell index 0, drawn
// in their own colours) always survive, so the origin never loses its cross.
//
// Pure and header-inline on purpose: the drawing loop in Panels.cpp is a thin
// wrapper over this, so the rule is testable without an ImDrawList, a camera or
// a GPU — which is the only way a headless suite can pin it.
inline bool grid_line_visible(int cell_index, float world_offset,
                              float dense_radius = 20.0f) {
    if (cell_index == 0) {
        return true; // an axis
    }
    if (world_offset <= dense_radius && world_offset >= -dense_radius) {
        return true; // near field stays dense
    }
    // `%` keeps the sign for negative indices but is 0 for every even index
    // either way, which is all this needs.
    return (cell_index % 2) == 0;
}

// The window of grid cells to draw around a point on the floor.
//
// The grid is INFINITE in principle and the draw loop can only afford a finite
// patch, so the patch has to follow whatever the user is looking at. It did not
// used to: the range was `-half..+half` off the WORLD ORIGIN, so framing a
// level anywhere else produced a viewport with no floor reference at all — the
// one thing the grid exists to provide.
//
// Lines stay anchored to the world lattice (`i * spacing` measured from 0), not
// to the centre: re-anchoring to the pivot would make the entire floor slide as
// the camera orbits, which is worse than a fixed lattice with a moving window.
// The centre only picks WHICH cells of that lattice get drawn.
//
// `kMaxCells` is a LINE BUDGET, not a coordinate clamp. Clamping the index
// instead would be worse than useless: a pivot 500 cells out would "fit" by
// drawing a patch 372 cells away from the user, which reads as a grid floating
// in empty space. Shrinking the half-extent keeps the window ON the pivot, just
// smaller, and never below the pivot's own cell.
struct GridCellRange {
    int lo = 0;
    int hi = -1; // lo > hi means "nothing to draw"
    bool empty() const { return lo > hi; }
    int count() const { return empty() ? 0 : (hi - lo + 1); }
};

inline GridCellRange grid_cell_range_around(float center, float spacing, int half_extent,
                                            int kMaxCells = 256) {
    if (!(spacing > 0.0f) || half_extent <= 0 || kMaxCells <= 0) {
        return GridCellRange{}; // empty: a non-positive step or budget draws nothing
    }
    // floor, not truncate: the cell a negative centre falls in must still be the
    // one the camera is over, and `(int)` truncates toward zero (so -17.3 would
    // pick cell -17 instead of -18).
    const int cell = static_cast<int>(std::floor(center / spacing));
    // Shrink to the budget, rounding so the WINDOW (not the half) fits: a
    // half-extent of 0 still yields the centre's own cell, so the budget can
    // never produce an empty patch for a valid pivot.
    const int half = std::max(0, std::min(half_extent, (kMaxCells - 1) / 2));
    return GridCellRange{cell - half, cell + half};
}

/// True when the cell at `index` carries a world axis line. The lattice is
/// anchored at 0, so that is exactly cell 0 — independent of where the view
/// pivot is. Distinct from grid_line_visible's "the origin axis is always
/// drawn", which is a VISIBILITY rule; this is a COLOUR rule, and conflating
/// the two makes the origin cross appear to move every time the user frames
/// something. `spacing` is taken only so a degenerate lattice has one honest
/// answer rather than a special case at the call site.
inline bool grid_cell_is_world_axis(int index, float spacing) {
    return spacing > 0.0f && index == 0;
}


// --- Game View (nf.ui preview in the editor viewport) -------------------------
//
// The editor Playing session renders the game into the same offscreen target
// as edit mode and draws `Runtime::game_ui()` over it, inside the ImGui::Image
// rect only. The layout viewport is the game content size (the offscreen
// target size), NOT the ImGui window size: layout, drawing and hit-testing
// must agree, or the UI would drift from the scene whenever the dock resizes.
// When the game's aspect differs from the display rect, the content rect is
// the aspect-fit letterbox inside it; input and drawing both use that rect,
// so the black bars never receive UI.
//
// All math is in ImGui logical coordinates (the shell is DPI aware; the
// offscreen target follows the displayed size 1:1, so one logical pixel is
// one game pixel here). Pure and header-inline so EditorTests pins it without
// a GPU — the same rule as grid_line_visible above.

struct GameContentRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    bool valid() const { return w >= 1.0f && h >= 1.0f; }
    bool contains(float sx, float sy) const {
        return valid() && sx >= x && sy >= y && sx < x + w && sy < y + h;
    }
};

// Aspect-fit of `game_w x game_h` into the display rect (dx, dy, dw, dh).
// Equal aspects return the display rect itself. Invalid game/display sizes
// return an empty rect (never a zero-size div).
inline GameContentRect game_content_fit(float dx, float dy, float dw, float dh,
                                        uint32_t game_w, uint32_t game_h) {
    if (!(dw >= 1.0f) || !(dh >= 1.0f) || game_w == 0 || game_h == 0) {
        return GameContentRect{};
    }
    const float game_aspect = static_cast<float>(game_w) / static_cast<float>(game_h);
    const float disp_aspect = dw / dh;
    GameContentRect out{};
    out.x = dx;
    out.y = dy;
    if (disp_aspect > game_aspect) {
        // Pillarbox: black bars left/right, full height used.
        out.h = dh;
        out.w = dh * game_aspect;
        out.x = dx + (dw - out.w) * 0.5f;
    } else {
        // Letterbox: black bars top/bottom, full width used.
        out.w = dw;
        out.h = dw / game_aspect;
        out.y = dy + (dh - out.h) * 0.5f;
    }
    return out;
}

// Screen (ImGui logical) -> game normalized (0..1, origin top-left).
// Returns false outside the content rect (bars, margins, or invalid rect),
// which the shell turns into an unfocused pointer — never a clamped edge
// press that would hit a button through the black bar.
inline bool game_screen_to_normal(float sx, float sy, const GameContentRect& rect,
                                  float& out_x, float& out_y) {
    if (!rect.contains(sx, sy)) {
        return false;
    }
    out_x = (sx - rect.x) / rect.w;
    out_y = (sy - rect.y) / rect.h;
    if (out_x < 0.0f) out_x = 0.0f;
    if (out_x > 1.0f) out_x = 1.0f;
    if (out_y < 0.0f) out_y = 0.0f;
    if (out_y > 1.0f) out_y = 1.0f;
    return true;
}

// Game normalized -> screen (for tests; the shell draws with offset math).
inline bool game_normal_to_screen(float nx, float ny, const GameContentRect& rect,
                                  float& out_x, float& out_y) {
    if (!rect.valid() || !(nx >= 0.0f && nx <= 1.0f && ny >= 0.0f && ny <= 1.0f)) {
        return false;
    }
    out_x = rect.x + nx * rect.w;
    out_y = rect.y + ny * rect.h;
    return true;
}

} // namespace nf::editor
