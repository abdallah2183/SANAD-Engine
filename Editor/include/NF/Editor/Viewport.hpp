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

} // namespace nf::editor
