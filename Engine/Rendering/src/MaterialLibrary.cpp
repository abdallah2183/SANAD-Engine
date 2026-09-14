#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Core/Logger.hpp>

#include <cstring>

namespace nf::rendering {

// Matches the gbuffer shader's MaterialParams uniform block (std140, 48 bytes).
static constexpr usize kMaterialParamsUboSize = 12 * sizeof(float);

MaterialLibrary::MaterialLibrary(rhi::IGraphicsDevice& device) : m_device(&device) {}

MaterialHandle MaterialLibrary::create_instance(Material& material, const PBRMaterialParams& params,
                                                std::string name) {
    auto entry = std::make_unique<MaterialEntry>();
    entry->material = &material;
    entry->params = params;
    entry->name = std::move(name);

    rhi::BufferDesc ubo_desc{};
    ubo_desc.size = kMaterialParamsUboSize;
    ubo_desc.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst;
    ubo_desc.memory = rhi::MemoryUsage::CPUToGPU;
    entry->params_ubo = m_device->create_buffer(ubo_desc);
    if (!entry->params_ubo) {
        NF_LOG_ERROR(LogCategory::RHI, "MaterialLibrary: failed to create params buffer for '{}'", entry->name);
        return kInvalidMaterialHandle;
    }

    float packed[12];
    params.pack(packed);
    entry->params_ubo->update(packed, 0, kMaterialParamsUboSize);

    m_entries.push_back(std::move(entry));
    return MaterialHandle{static_cast<u32>(m_entries.size() - 1)};
}

MaterialEntry* MaterialLibrary::get(MaterialHandle handle) {
    if (!handle.valid() || handle.id >= m_entries.size()) return nullptr;
    return m_entries[handle.id].get();
}

const MaterialEntry* MaterialLibrary::get(MaterialHandle handle) const {
    if (!handle.valid() || handle.id >= m_entries.size()) return nullptr;
    return m_entries[handle.id].get();
}

void MaterialLibrary::set_params(MaterialHandle handle, const PBRMaterialParams& params) {
    MaterialEntry* entry = get(handle);
    if (!entry || !entry->params_ubo) return;
    entry->params = params;
    float packed[12];
    params.pack(packed);
    entry->params_ubo->update(packed, 0, kMaterialParamsUboSize);
}

const PBRMaterialParams* MaterialLibrary::params(MaterialHandle handle) const {
    const MaterialEntry* entry = get(handle);
    return entry ? &entry->params : nullptr;
}

void MaterialLibrary::clear_albedo_texture(MaterialHandle handle) {
    MaterialEntry* entry = get(handle);
    if (!entry) {
        return;
    }
    entry->albedo_view = nullptr;
    entry->sampler = nullptr;
    // Binding changed: the cached descriptor set no longer matches. Marked, not
    // destroyed — the renderer rebuilds it after it has waited on the fence.
    entry->set_dirty = true;
    PBRMaterialParams p = entry->params;
    p.use_base_color_texture = 0.0f;
    set_params(handle, p);
}

void MaterialLibrary::set_albedo_texture(MaterialHandle handle, const rhi::TextureView& view,
                                          const rhi::Sampler& sampler) {
    MaterialEntry* entry = get(handle);
    if (!entry) return;
    entry->albedo_view = &view;
    entry->sampler = &sampler;
    entry->set_dirty = true; // see clear_albedo_texture

    // Tell the shader to sample the texture instead of the scalar base color
    PBRMaterialParams p = entry->params;
    p.use_base_color_texture = 1.0f;
    set_params(handle, p);
}

} // namespace nf::rendering
