// NF/RHI/src/Vulkan/Pipeline_Vk.cpp — graphics pipeline + render pass + framebuffer

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <array>
#include <vector>

namespace nf::rhi {

// ---------------------------------------------------------------------------
// Render pass
// ---------------------------------------------------------------------------

VulkanRenderPass::VulkanRenderPass(VulkanDevice* device, const RenderPassDesc& desc)
    : m_device(device),
      m_depth_format(desc.depth_format),
      m_has_depth(desc.has_depth),
      m_present_source(desc.present_source),
      m_color_load(desc.color_load) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanRenderPass requires a device");

    // Keep an owning copy: the caller's span is only valid during the call.
    // desc() re-borrows this vector, so the pass owns its description outright.
    m_color_attachments.assign(desc.color_attachments.begin(), desc.color_attachments.end());

    // Depth-only passes (depth prepass) have no color attachments; that is a
    // valid Vulkan subpass. What makes no sense is a pass with neither color
    // nor depth — nothing to write to.
    if (m_color_attachments.empty() && !desc.has_depth) {
        NF_LOG_ERROR(LogCategory::RHI, "Render pass needs at least one color or depth attachment");
        return;
    }

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_refs;
    attachments.reserve(m_color_attachments.size() + 1);
    color_refs.reserve(m_color_attachments.size());

    // On-screen passes must leave the image presentation-ready; offscreen
    // targets stay renderable so they can be sampled or read back without
    // another transition. Only use PRESENT_SRC_KHR if we actually have a surface.
    const bool can_present = m_present_source && (m_device->context().surface != VK_NULL_HANDLE);
    const VkImageLayout present_or_attachment = can_present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    for (u32 i = 0; i < m_color_attachments.size(); ++i) {
        VkAttachmentDescription attachment{};
        attachment.format = to_vk_format(m_color_attachments[i].format);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = (m_color_load == RenderPassDesc::ColorLoad::Load)
                                ? VK_ATTACHMENT_LOAD_OP_LOAD
                                : VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // With Load, the pass promises to read the attachment's previous
        // contents — initialLayout must therefore declare the layout those
        // contents live in (mirroring finalLayout), exactly like DepthLoad.
        // UNDEFINED+LOAD is legal Vulkan but reads garbage.
        attachment.initialLayout = (m_color_load == RenderPassDesc::ColorLoad::Load)
                                       ? present_or_attachment
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = present_or_attachment;

        VkAttachmentReference ref{};
        ref.attachment = static_cast<u32>(i);
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        attachments.push_back(attachment);
        color_refs.push_back(ref);
    }

    VkAttachmentReference depth_ref{};
    if (desc.has_depth) {
        VkAttachmentDescription depth_attachment{};
        depth_attachment.format = to_vk_format(desc.depth_format);
        depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_attachment.loadOp = desc.depth_load == RenderPassDesc::DepthLoad::Load
                                      ? VK_ATTACHMENT_LOAD_OP_LOAD
                                      : VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        // With Load, the pass promises to read the attachment's previous
        // contents — initialLayout must therefore declare the layout those
        // contents live in. UNDEFINED+LOAD is legal Vulkan but reads garbage.
        depth_attachment.initialLayout =
            desc.depth_load == RenderPassDesc::DepthLoad::Load
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED;
        depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depth_ref.attachment = static_cast<u32>(attachments.size());
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        attachments.push_back(depth_attachment);
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<u32>(color_refs.size());
    subpass.pColorAttachments = color_refs.data();
    subpass.pDepthStencilAttachment = desc.has_depth ? &depth_ref : nullptr;

    // The subpass must not start before the swapchain image is acquired, and
    // must finish before presentation reads it. Also synchronize depth access.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (desc.color_load == RenderPassDesc::ColorLoad::Load) {
        // loadOp=LOAD reads the preserved attachment contents.
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    }

    if (desc.has_depth) {
        dependency.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkRenderPassCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = static_cast<u32>(attachments.size());
    create_info.pAttachments = attachments.data();
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;
    create_info.dependencyCount = 1;
    create_info.pDependencies = &dependency;

    if (!NF_VK_CHECK(vkCreateRenderPass(m_device->context().device, &create_info,
                                         nullptr, &m_render_pass))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create render pass");
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Render pass created ({} color attachment(s), depth: {})",
                 m_color_attachments.size(), desc.has_depth ? "yes" : "no");
}

VulkanRenderPass::~VulkanRenderPass() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;

    if (m_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vk_device, m_render_pass, nullptr);
        m_render_pass = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

VulkanFramebuffer::VulkanFramebuffer(VulkanDevice* device, const VulkanRenderPass& pass,
                                     std::span<Texture* const> color_attachments,
                                     Texture* depth_attachment)
    : m_device(device),
      m_color_attachments(color_attachments.begin(), color_attachments.end()),
      m_depth_attachment(depth_attachment) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanFramebuffer requires a device");

