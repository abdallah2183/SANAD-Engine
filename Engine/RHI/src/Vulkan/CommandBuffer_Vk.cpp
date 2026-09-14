// NF/RHI/src/Vulkan/CommandBuffer_Vk.cpp — Vulkan command buffer implementation

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <vector>

namespace nf::rhi {

// ---------------------------------------------------------------------------
// Command buffer
// ---------------------------------------------------------------------------

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice* device)
    : m_device(device) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanCommandBuffer requires a device");

    const VkDevice vk_device = m_device->context().device;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = m_device->context().graphics_family;
    if (!NF_VK_CHECK(vkCreateCommandPool(vk_device, &pool_info, nullptr, &m_pool))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create command buffer's private pool");
        m_pool = VK_NULL_HANDLE;
        return;
    }

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = m_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    if (!NF_VK_CHECK(vkAllocateCommandBuffers(m_device->context().device, &alloc_info, &m_cmd))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to allocate command buffer");
    }
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    // Destroying the private pool implicitly frees the command buffer — no
    // reset/free dance, and no handle reuse that could inherit stale
    // object-lifetime bindings.
    if (vk_device != VK_NULL_HANDLE && m_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
        m_cmd = VK_NULL_HANDLE;
    }
}

void VulkanCommandBuffer::reset() {
    if (!NF_VK_CHECK(vkResetCommandBuffer(m_cmd, 0))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to reset command buffer");
        return;
    }
    m_recording = false;
}

void VulkanCommandBuffer::begin() {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = 0;
    begin_info.pInheritanceInfo = nullptr;

    if (!NF_VK_CHECK(vkBeginCommandBuffer(m_cmd, &begin_info))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to begin command buffer");
        return;
    }
    m_recording = true;
}

void VulkanCommandBuffer::end() {
    if (!m_recording) {
        NF_LOG_WARN(LogCategory::RHI, "end() called on a command buffer that is not recording");
        return;
    }

    if (!NF_VK_CHECK(vkEndCommandBuffer(m_cmd))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to end command buffer");
        return;
    }
    m_recording = false;
}

