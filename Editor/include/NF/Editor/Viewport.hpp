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

} // namespace nf::editor
