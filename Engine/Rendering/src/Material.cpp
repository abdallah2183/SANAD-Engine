#include <NF/Rendering/Material.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Assert.hpp>

namespace nf::rendering {

Material::Material(rhi::IGraphicsDevice& device, PipelineCache& cache, const MaterialDesc& desc)
    : m_device(&device), m_desc(desc), m_descriptor_layout(desc.descriptor_set_layout) {

    rhi::PipelineDesc pdesc{};
    pdesc.vs = desc.vs;
    pdesc.fs = desc.fs;
    pdesc.render_pass = desc.render_pass;
    pdesc.descriptor_set_layout = desc.descriptor_set_layout;
    pdesc.vertex_layout = desc.vertex_layout;
    pdesc.topology = desc.topology;
    pdesc.rasterizer = desc.rasterizer;
    pdesc.depth = desc.depth;
    pdesc.push_constant_size = desc.push_constant_size;
    pdesc.push_constant_stages = desc.push_constant_stages;

    m_pipeline = cache.get_or_create(pdesc);
    if (!m_pipeline) {
        NF_LOG_ERROR(LogCategory::RHI, "Material: failed to get pipeline from cache");
    } else {
        NF_LOG_TRACE(LogCategory::RHI, "Material created (pipeline {})", static_cast<void*>(m_pipeline));
    }
}

// ---------------------------------------------------------------------------
// MaterialInstance
// ---------------------------------------------------------------------------

MaterialInstance::MaterialInstance(Material& material, rhi::DescriptorAllocator& allocator)
    : m_material(&material), m_allocator(&allocator) {
    NF_ASSERT(m_material, "MaterialInstance requires a material");
    NF_ASSERT(m_allocator, "MaterialInstance requires an allocator");

    const rhi::DescriptorSetLayout* layout = m_material->descriptor_layout();
    if (!layout) {
        // Material without descriptors (e.g. push-constant only) — no set needed
        m_set = nullptr;
        return;
    }

    m_set = m_allocator->allocate(*layout);
    if (!m_set) {
        NF_LOG_ERROR(LogCategory::RHI, "MaterialInstance: failed to allocate descriptor set");
    }
}

void MaterialInstance::set_texture(u32 binding, const rhi::TextureView& view, const rhi::Sampler& sampler) {
    NF_ASSERT(m_material, "set_texture on invalid instance");
    rhi::DescriptorWrite w{};
    w.binding = binding;
    w.type = rhi::DescriptorType::SampledImage;
    w.texture_view = &view;
    w.sampler = &sampler;
    m_pending_writes.push_back(w);
}

void MaterialInstance::set_uniform_buffer(u32 binding, const rhi::Buffer& buffer, usize offset, usize range) {
    rhi::DescriptorWrite w{};
    w.binding = binding;
    w.type = rhi::DescriptorType::UniformBuffer;
    w.buffer = &buffer;
    w.buffer_offset = offset;
    w.buffer_range = range;
    m_pending_writes.push_back(w);
}

void MaterialInstance::update() {
    if (!m_set || m_pending_writes.empty()) return;
    if (!m_material) return;
    rhi::IGraphicsDevice& device = m_material->device();
    device.update_descriptor_set(*m_set, std::span<const rhi::DescriptorWrite>(m_pending_writes));
    m_pending_writes.clear();
}

void MaterialInstance::bind(rhi::CommandBuffer& cmd, const rhi::DescriptorSetLayout& layout) {
    if (!m_set) return;
    // Flush pending writes if any - we need device. Since we don't have it, we will
    // assume the caller already called update via device.
    // For the hardening, we make bind just bind the set.
    const rhi::DescriptorSet* sets[] = { m_set.get() };
    cmd.bind_descriptor_sets(layout, std::span<const rhi::DescriptorSet* const>(sets, 1), 0);
}

void MaterialInstance::bind_pipeline_and_descriptors(rhi::CommandBuffer& cmd) {
    NF_ASSERT(m_material && m_material->valid(), "bind on invalid material");
    cmd.bind_pipeline(m_material->pipeline());
    if (m_set && m_material->descriptor_layout()) {
        const rhi::DescriptorSet* sets[] = { m_set.get() };
        cmd.bind_descriptor_sets(*m_material->descriptor_layout(),
                                 std::span<const rhi::DescriptorSet* const>(sets, 1), 0);
    }
}

} // namespace nf::rendering