void VulkanCommandBuffer::begin_render_pass(const RenderPass& pass, const Framebuffer& fb,
                                            std::span<const ClearValue> clear_colors,
                                            float clear_depth, u8 clear_stencil) {
    const auto& vk_pass = static_cast<const VulkanRenderPass&>(pass);
    const auto& vk_fb = static_cast<const VulkanFramebuffer&>(fb);

    const u32 attachment_count = vk_pass.color_attachment_count();

    // One clear value per color attachment, in attachment order.
    std::vector<VkClearValue> clear_values;
    clear_values.reserve(attachment_count + 1);

    for (u32 i = 0; i < attachment_count; ++i) {
        VkClearValue value{};
        if (i < clear_colors.size()) {
            value.color.float32[0] = clear_colors[i].r;
            value.color.float32[1] = clear_colors[i].g;
            value.color.float32[2] = clear_colors[i].b;
            value.color.float32[3] = clear_colors[i].a;
        } else {
            // Opaque black for any attachment the caller did not specify.
            value.color.float32[0] = 0.0f;
            value.color.float32[1] = 0.0f;
            value.color.float32[2] = 0.0f;
            value.color.float32[3] = 1.0f;
        }
        clear_values.push_back(value);
    }

    if (vk_pass.has_depth()) {
        VkClearValue value{};
        value.depthStencil.depth = clear_depth;
        value.depthStencil.stencil = static_cast<u32>(clear_stencil);
        clear_values.push_back(value);
    }

    VkRenderPassBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = vk_pass.handle();
    begin_info.framebuffer = vk_fb.handle();
    begin_info.renderArea.offset = { 0, 0 };
    begin_info.renderArea.extent = { vk_fb.width(), vk_fb.height() };
    begin_info.clearValueCount = static_cast<u32>(clear_values.size());
    begin_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(m_cmd, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

    // The pass transitions its attachments implicitly. Record that so later
    // operations (a readback, a sample) emit a barrier from the layout the
    // image is actually in — otherwise the first copy after a render pass
    // would treat the image as UNDEFINED and discard its contents.
    const VkImageLayout final_layout = vk_pass.final_color_layout();
    for (Texture* tex : vk_fb.color_attachments()) {
        static_cast<const VulkanTexture*>(tex)->set_layout(final_layout);
    }
    if (Texture* depth = vk_fb.depth_attachment()) {
        static_cast<const VulkanTexture*>(depth)->set_layout(
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
}

void VulkanCommandBuffer::end_render_pass() {
    vkCmdEndRenderPass(m_cmd);
}

void VulkanCommandBuffer::bind_pipeline(const Pipeline& pipeline) {
    const auto& vk_pipeline = static_cast<const VulkanPipeline&>(pipeline);
    vkCmdBindPipeline(m_cmd, vk_pipeline.bind_point(), vk_pipeline.handle());

    // Remembered so push_constants() can supply the layout Vulkan requires.
    m_current_layout = vk_pipeline.layout();
}

void VulkanCommandBuffer::bind_vertex_buffers(std::span<const Buffer* const> buffers) {
    if (buffers.empty()) return;

    std::vector<VkBuffer> vk_buffers;
    std::vector<VkDeviceSize> offsets;
    vk_buffers.reserve(buffers.size());
    offsets.reserve(buffers.size());

    for (const Buffer* buffer : buffers) {
        if (!buffer) {
            NF_LOG_ERROR(LogCategory::RHI, "bind_vertex_buffers received a null buffer");
            return;
        }
        vk_buffers.push_back(static_cast<const VulkanBuffer*>(buffer)->handle());
        offsets.push_back(0);
    }

    vkCmdBindVertexBuffers(m_cmd, 0, static_cast<u32>(vk_buffers.size()),
                            vk_buffers.data(), offsets.data());
}

void VulkanCommandBuffer::bind_index_buffer(const Buffer& buffer, usize offset) {
    const auto& vk_buffer = static_cast<const VulkanBuffer&>(buffer);
    vkCmdBindIndexBuffer(m_cmd, vk_buffer.handle(), static_cast<VkDeviceSize>(offset),
                         VK_INDEX_TYPE_UINT32);
}

void VulkanCommandBuffer::bind_descriptor_sets(const DescriptorSetLayout& layout,
                                                 std::span<const DescriptorSet* const> sets,
                                                 u32 first_set) {
    const auto& vk_layout = static_cast<const VulkanDescriptorSetLayout&>(layout);
    if (!vk_layout.valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "bind_descriptor_sets received an invalid layout");
        return;
    }

    if (!m_current_layout) {
        NF_LOG_ERROR(LogCategory::RHI,
            "bind_descriptor_sets called before any pipeline was bound — "
            "the bound pipeline's layout is required for descriptor binding");
        return;
    }

    if (sets.empty()) return;

    std::vector<VkDescriptorSet> vk_sets;
    vk_sets.reserve(sets.size());

    for (const DescriptorSet* set : sets) {
        if (!set) {
            NF_LOG_ERROR(LogCategory::RHI, "bind_descriptor_sets received a null set");
            return;
        }
        VkDescriptorSet handle = VK_NULL_HANDLE;
        if (auto* s = dynamic_cast<const VulkanDescriptorSet*>(set)) {
            if (!s->valid()) {
                NF_LOG_ERROR(LogCategory::RHI, "bind_descriptor_sets received an invalid set");
                return;
            }
            handle = s->handle();
        } else if (auto* p = dynamic_cast<const VulkanDescriptorSetPooled*>(set)) {
            if (!p->valid()) {
                NF_LOG_ERROR(LogCategory::RHI, "bind_descriptor_sets received an invalid pooled set");
                return;
            }
            handle = p->handle();
        } else {
            NF_LOG_ERROR(LogCategory::RHI, "bind_descriptor_sets received an unknown DescriptorSet type");
            return;
        }
        vk_sets.push_back(handle);
    }

    // vkCmdBindDescriptorSets needs the VkPipelineLayout the pipeline was
    // built against, not the VkDescriptorSetLayout. The RHI keeps the
    // layout from the most recent bind_pipeline(), which is exactly the
    // layout the pipeline was created with.
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_current_layout, first_set,
                            static_cast<u32>(vk_sets.size()), vk_sets.data(),
                            0, nullptr);
}

void VulkanCommandBuffer::push_constants(ShaderStage stages, u32 offset, u32 size,
                                          const void* data) {
    if (!data || size == 0) {
        NF_LOG_ERROR(LogCategory::RHI, "push_constants called with null data or zero size");
        return;
    }

    if (!m_current_layout) {
        NF_LOG_ERROR(LogCategory::RHI,
            "push_constants called before any pipeline was bound");
        return;
    }

    VkShaderStageFlags visibility = 0;
    const u16 stage_bits = static_cast<u16>(stages);
    if (stage_bits & static_cast<u16>(ShaderStage::Vertex))   visibility |= VK_SHADER_STAGE_VERTEX_BIT;
    if (stage_bits & static_cast<u16>(ShaderStage::Fragment)) visibility |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (stage_bits & static_cast<u16>(ShaderStage::Compute))  visibility |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (stage_bits & static_cast<u16>(ShaderStage::Geometry)) visibility |= VK_SHADER_STAGE_GEOMETRY_BIT;

    vkCmdPushConstants(m_cmd, m_current_layout, visibility, offset, size, data);
}

void VulkanCommandBuffer::set_viewport(u32 x, u32 y, u32 width, u32 height,
                                       float min_depth, float max_depth) {
    VkViewport viewport{};
    viewport.x = static_cast<float>(x);
    viewport.y = static_cast<float>(y);
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = min_depth;
    viewport.maxDepth = max_depth;

    vkCmdSetViewport(m_cmd, 0, 1, &viewport);
}

void VulkanCommandBuffer::set_scissor(u32 x, u32 y, u32 width, u32 height) {
    VkRect2D scissor{};
    scissor.offset = { static_cast<i32>(x), static_cast<i32>(y) };
    scissor.extent = { width, height };

    vkCmdSetScissor(m_cmd, 0, 1, &scissor);
}

void VulkanCommandBuffer::draw(u32 vertex_count, u32 instance_count,
                               u32 first_vertex, u32 first_instance) {
    vkCmdDraw(m_cmd, vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanCommandBuffer::draw_indexed(u32 index_count, u32 instance_count,
                                       u32 first_index, i32 vertex_offset,
                                       u32 first_instance) {
    vkCmdDrawIndexed(m_cmd, index_count, instance_count, first_index,
                     vertex_offset, first_instance);
}

void VulkanCommandBuffer::copy_buffer(const Buffer& src, Buffer& dst,
                                      usize src_offset, usize dst_offset, usize size) {
    const auto& vk_src = static_cast<const VulkanBuffer&>(src);
    const auto& vk_dst = static_cast<VulkanBuffer&>(dst);

    VkBufferCopy region{};
    region.srcOffset = static_cast<VkDeviceSize>(src_offset);
    region.dstOffset = static_cast<VkDeviceSize>(dst_offset);
    region.size = static_cast<VkDeviceSize>(size);

    vkCmdCopyBuffer(m_cmd, vk_src.handle(), vk_dst.handle(), 1, &region);

    // Make this copy's writes visible to the next transfer that touches the
    // destination. Consecutive copies into the same buffer (staging → GPU →
    // readback chains) have no other execution dependency and would race.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanCommandBuffer::copy_buffer_to_texture(const Buffer& src, Texture& dst,
                                                 usize buffer_offset,
                                                 u32 tex_x, u32 tex_y,
                                                 u32 tex_w, u32 tex_h) {
    const auto& vk_src = static_cast<const VulkanBuffer&>(src);
    auto& vk_dst = static_cast<VulkanTexture&>(dst);

    const bool is_depth = (vk_dst.format() == Format::D16_UNorm ||
                           vk_dst.format() == Format::D32_SFloat ||
                           vk_dst.format() == Format::D24_UNorm_S8_UInt ||
                           vk_dst.format() == Format::D32_SFloat_S8_UInt);

    const VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                               : VK_IMAGE_ASPECT_COLOR_BIT;

    // The destination must be in the transfer-optimal layout before the copy.
    const VkImageLayout old_layout = vk_dst.layout();

    if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        transition_image_layout(vk_dst.image(), old_layout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect);
    }

    VkBufferImageCopy region{};
    region.bufferOffset = static_cast<VkDeviceSize>(buffer_offset);
    region.bufferRowLength = 0;   // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspect;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { static_cast<i32>(tex_x), static_cast<i32>(tex_y), 0 };
    region.imageExtent = { tex_w, tex_h, 1 };

    vkCmdCopyBufferToImage(m_cmd, vk_src.handle(), vk_dst.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    vk_dst.set_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

bool VulkanCommandBuffer::generate_mipmaps(Texture& texture) {
    auto& vk_tex = static_cast<VulkanTexture&>(texture);
    if (!vk_tex.valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "generate_mipmaps: invalid texture");
        return false;
    }
    const u32 mips = vk_tex.mip_levels();
    if (mips <= 1) {
        return true; // nothing to do
    }
    if (vk_tex.format() != Format::R8G8B8A8_UNorm && vk_tex.format() != Format::R8G8B8A8_sRGB &&
        vk_tex.format() != Format::B8G8R8A8_UNorm) {
        NF_LOG_ERROR(LogCategory::RHI, "generate_mipmaps: unsupported format for blitting");
        return false;
    }
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(m_device->context().physical_device, vk_tex.vk_format(),
                                        &props);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((props.optimalTilingFeatures & need) != need) {
        NF_LOG_ERROR(LogCategory::RHI, "generate_mipmaps: format lacks blit support");
        return false;
    }

    // Per-mip barrier (the shared helper only handles whole-image ranges).
    auto mip_barrier = [&](u32 mip, VkImageLayout old_layout, VkImageLayout new_layout,
                           VkAccessFlags src_access, VkAccessFlags dst_access,
                           VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vk_tex.image();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = mip;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        vkCmdPipelineBarrier(m_cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };

    u32 w = vk_tex.width();
    u32 h = vk_tex.height();
    for (u32 i = 1; i < mips; ++i) {
        // Blit source: previous level finished writing (or was just copied).
        mip_barrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // Blit destination: undefined to transfer-destination.
        mip_barrier(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<i32>(w), static_cast<i32>(h), 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<i32>(w > 1 ? w / 2 : 1), static_cast<i32>(h > 1 ? h / 2 : 1),
                              1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        vkCmdBlitImage(m_cmd, vk_tex.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_tex.image(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        // Source level is done being blitted from: make it shader-readable.
        mip_barrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        if (w > 1) {
            w /= 2;
        }
        if (h > 1) {
            h /= 2;
        }
    }
    // The last level never became a blit source: transition it for sampling.
    mip_barrier(mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk_tex.set_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

void VulkanCommandBuffer::transition_texture_for_sampling(Texture& texture) {
    auto& vk_tex = static_cast<VulkanTexture&>(texture);
    if (!vk_tex.valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "transition_texture_for_sampling: invalid texture");
        return;
    }

    const VkImageLayout current = vk_tex.layout();
    if (current == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return; // Already readable — transitioning again would be a no-op
    }

    const bool is_depth = (vk_tex.format() == Format::D16_UNorm ||
                           vk_tex.format() == Format::D32_SFloat ||
                           vk_tex.format() == Format::D24_UNorm_S8_UInt ||
                           vk_tex.format() == Format::D32_SFloat_S8_UInt);

    const VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                               : VK_IMAGE_ASPECT_COLOR_BIT;

    transition_image_layout(vk_tex.image(), current,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect);

    vk_tex.set_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanCommandBuffer::barrier_texture(Texture& texture, ImageUsage before, ImageUsage after) {
    auto& vk_tex = static_cast<VulkanTexture&>(texture);
    if (!vk_tex.valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "barrier_texture: invalid texture");
        return;
    }

    // Map ImageUsage to layout
    auto usage_to_layout = [](ImageUsage u) -> VkImageLayout {
        if (u == ImageUsage::None) return VK_IMAGE_LAYOUT_UNDEFINED;
        if (has_usage(u, ImageUsage::ColorAtt)) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (has_usage(u, ImageUsage::DepthAtt)) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (has_usage(u, ImageUsage::Sampled)) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (has_usage(u, ImageUsage::TransferSrc)) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (has_usage(u, ImageUsage::TransferDst)) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (has_usage(u, ImageUsage::Storage)) return VK_IMAGE_LAYOUT_GENERAL;
        return VK_IMAGE_LAYOUT_UNDEFINED;
    };

    const bool is_depth = (vk_tex.format() == Format::D16_UNorm ||
                           vk_tex.format() == Format::D32_SFloat ||
                           vk_tex.format() == Format::D24_UNorm_S8_UInt ||
                           vk_tex.format() == Format::D32_SFloat_S8_UInt);
    VkImageLayout old_layout = usage_to_layout(before);
    VkImageLayout new_layout = usage_to_layout(after);

    // If before is None (Undefined), use the texture's current tracked layout
    if (before == ImageUsage::None) old_layout = vk_tex.layout();
    const VkImageAspectFlags aspect_barrier =
        is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (old_layout == new_layout) {
        // Same-usage barrier: no layout transition, but prior writes must
        // become visible to subsequent reads. The chaining case is two render
        // passes on one image in a single command buffer (scene pass, then a
        // Load pass such as the editor UI overlay): without this, loadOp=LOAD
        // can read stale attachment data — silent on some GPUs, garbage on
        // tiled ones. Scoped to color attachments; every other same-usage
        // pair stays a no-op as before.
        if (has_usage(before, ImageUsage::ColorAtt) && has_usage(after, ImageUsage::ColorAtt)) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = vk_tex.layout();
            barrier.newLayout = vk_tex.layout();
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = vk_tex.image();
            barrier.subresourceRange.aspectMask = aspect_barrier;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &barrier);
            // Tracker unchanged: the image never left its layout.
        }
        return;
    }

    const VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    transition_image_layout(vk_tex.image(), old_layout, new_layout, aspect);
    vk_tex.set_layout(new_layout);
}

void VulkanCommandBuffer::copy_texture_to_buffer(const Texture& src, Buffer& dst,
                                                 u32 tex_x, u32 tex_y,
                                                 u32 tex_w, u32 tex_h,
                                                 usize buffer_offset) {
    const auto& vk_src = static_cast<const VulkanTexture&>(src);
    auto& vk_dst = static_cast<VulkanBuffer&>(dst);

    const bool is_depth = (vk_src.format() == Format::D16_UNorm ||
                           vk_src.format() == Format::D32_SFloat ||
                           vk_src.format() == Format::D24_UNorm_S8_UInt ||
                           vk_src.format() == Format::D32_SFloat_S8_UInt);

    const VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                               : VK_IMAGE_ASPECT_COLOR_BIT;

    // The source must be readable by the transfer unit before the copy.
    const VkImageLayout old_layout = vk_src.layout();
    if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        transition_image_layout(vk_src.image(), old_layout,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);
    }

    VkBufferImageCopy region{};
    region.bufferOffset = static_cast<VkDeviceSize>(buffer_offset);
    region.bufferRowLength = 0;   // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspect;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { static_cast<i32>(tex_x), static_cast<i32>(tex_y), 0 };
    region.imageExtent = { tex_w, tex_h, 1 };

    vkCmdCopyImageToBuffer(m_cmd, vk_src.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           vk_dst.handle(), 1, &region);

    // The copy leaves the image in TRANSFER_SRC_OPTIMAL; record that so the
    // next transition knows where it stands.
    vk_src.set_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

void VulkanCommandBuffer::transition_image_layout(VkImage image, VkImageLayout old_layout,
                                                  VkImageLayout new_layout,
                                                  VkImageAspectFlags aspect) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destination_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        // Uploads, readbacks and blits: make the previous writes visible to
        // the transfer unit. Color-attachment writes and transfer writes are
        // both covered so this works after rendering and after an upload.
        if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            source_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else {
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            source_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        // On-screen and offscreen targets both go through here before the
        // first draw that writes them.
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        // Readback-then-sample cycle (e.g. the editor viewport target, which
        // is copied for pixel proofs on some frames and sampled by the UI).
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else {
        NF_LOG_WARN(LogCategory::RHI,
            "Unsupported image layout transition ({} -> {}); using a conservative barrier",
            static_cast<int>(old_layout), static_cast<int>(new_layout));
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destination_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(m_cmd, source_stage, destination_stage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

VulkanSemaphore::VulkanSemaphore(VulkanDevice* device) : m_device(device) {
    m_alive.attach(m_device);
    NF_ASSERT(m_device, "VulkanSemaphore requires a device");

    VkSemaphoreCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    if (!NF_VK_CHECK(vkCreateSemaphore(m_device->context().device, &create_info,
                                        nullptr, &m_semaphore))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create semaphore");
    }
}

VulkanSemaphore::~VulkanSemaphore() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device != VK_NULL_HANDLE && m_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(vk_device, m_semaphore, nullptr);
        m_semaphore = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Fence
// ---------------------------------------------------------------------------

VulkanFence::VulkanFence(VulkanDevice* device, bool signaled) : m_device(device) {
    m_alive.attach(m_device);
    NF_ASSERT(m_device, "VulkanFence requires a device");

    VkFenceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    create_info.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;

    if (!NF_VK_CHECK(vkCreateFence(m_device->context().device, &create_info,
                                    nullptr, &m_fence))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create fence");
    }
}

VulkanFence::~VulkanFence() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device != VK_NULL_HANDLE && m_fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk_device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
    }
}

bool VulkanFence::wait(u64 timeout_ns) {
    const VkResult result = vkWaitForFences(m_device->context().device, 1, &m_fence,
                                            VK_TRUE, timeout_ns);
    if (result == VK_TIMEOUT) {
        return false;
    }
    if (result != VK_SUCCESS) {
        NF_LOG_ERROR(LogCategory::RHI, "vkWaitForFences failed: {}", vk_result_string(result));
        return false;
    }
    return true;
}

void VulkanFence::reset() {
    if (!NF_VK_CHECK(vkResetFences(m_device->context().device, 1, &m_fence))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to reset fence");
    }
}

bool VulkanFence::is_signaled() {
    const VkResult result = vkWaitForFences(m_device->context().device, 1, &m_fence,
                                            VK_TRUE, 0);
    return result == VK_SUCCESS;
}

} // namespace nf::rhi
