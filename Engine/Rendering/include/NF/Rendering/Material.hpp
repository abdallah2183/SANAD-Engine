#pragma once

// NF/Rendering/Material.hpp — Material + MaterialInstance + PipelineCache layer
//
// Material sits above RenderGraph and RHI, not directly on Vulkan:
//
//   Material → Shader → Parameter Layout → MaterialInstance → Descriptor Bindings → RenderGraph → RHI
//
// A Material describes a pipeline configuration (shaders, vertex layout, render
// pass, blending) and a descriptor layout. A MaterialInstance holds the per-
// draw resources (textures, buffers) and owns a DescriptorSet allocated from a
// per-frame allocator. This separation ensures that creating a new instance
// does NOT create a new Pipeline — many instances share one pipeline.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::rendering {

// Forward
class PipelineCache;

// Parameters that a PBR-like material exposes. This is the initial surface;
// more can be added without breaking the Material → Instance split.
struct MaterialParams {
    // BaseColor is either a texture or a constant color (fallback)
    // For now the textured quad uses only albedo_texture + sampler.
    std::string albedo_texture_name = "albedo_texture";
};

struct MaterialDesc {
    const rhi::ShaderModule* vs = nullptr;
    const rhi::ShaderModule* fs = nullptr;
    const rhi::RenderPass* render_pass = nullptr;
    const rhi::DescriptorSetLayout* descriptor_set_layout = nullptr; // optional
    rhi::VertexLayout vertex_layout{};
    rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
    rhi::RasterizerState rasterizer{};
    rhi::DepthState depth{};
    u32 push_constant_size = 0;
    rhi::ShaderStage push_constant_stages = rhi::ShaderStage::Vertex;
};

class Material {
public:
    Material(rhi::IGraphicsDevice& device, PipelineCache& cache, const MaterialDesc& desc);
    ~Material() = default;

    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;

    bool valid() const { return m_pipeline != nullptr; }
    rhi::Pipeline& pipeline() const { return *m_pipeline; }
    const rhi::DescriptorSetLayout* descriptor_layout() const { return m_descriptor_layout; }
    const MaterialDesc& desc() const { return m_desc; }
    rhi::IGraphicsDevice& device() const { return *m_device; }

private:
    rhi::IGraphicsDevice* m_device = nullptr;
    MaterialDesc m_desc;
    rhi::Pipeline* m_pipeline = nullptr; // owned by PipelineCache
    const rhi::DescriptorSetLayout* m_descriptor_layout = nullptr;
};

class MaterialInstance {
public:
    MaterialInstance(Material& material, rhi::DescriptorAllocator& allocator);
    ~MaterialInstance() = default;

    MaterialInstance(const MaterialInstance&) = delete;
    MaterialInstance& operator=(const MaterialInstance&) = delete;

    bool valid() const { return m_set != nullptr || m_material->descriptor_layout() == nullptr; }

    // Binding helpers — call before update()
    void set_texture(u32 binding, const rhi::TextureView& view, const rhi::Sampler& sampler);
    void set_uniform_buffer(u32 binding, const rhi::Buffer& buffer, usize offset = 0, usize range = 0);
    // Flushes the pending writes into the descriptor set (uses the material's device)
    void update();

    // Binds the pipeline + descriptor set(s) into the command buffer.
    // Must be called inside a render pass after the pipeline is bound.
    void bind(rhi::CommandBuffer& cmd, const rhi::DescriptorSetLayout& layout);

    // Convenience: binds pipeline + set in one call
    void bind_pipeline_and_descriptors(rhi::CommandBuffer& cmd);

    rhi::DescriptorSet* descriptor_set() const { return m_set.get(); }
    Material& material() const { return *m_material; }

private:
    Material* m_material = nullptr;
    rhi::DescriptorAllocator* m_allocator = nullptr;
    std::unique_ptr<rhi::DescriptorSet> m_set;
    std::vector<rhi::DescriptorWrite> m_pending_writes;
};

} // namespace nf::rendering
