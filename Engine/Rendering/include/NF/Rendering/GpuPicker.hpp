#pragma once

// NF/Rendering/GpuPicker.hpp — GPU-based entity picking.
//
// The CPU path (ray vs world AABB, see Culling.hpp / Gizmo pick_ray) is cheap
// and always available, but it answers a different question: what the ray
// intersects geometrically. It ignores occlusion, and it picks against
// bounding boxes rather than actual geometry, so clicking a hole in a mesh or
// an object hidden behind another still selects the wrong thing. GPU picking
// answers what the user actually sees.
//
// Mechanism: draw the scene's objects into an offscreen id image where each
// fragment writes its entity id, with depth testing so the nearest surface
// wins, then read back the single pixel under the cursor.
//
// This is deliberately self-contained rather than a fifth pass in Renderer3D's
// frame graph. Picking happens on demand (a click), not every frame, so an
// extra target and pass per frame would be pure waste; and keeping it out of
// the graph means a picker bug cannot perturb the rendered image. The cost is
// that it re-draws the scene on a pick, which is the right trade for an
// interactive editor.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/RenderWorld.hpp>

#include <filesystem>
#include <memory>
#include <span>

namespace nf::rendering {

struct PickHit {
    bool hit = false;
    /// RenderObject::id of the surface under the cursor (the entity id).
    /// Only meaningful when `hit` is true.
    u32 object_id = 0;
};

class GpuPicker {
public:
    GpuPicker() = default;
    ~GpuPicker();

    GpuPicker(const GpuPicker&) = delete;
    GpuPicker& operator=(const GpuPicker&) = delete;

    /// Loads the pick shaders and builds the pass/pipeline at the given size.
    /// Returns false (and leaves the picker not-ready) when shaders are missing
    /// or any RHI object fails to build — callers fall back to the CPU path.
    bool init(rhi::IGraphicsDevice& device, const std::filesystem::path& shader_dir,
              u32 width, u32 height);

    void shutdown();

    /// Recreates the id/depth targets. Caller must have waited for the GPU.
    bool resize(u32 width, u32 height);

    bool ready() const { return m_ready; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }

    /// Renders ids for `objects` into the id image and reads back the pixel at
    /// (x, y), in the same pixel space as the rendered image (origin top-left).
    ///
    /// Records and submits internally; `cmd` must be in the initial state (not
    /// already begun). Returns {false, 0} when the picker is not ready, the
    /// coordinate is out of range, nothing was drawn, or no object covered that
    /// pixel.
    PickHit pick(rhi::CommandBuffer& cmd, std::span<const RenderObject> objects,
                 const Camera& camera, MeshLibrary& meshes, u32 x, u32 y);

private:
    bool create_targets(u32 width, u32 height);
    void destroy_targets();

    rhi::IGraphicsDevice* m_device = nullptr;

    std::unique_ptr<rhi::RenderPass> m_pass;
    std::unique_ptr<rhi::Pipeline> m_pipeline;
    std::unique_ptr<rhi::ShaderModule> m_vs;
    std::unique_ptr<rhi::ShaderModule> m_fs;

    std::unique_ptr<rhi::Texture> m_color;
    std::unique_ptr<rhi::Texture> m_depth;
    std::unique_ptr<rhi::Framebuffer> m_fb;
    std::unique_ptr<rhi::Buffer> m_readback;

    u32 m_width = 0;
    u32 m_height = 0;
    bool m_ready = false;
};

/// Packs a 1-based id into the rgb bytes the pick shader writes.
/// Exposed for tests so the encoding and decoding can be checked without a GPU.
void pack_pick_id(u32 object_id, float out_rgb[3]);

/// Inverse of pack_pick_id. Returns 0 for the clear value / an unoccupied pixel.
u32 unpack_pick_id(u8 r, u8 g, u8 b, u8 a);

} // namespace nf::rendering