    // Depth-only framebuffers (depth prepass) have no color attachments but
    // must carry the depth attachment.
    if (color_attachments.empty() && !depth_attachment) {
        NF_LOG_ERROR(LogCategory::RHI, "Framebuffer needs at least one color or depth attachment");
        return;
    }

    std::vector<VkImageView> views;
    views.reserve(color_attachments.size() + 1);

    for (Texture* tex : color_attachments) {
        if (!tex) {
            NF_LOG_ERROR(LogCategory::RHI, "Framebuffer received a null color attachment");
            return;
        }
        views.push_back(static_cast<const VulkanTexture*>(tex)->view());
    }

    if (depth_attachment) {
        views.push_back(static_cast<const VulkanTexture*>(depth_attachment)->view());
    }

    // Attachment count must match the pass, otherwise the framebuffer is
    // silently incompatible with it and drawing fails at submit time.
    if (views.size() != pass.color_attachment_count() + (depth_attachment ? 1u : 0u)) {
        NF_LOG_ERROR(LogCategory::RHI,
            "Framebuffer has {} attachments but the render pass expects {}",
            views.size(), pass.color_attachment_count() + (depth_attachment ? 1u : 0u));
        return;
    }

    // Derive dimensions from the first attachment — all must match anyway.
    const auto* first = static_cast<const VulkanTexture*>(
        color_attachments.empty() ? depth_attachment : color_attachments[0]);
    m_width = first->width();
    m_height = first->height();

    VkFramebufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    create_info.renderPass = pass.handle();
    create_info.attachmentCount = static_cast<u32>(views.size());
    create_info.pAttachments = views.data();
    create_info.width = m_width;
    create_info.height = m_height;
    create_info.layers = 1;

    if (!NF_VK_CHECK(vkCreateFramebuffer(m_device->context().device, &create_info,
                                          nullptr, &m_framebuffer))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create framebuffer {}x{}", m_width, m_height);
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Framebuffer created: {}x{}", m_width, m_height);
}

VulkanFramebuffer::~VulkanFramebuffer() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;

    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(vk_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Graphics pipeline
// ---------------------------------------------------------------------------

VulkanPipeline::VulkanPipeline(VulkanDevice* device, const PipelineDesc& desc)
    : m_device(device) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanPipeline requires a device");

    if (!desc.vs) {
        NF_LOG_ERROR(LogCategory::RHI, "Graphics pipeline requires a vertex shader");
        return;
    }

    if (!desc.render_pass) {
        NF_LOG_ERROR(LogCategory::RHI,
            "Graphics pipeline requires the render pass it will be used with");
        return;
    }

    const auto* vk_render_pass = static_cast<const VulkanRenderPass*>(desc.render_pass);
    if (!vk_render_pass->valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "Graphics pipeline received an invalid render pass");
        return;
    }

    const VkDevice vk_device = m_device->context().device;

    // --- Shader stages ----------------------------------------------------
    std::vector<VkPipelineShaderStageCreateInfo> stages;

    const auto* vk_vs = static_cast<const VulkanShaderModule*>(desc.vs);
    VkPipelineShaderStageCreateInfo vs_stage{};
    vs_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs_stage.module = vk_vs->handle();
    vs_stage.pName = vk_vs->entry_point();
    stages.push_back(vs_stage);

    const VulkanShaderModule* vk_fs = nullptr;
    if (desc.fs) {
        vk_fs = static_cast<const VulkanShaderModule*>(desc.fs);
        VkPipelineShaderStageCreateInfo fs_stage{};
        fs_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fs_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fs_stage.module = vk_fs->handle();
        fs_stage.pName = vk_fs->entry_point();
        stages.push_back(fs_stage);
    }

    // --- Vertex input -----------------------------------------------------
    std::vector<VkVertexInputAttributeDescription> attribs;
    attribs.reserve(desc.vertex_layout.attributes.size());

    for (const VertexAttrib& attrib : desc.vertex_layout.attributes) {
        VkVertexInputAttributeDescription vk_attrib{};
        vk_attrib.location = attrib.location;
        vk_attrib.binding = desc.vertex_layout.binding;
        vk_attrib.format = to_vk_format(attrib.format);
        vk_attrib.offset = attrib.offset;
        attribs.push_back(vk_attrib);
    }

    VkVertexInputBindingDescription binding{};
    binding.binding = desc.vertex_layout.binding;
    binding.stride = desc.vertex_layout.stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = desc.vertex_layout.stride > 0 ? 1u : 0u;
    vertex_input.pVertexBindingDescriptions = desc.vertex_layout.stride > 0 ? &binding : nullptr;
    vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(attribs.size());
    vertex_input.pVertexAttributeDescriptions = attribs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = to_vk_topology(desc.topology);
    input_assembly.primitiveRestartEnable = VK_FALSE;

