#include <NF/Editor/Viewport.hpp>

namespace nf::editor {

bool ensure_viewport_target(rhi::IGraphicsDevice& device, const ViewportState& state,
                            ViewportResources& res) {
    if (state.width == 0 || state.height == 0) {
        return false;
    }
    if (res.target && res.width == state.width && res.height == state.height) {
        return true;
    }
    // Caller guarantees the GPU is idle; dropping the old texture here is safe.
    // View/sampler are destroyed first: views must never outlive their texture.
    res.reset();
    rhi::TextureDesc desc{};
    desc.width = state.width;
    desc.height = state.height;
    desc.format = rhi::Format::R8G8B8A8_UNorm;
    desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    res.target = device.create_texture(desc);
    if (!res.target) {
        return false;
    }
    rhi::TextureViewDesc vd{};
    vd.texture = res.target.get();
    res.view = device.create_texture_view(vd);
    rhi::SamplerDesc sd{};
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    res.sampler = device.create_sampler(sd);
    if (!res.view || !res.sampler) {
        res.reset();
        return false;
    }
    res.width = state.width;
    res.height = state.height;
    return true;
}

} // namespace nf::editor
