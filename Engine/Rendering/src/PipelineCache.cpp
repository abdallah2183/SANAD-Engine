#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Core/Logger.hpp>

namespace nf::rendering {

PipelineCache::PipelineCache(rhi::IGraphicsDevice& device) : m_device(device) {}

bool PipelineCache::Key::operator==(const Key& other) const {
    if (vs != other.vs || fs != other.fs || render_pass != other.render_pass ||
        layout != other.layout || topology != other.topology ||
        push_constant_size != other.push_constant_size ||
        push_constant_stages != other.push_constant_stages) return false;
    if (rasterizer.cull_mode != other.rasterizer.cull_mode ||
        rasterizer.front_face != other.rasterizer.front_face ||
        rasterizer.wireframe != other.rasterizer.wireframe) return false;
    if (depth.test_enabled != other.depth.test_enabled ||
        depth.write_enabled != other.depth.write_enabled ||
        depth.compare != other.depth.compare) return false;
    if (vertex_binding != other.vertex_binding ||
        vertex_stride != other.vertex_stride ||
        vertex_attribs.size() != other.vertex_attribs.size()) return false;
    for (size_t i = 0; i < vertex_attribs.size(); ++i) {
        const auto& a = vertex_attribs[i];
        const auto& b = other.vertex_attribs[i];
        if (a.location != b.location || a.offset != b.offset || a.format != b.format) return false;
    }
    return true;
}

size_t PipelineCache::KeyHash::operator()(const Key& k) const noexcept {
    size_t h = 0;
    auto combine = [&](size_t v){ h ^= v + 0x9e3779b9 + (h<<6) + (h>>2); };
    combine(reinterpret_cast<size_t>(k.vs));
    combine(reinterpret_cast<size_t>(k.fs));
    combine(reinterpret_cast<size_t>(k.render_pass));
    combine(reinterpret_cast<size_t>(k.layout));
    combine(static_cast<size_t>(k.topology));
    combine(static_cast<size_t>(k.rasterizer.cull_mode));
    combine(static_cast<size_t>(k.rasterizer.front_face));
    combine(k.rasterizer.wireframe ? 1 : 0);
    combine(k.depth.test_enabled ? 1 : 0);
    combine(k.depth.write_enabled ? 1 : 0);
    combine(static_cast<size_t>(k.depth.compare));
    combine(k.vertex_binding);
    combine(k.vertex_stride);
    combine(k.push_constant_size);
    combine(static_cast<size_t>(static_cast<u16>(k.push_constant_stages)));
    for (auto& a : k.vertex_attribs) {
        combine(a.location);
        combine(a.offset);
        combine(static_cast<size_t>(a.format));
    }
    return h;
}

rhi::Pipeline* PipelineCache::get_or_create(const rhi::PipelineDesc& desc) {
    Key key;
    key.vs = desc.vs;
    key.fs = desc.fs;
    key.render_pass = desc.render_pass;
    key.layout = desc.descriptor_set_layout;
    key.vertex_binding = desc.vertex_layout.binding;
    key.vertex_stride = desc.vertex_layout.stride;
    // Owned copy: this is the critical fix — the key owns its attribute data
    // so the source PipelineDesc can be a temporary (e.g. vector that dies after the call).
    key.vertex_attribs.assign(desc.vertex_layout.attributes.begin(), desc.vertex_layout.attributes.end());
    key.topology = desc.topology;
    key.rasterizer = desc.rasterizer;
    key.depth = desc.depth;
    key.push_constant_size = desc.push_constant_size;
    key.push_constant_stages = desc.push_constant_stages;

    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        NF_LOG_TRACE(LogCategory::RHI, "PipelineCache hit");
        return it->second.get();
    }

    // Copy desc but replace the span with our owned storage's span for the actual creation.
    // The RHI pipeline creation only needs the span to be valid during the call, so we can
    // create a temporary PipelineDesc that points to the key's owned vector.
    rhi::PipelineDesc owned_desc = desc;
    owned_desc.vertex_layout.attributes = std::span<const rhi::VertexAttrib>(key.vertex_attribs);

    auto pipeline = m_device.create_pipeline(owned_desc);
    if (!pipeline) {
        NF_LOG_ERROR(LogCategory::RHI, "PipelineCache: failed to create pipeline");
        return nullptr;
    }
    rhi::Pipeline* raw = pipeline.get();
    m_cache.emplace(std::move(key), std::move(pipeline));
    NF_LOG_INFO(LogCategory::RHI, "PipelineCache: new pipeline cached (total {})", m_cache.size());
    return raw;
}

size_t PipelineCache::invalidate_shader(const rhi::ShaderModule* shader) {
    if (!shader) return 0;
    size_t removed = 0;
    for (auto it = m_cache.begin(); it != m_cache.end(); ) {
        if (it->first.vs == shader || it->first.fs == shader) {
            it = m_cache.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        NF_LOG_INFO(LogCategory::RHI, "PipelineCache: invalidated {} pipelines for shader {}", removed, static_cast<const void*>(shader));
    }
    return removed;
}

size_t PipelineCache::invalidate_all() {
    size_t n = m_cache.size();
    m_cache.clear();
    return n;
}

void PipelineCache::clear() {
    m_cache.clear();
}

} // namespace nf::rendering
