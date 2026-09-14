#pragma once

#include <NF/RHI/RHI.hpp>
#include <NF/Core/Types.hpp>

namespace nf::rendering {

// Generic, backend-neutral resource states. The RenderGraph speaks only this
// language — no Vulkan types cross this line. The backend alone translates a
// transition (previous usage → new usage) into its own layout/stage/access
// triple (e.g. VkImageLayout / VkPipelineStageFlags / VkAccessFlags).
enum class RGResourceState : u8 {
    Undefined = 0,
    ColorAttachment,
    DepthAttachment,
    ShaderRead,
    ShaderWrite,
    TransferSrc,
    TransferDst,
    Present
};

// Converts RG state to RHI ImageUsage for generic barrier
rhi::ImageUsage to_image_usage(RGResourceState state);
RGResourceState from_image_usage(rhi::ImageUsage usage);

// For RenderGraph's automatic barrier inference: given previous and new state,
// returns the before/after ImageUsage pair for RHI barrier_texture
struct UsageTransition {
    rhi::ImageUsage before = rhi::ImageUsage::None;
    rhi::ImageUsage after = rhi::ImageUsage::None;
};

UsageTransition get_transition(RGResourceState from, RGResourceState to);

} // namespace nf::rendering
