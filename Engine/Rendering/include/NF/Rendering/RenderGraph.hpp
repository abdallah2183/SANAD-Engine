#pragma once

// NF/Rendering/RenderGraph.hpp — RenderGraph layer above RHI
//
// The graph understands Pass / Resource / Read / Write / Dependency / Lifetime
// / Barrier / Execution order, and translates that into correct RHI command
// recording with layout transitions.
//
// Example:
//   ShadowPass writes ShadowMap
//          ↓
//   GeometryPass writes Color/Depth
//          ↓
//   LightingPass reads ShadowMap + GBuffer
//          ↓
//   PostProcess reads HDR writes Backbuffer
//
// The graph infers execution order and barriers; passes only declare what they
// read/write.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/ResourceState.hpp>

#include <functional>
#include <string>
#include <vector>

namespace nf::rendering {

using RGTextureHandle = u32;
static constexpr u32 kInvalidRGHandle = u32_max;

struct RGTextureDesc {
    std::string name;
    rhi::TextureDesc desc;
};

struct RGPassDesc {
    std::string name;
    std::vector<RGTextureHandle> reads;   // sampled / input attachments
    std::vector<RGTextureHandle> writes;  // color attachments / transfer dst
    // Optional: explicit color attachments for render passes (subset of writes)
    // If empty, writes are not treated as render targets.
    std::vector<RGTextureHandle> color_attachments;

    // Explicit resource roles. Declaring the role — instead of the graph
    // guessing it from the texture format — is what keeps the state machine
    // generic: DepthPrepass/GBuffer declare depth_attachment, compute-style
    // passes declare shader_writes, everything stays backend-neutral.
    RGTextureHandle depth_attachment = kInvalidRGHandle; // depth-stencil output
    std::vector<RGTextureHandle> shader_writes;          // storage-image writes

    // The work for this pass. The graph guarantees that all reads have been
    // transitioned to shader-readable before this is called, and that the
    // command buffer is in the recording state.
    std::function<void(rhi::CommandBuffer& cmd)> execute;
};

class RenderGraph {
public:
    explicit RenderGraph(rhi::IGraphicsDevice& device);
    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Creates an owned texture resource. The actual rhi::Texture is created on
    // compile() so the graph can decide lifetime.
    RGTextureHandle create_texture(const std::string& name, const rhi::TextureDesc& desc);

    // Imports an externally owned texture (e.g. swapchain image, offscreen target
    // created outside the graph). The graph never destroys it.
    RGTextureHandle import_texture(const std::string& name, rhi::Texture* external);

    // Adds a pass. Returns handle for dependency queries.
    u32 add_pass(const RGPassDesc& desc);

    // Builds the dependency graph and topologically sorts passes.
    // Must be called after all passes/resources are added and before execute().
    // Returns false if a cycle is detected.
    bool compile();

    // Executes the compiled graph into the given command buffer.
    // The command buffer must be in recording state (begin() already called).
    // The graph inserts barriers/layout transitions between passes.
    void execute(rhi::CommandBuffer& cmd);

    // Resets for the next frame: clears passes but keeps owned textures alive
    // until explicitly released. For a per-frame graph, call reset() each frame.
    void reset(bool keep_resources = true);

    // Accessors for passes that need the underlying RHI texture
    rhi::Texture* get_texture(RGTextureHandle handle);
    const rhi::Texture* get_texture(RGTextureHandle handle) const;

    // Debug
    size_t pass_count() const { return m_passes.size(); }
    std::string pass_name(u32 index) const {
        return index < m_passes.size() ? m_passes[index].name : "?";
    }
    size_t resource_count() const { return m_resources.size(); }
    bool is_compiled() const { return m_compiled; }
    const std::vector<u32>& execution_order() const { return m_execution_order; }

private:
    struct Resource {
        std::string name;
        rhi::TextureDesc desc;
        std::unique_ptr<rhi::Texture> owned_texture;
        rhi::Texture* external = nullptr; // non-owning if imported
        bool imported = false;
        RGResourceState current_state = RGResourceState::Undefined;
        rhi::Texture* texture() const {
            return imported ? external : owned_texture.get();
        }
    };

    struct Pass {
        std::string name;
        std::vector<RGTextureHandle> reads;
        std::vector<RGTextureHandle> writes;          // all touched-as-output resources
        std::vector<RGTextureHandle> color_attachments;
        RGTextureHandle depth_attachment = kInvalidRGHandle;
        std::vector<RGTextureHandle> shader_writes;
        std::function<void(rhi::CommandBuffer&)> execute;
    };

    rhi::IGraphicsDevice& m_device;
    std::vector<Resource> m_resources;
    std::vector<Pass> m_passes;
    std::vector<u32> m_execution_order;
    bool m_compiled = false;

    // Dependency graph built from read/write sets
    bool build_execution_order();
};

} // namespace nf::rendering
