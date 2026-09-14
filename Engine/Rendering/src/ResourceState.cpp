#include <NF/Rendering/ResourceState.hpp>

namespace nf::rendering {

rhi::ImageUsage to_image_usage(RGResourceState state) {
    switch (state) {
        case RGResourceState::Undefined:       return rhi::ImageUsage::None;
        case RGResourceState::ColorAttachment: return rhi::ImageUsage::ColorAtt;
        case RGResourceState::DepthAttachment: return rhi::ImageUsage::DepthAtt;
        case RGResourceState::ShaderRead:      return rhi::ImageUsage::Sampled;
        case RGResourceState::ShaderWrite:     return rhi::ImageUsage::Storage;
        case RGResourceState::TransferSrc:     return rhi::ImageUsage::TransferSrc;
        case RGResourceState::TransferDst:     return rhi::ImageUsage::TransferDst;
        case RGResourceState::Present:         return rhi::ImageUsage::ColorAtt; // will be handled as Present in RHI
        default: return rhi::ImageUsage::None;
    }
}

RGResourceState from_image_usage(rhi::ImageUsage usage) {
    if (usage == rhi::ImageUsage::None) return RGResourceState::Undefined;
    if ((static_cast<u16>(usage) & static_cast<u16>(rhi::ImageUsage::ColorAtt)) != 0) return RGResourceState::ColorAttachment;
    if ((static_cast<u16>(usage) & static_cast<u16>(rhi::ImageUsage::DepthAtt)) != 0) return RGResourceState::DepthAttachment;
    if ((static_cast<u16>(usage) & static_cast<u16>(rhi::ImageUsage::Sampled)) != 0) return RGResourceState::ShaderRead;
    if ((static_cast<u16>(usage) & static_cast<u16>(rhi::ImageUsage::Storage)) != 0) return RGResourceState::ShaderWrite;
    if ((static_cast<u16>(usage) & static_cast<u16>(rhi::ImageUsage::TransferSrc)) != 0) return RGResourceState::TransferSrc;
    if ((static_cast<u16>(usage) & static_cast<u16>(rhi::ImageUsage::TransferDst)) != 0) return RGResourceState::TransferDst;
    return RGResourceState::Undefined;
}

UsageTransition get_transition(RGResourceState from, RGResourceState to) {
    UsageTransition t;
    t.before = to_image_usage(from);
    t.after = to_image_usage(to);
    return t;
}

} // namespace nf::rendering