    // --- Tessellation (unused) --------------------------------------------
    VkPipelineTessellationStateCreateInfo tessellation{};
    tessellation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;

    // --- Viewport / scissor ------------------------------------------------
    // Dynamic state is used so a single pipeline can target any swapchain size
    // without recompilation, and so resize does not invalidate pipelines.
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    // --- Rasterizer --------------------------------------------------------
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = desc.rasterizer.wireframe ? VK_POLYGON_MODE_LINE
                                                       : VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = to_vk_cull_mode(desc.rasterizer.cull_mode);
    rasterizer.frontFace = to_vk_front_face(desc.rasterizer.front_face);
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // --- Depth / stencil ---------------------------------------------------
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = desc.depth.test_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth.write_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = to_vk_compare_op(desc.depth.compare);
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    // --- Blending ----------------------------------------------------------
    // One blend state per attachment, read from the pass we were given. Taking
    // it from the RenderPass rather than a second copy of the description is
    // what guarantees the pipeline and the pass can never drift apart.
    const RenderPassDesc pass_desc = vk_render_pass->desc();

    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments;
    blend_attachments.reserve(pass_desc.color_attachments.size());

    for (const ColorAttachment& attachment : pass_desc.color_attachments) {
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = attachment.blend_enabled ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = to_vk_blend_factor(attachment.src_color);
        blend.dstColorBlendFactor = to_vk_blend_factor(attachment.dst_color);
        blend.colorBlendOp = to_vk_blend_op(attachment.color_blend_op);
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_attachments.push_back(blend);
    }

    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.logicOpEnable = VK_FALSE;
    blending.attachmentCount = static_cast<u32>(blend_attachments.size());
    blending.pAttachments = blend_attachments.data();

    // --- Dynamic state -----------------------------------------------------
    const std::array<VkDynamicState, 2> dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<u32>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    // --- Pipeline layout ---------------------------------------------------
    VkPushConstantRange push_range{};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    // The descriptor set layout this pipeline samples its textures / uniforms
    // from. Null means no descriptor sets at all — which is what the
    // hardcoded-vertex triangle path uses.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (desc.descriptor_set_layout) {
        const auto* vk_layout =
            static_cast<const VulkanDescriptorSetLayout*>(desc.descriptor_set_layout);
        if (!vk_layout->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "Pipeline received an invalid descriptor set layout");
            return;
        }
        set_layout = vk_layout->handle();
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout;
    } else {
        layout_info.setLayoutCount = 0;
        layout_info.pSetLayouts = nullptr;
    }

    if (desc.push_constant_size > 0) {
        // Clamp to the guaranteed minimum; larger blocks are not portable.
        const u32 max_push = m_device->context().device_properties.limits.maxPushConstantsSize;
        if (desc.push_constant_size > max_push) {
            NF_LOG_ERROR(LogCategory::RHI,
                "Push constant size {} exceeds device limit {}", desc.push_constant_size, max_push);
            return;
        }

        VkShaderStageFlags visibility = 0;
        const u16 stage_bits = static_cast<u16>(desc.push_constant_stages);
        if (stage_bits & static_cast<u16>(ShaderStage::Vertex))   visibility |= VK_SHADER_STAGE_VERTEX_BIT;
        if (stage_bits & static_cast<u16>(ShaderStage::Fragment)) visibility |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (stage_bits & static_cast<u16>(ShaderStage::Compute))  visibility |= VK_SHADER_STAGE_COMPUTE_BIT;
        if (stage_bits & static_cast<u16>(ShaderStage::Geometry)) visibility |= VK_SHADER_STAGE_GEOMETRY_BIT;

        push_range.stageFlags = visibility;
        push_range.offset = 0;
        push_range.size = desc.push_constant_size;

        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
    }

    if (!NF_VK_CHECK(vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &m_layout))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create pipeline layout");
        return;
    }

    // --- Assemble ----------------------------------------------------------
    // The pipeline is built against the exact VkRenderPass it will be drawn
    // with, so compatibility is structural rather than something the caller
    // has to maintain.
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = static_cast<u32>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pTessellationState = &tessellation;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = m_layout;
    pipeline_info.renderPass = vk_render_pass->handle();
    pipeline_info.subpass = 0;

    if (!NF_VK_CHECK(vkCreateGraphicsPipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                nullptr, &m_pipeline))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create graphics pipeline");
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Graphics pipeline created");
}

VulkanPipeline::~VulkanPipeline() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

} // namespace nf::rhi
