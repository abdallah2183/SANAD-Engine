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

} // namespace nf::editor
