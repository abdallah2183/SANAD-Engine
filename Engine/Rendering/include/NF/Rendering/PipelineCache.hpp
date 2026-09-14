#pragma once

// NF/Rendering/PipelineCache.hpp — deduplicates rhi::Pipeline creation
//
// Creating a pipeline is expensive (shader compilation, driver work). The cache
// ensures that two Materials with identical PipelineDesc share one rhi::Pipeline
// instead of creating duplicates.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace nf::rendering {

class PipelineCache {
public:
    explicit PipelineCache(rhi::IGraphicsDevice& device);
    ~PipelineCache() = default;

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // Returns a cached pipeline or creates a new one. The returned pointer
    // remains valid until the cache is destroyed, clear() is called, or the
    // pipeline is invalidated via invalidate_shader().
    rhi::Pipeline* get_or_create(const rhi::PipelineDesc& desc);

    // Invalidates only pipelines that depend on the given shader.
    // Returns the number of pipelines removed.
    size_t invalidate_shader(const rhi::ShaderModule* shader);
    size_t invalidate_all();

    void clear();
    size_t size() const { return m_cache.size(); }

private:
    // Production-safe key: owns all data so it remains valid even if the
    // source PipelineDesc and its spans die (e.g. temporary vectors).
    struct Key {
        const rhi::ShaderModule* vs = nullptr;
        const rhi::ShaderModule* fs = nullptr;
        const rhi::RenderPass* render_pass = nullptr;
        const rhi::DescriptorSetLayout* layout = nullptr;
        u32 vertex_binding = 0;
        u32 vertex_stride = 0;
        std::vector<rhi::VertexAttrib> vertex_attribs; // owned copy
        rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
        rhi::RasterizerState rasterizer{};
        rhi::DepthState depth{};
        u32 push_constant_size = 0;
        rhi::ShaderStage push_constant_stages = rhi::ShaderStage::Vertex;

        bool operator==(const Key& other) const;
    };

    struct KeyHash {
        size_t operator()(const Key& k) const noexcept;
    };

    rhi::IGraphicsDevice& m_device;
    std::unordered_map<Key, std::unique_ptr<rhi::Pipeline>, KeyHash> m_cache;
};

} // namespace nf::rendering
