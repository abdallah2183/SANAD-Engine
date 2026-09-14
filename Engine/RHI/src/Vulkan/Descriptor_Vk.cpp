// NF/RHI/src/Vulkan/Descriptor_Vk.cpp — Vulkan texture view, sampler,
// descriptor set layout, and descriptor set implementations.

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <array>
#include <vector>

namespace nf::rhi {

// ---------------------------------------------------------------------------
// TextureView
// ---------------------------------------------------------------------------

bool VulkanTextureView::create_from_desc(const TextureViewDesc& desc) {
    NF_ASSERT(m_device, "VulkanTextureView requires a device");
    if (!desc.texture) {
        NF_LOG_ERROR(LogCategory::RHI, "TextureViewDesc missing texture");
        return false;
    }
    const auto& vk_tex = static_cast<const VulkanTexture&>(*desc.texture);
    if (!vk_tex.valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "TextureView: source texture is invalid");
        return false;
    }

    // Resolve format: explicit view format overrides texture format
    m_vk_format = (desc.format != Format::Unknown) ? to_vk_format(desc.format) : vk_tex.vk_format();
    m_format = (desc.format != Format::Unknown) ? desc.format : vk_tex.format();
    m_width = vk_tex.width();
    m_height = vk_tex.height();
    m_dimension = desc.dimension;
    m_aspect = desc.aspect;

    // Validate mip/layer ranges against texture description
    // For now texture is always 2D with mip_levels=1, array_layers=1, so clamp
    u32 mip_count = desc.mip_count;
    u32 layer_count = desc.layer_count;
    if (mip_count == 0) mip_count = 1;
    if (layer_count == 0) layer_count = 1;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = vk_tex.image();
    view_info.viewType = to_vk_view_type(desc.dimension);
    view_info.format = m_vk_format;
    view_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
    };

    // Aspect: if caller asked Color but texture is depth, use depth aspect
    VkImageAspectFlags aspect = to_vk_image_aspect(desc.aspect);
    // If view is for depth texture but aspect is Color, correct it
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        // Keep color for color textures; for depth textures the caller should have set Depth
        // but we auto-correct if they used default Color on a depth texture
        VkFormat fmt = vk_tex.vk_format();
        if (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT ||
            fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) {
            // Depth texture with Color aspect requested — this is likely a mistake, but
            // we handle it by using the format's aspect
            aspect = aspect_for_format(m_format);
        }
    }

    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.baseMipLevel = desc.base_mip;
    view_info.subresourceRange.levelCount = mip_count;
    view_info.subresourceRange.baseArrayLayer = desc.base_layer;
    view_info.subresourceRange.layerCount = layer_count;

    if (!NF_VK_CHECK(vkCreateImageView(m_device->context().device, &view_info,
                                       nullptr, &m_view))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create texture view (dim={}, aspect={}, mips {}:{}, layers {}:{})",
                     static_cast<int>(desc.dimension), static_cast<int>(desc.aspect),
                     desc.base_mip, mip_count, desc.base_layer, layer_count);
        return false;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Texture view created: {}x{} dim={} aspect={} mips {} layers {}",
                 m_width, m_height, static_cast<int>(m_dimension), static_cast<int>(m_aspect),
                 mip_count, layer_count);
    return true;
}

VulkanTextureView::VulkanTextureView(VulkanDevice* device, const Texture& texture)
    : m_device(device) {
    m_alive.attach(m_device);
    TextureViewDesc desc{};
    desc.texture = &texture;
    desc.dimension = ViewDimension::View2D;
    desc.aspect = ImageAspect::Color;
    desc.format = Format::Unknown;
    desc.base_mip = 0;
    desc.mip_count = 1;
    desc.base_layer = 0;
    desc.layer_count = 1;
    if (!create_from_desc(desc)) {
        // create_from_desc already logged
    }
}

VulkanTextureView::VulkanTextureView(VulkanDevice* device, const TextureViewDesc& desc)
    : m_device(device) {
    m_alive.attach(m_device);
    if (!create_from_desc(desc)) {
        // error already logged
    }
}

VulkanTextureView::~VulkanTextureView() {
    if (!m_device) return;
    const VkDevice vk_device = m_device->context().device;
    if (vk_device != VK_NULL_HANDLE && m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk_device, m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

VulkanSampler::VulkanSampler(VulkanDevice* device, const SamplerDesc& desc)
    : m_device(device) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanSampler requires a device");

    VkSamplerCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    create_info.magFilter = to_vk_filter(desc.mag);
    create_info.minFilter = to_vk_filter(desc.min);
    create_info.mipmapMode = to_vk_mipmap_mode(desc.mip);
    create_info.addressModeU = to_vk_address_mode(desc.address_u);
    create_info.addressModeV = to_vk_address_mode(desc.address_v);
    create_info.addressModeW = to_vk_address_mode(desc.address_w);
    create_info.mipLodBias = 0.0f;
    create_info.anisotropyEnable = desc.anisotropy_enable ? VK_TRUE : VK_FALSE;
    create_info.maxAnisotropy = desc.max_anisotropy;
    create_info.compareEnable = VK_FALSE;
    create_info.compareOp = VK_COMPARE_OP_ALWAYS;
    create_info.minLod = desc.min_lod;
    create_info.maxLod = desc.max_lod;
    create_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    create_info.unnormalizedCoordinates = VK_FALSE;

    if (!NF_VK_CHECK(vkCreateSampler(m_device->context().device, &create_info,
                                     nullptr, &m_sampler))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create sampler");
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Sampler created");
}

VulkanSampler::~VulkanSampler() {
    if (!m_device) return;
    const VkDevice vk_device = m_device->context().device;
    if (vk_device != VK_NULL_HANDLE && m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// DescriptorSetLayout
// ---------------------------------------------------------------------------

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(
    VulkanDevice* device, const DescriptorSetLayoutDesc& desc)
    : m_device(device),
      m_bindings(desc.bindings.begin(), desc.bindings.end()) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanDescriptorSetLayout requires a device");

    if (m_bindings.empty()) {
        NF_LOG_ERROR(LogCategory::RHI, "Descriptor set layout needs at least one binding");
        return;
    }

    std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
    vk_bindings.reserve(m_bindings.size());

    for (const DescriptorBinding& b : m_bindings) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = b.binding;
        binding.descriptorType = to_vk_descriptor_type(b.type);
        binding.descriptorCount = b.count;

        VkShaderStageFlags stages = 0;
        const u16 stage_bits = static_cast<u16>(b.stages);
        if (stage_bits & static_cast<u16>(ShaderStage::Vertex))   stages |= VK_SHADER_STAGE_VERTEX_BIT;
        if (stage_bits & static_cast<u16>(ShaderStage::Fragment)) stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (stage_bits & static_cast<u16>(ShaderStage::Compute))  stages |= VK_SHADER_STAGE_COMPUTE_BIT;
        if (stage_bits & static_cast<u16>(ShaderStage::Geometry)) stages |= VK_SHADER_STAGE_GEOMETRY_BIT;
        binding.stageFlags = stages;

        vk_bindings.push_back(binding);
    }

    VkDescriptorSetLayoutCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.bindingCount = static_cast<u32>(vk_bindings.size());
    create_info.pBindings = vk_bindings.data();

    if (!NF_VK_CHECK(vkCreateDescriptorSetLayout(m_device->context().device,
                                                   &create_info, nullptr, &m_layout))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create descriptor set layout");
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Descriptor set layout created ({} bindings)",
                 m_bindings.size());
}

VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout() {
    if (!m_device) return;
    const VkDevice vk_device = m_device->context().device;
    if (vk_device != VK_NULL_HANDLE && m_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// DescriptorSet (1:1 pool — simple, correct, not for batch)
// ---------------------------------------------------------------------------

VulkanDescriptorSet::VulkanDescriptorSet(VulkanDevice* device,
                                           const VulkanDescriptorSetLayout& layout)
    : m_device(device) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanDescriptorSet requires a device");

    if (!layout.valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "Descriptor set requires a valid layout");
        return;
    }

    std::array<VkDescriptorPoolSize, 4> pool_sizes{};
    u32 pool_size_count = 0;

    for (const DescriptorBinding& b : layout.bindings()) {
        const VkDescriptorType type = to_vk_descriptor_type(b.type);
        bool found = false;
        for (u32 i = 0; i < pool_size_count; ++i) {
            if (pool_sizes[i].type == type) {
                pool_sizes[i].descriptorCount += b.count;
                found = true;
                break;
            }
        }
        if (!found) {
            pool_sizes[pool_size_count].type = type;
            pool_sizes[pool_size_count].descriptorCount = b.count;
            ++pool_size_count;
        }
    }

    if (pool_size_count == 0) {
        NF_LOG_ERROR(LogCategory::RHI, "Descriptor set layout declares no bindings");
        return;
    }

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = pool_size_count;
    pool_info.pPoolSizes = pool_sizes.data();

    if (!NF_VK_CHECK(vkCreateDescriptorPool(m_device->context().device, &pool_info,
                                              nullptr, &m_pool))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create descriptor pool");
        return;
    }

    VkDescriptorSetLayout set_layout = layout.handle();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = m_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &set_layout;

    if (!NF_VK_CHECK(vkAllocateDescriptorSets(m_device->context().device,
                                                 &alloc_info, &m_set))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to allocate descriptor set");
        vkDestroyDescriptorPool(m_device->context().device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Descriptor set allocated ({} pool size entries)",
                 pool_size_count);
}

VulkanDescriptorSet::~VulkanDescriptorSet() {
    if (!m_device) return;
    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
        m_set = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// DescriptorAllocator (growable, reset per frame)
// ---------------------------------------------------------------------------

VulkanDescriptorAllocator::VulkanDescriptorAllocator(VulkanDevice* device, u32 sets_per_pool)
    : m_device(device), m_sets_per_pool(sets_per_pool) {
    m_alive.attach(m_device);
    NF_ASSERT(m_device, "VulkanDescriptorAllocator requires a device");
    if (m_sets_per_pool == 0) m_sets_per_pool = 32;
    NF_LOG_TRACE(LogCategory::RHI, "Descriptor allocator created (sets_per_pool={})", m_sets_per_pool);
}

VulkanDescriptorAllocator::~VulkanDescriptorAllocator() {
    if (!m_device) return;
    const VkDevice vk_device = m_device->context().device;
    if (vk_device != VK_NULL_HANDLE) {
        for (Pool& p : m_pools) {
            if (p.pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(vk_device, p.pool, nullptr);
                p.pool = VK_NULL_HANDLE;
            }
        }
    }
    m_pools.clear();
    m_allocated = 0;
}

bool VulkanDescriptorAllocator::create_pool_for_layout(const VulkanDescriptorSetLayout& layout, Pool& out) {
    std::array<VkDescriptorPoolSize, 4> pool_sizes{};
    u32 pool_size_count = 0;
    for (const DescriptorBinding& b : layout.bindings()) {
        const VkDescriptorType type = to_vk_descriptor_type(b.type);
        bool found = false;
        for (u32 i = 0; i < pool_size_count; ++i) {
            if (pool_sizes[i].type == type) {
                pool_sizes[i].descriptorCount += b.count * m_sets_per_pool;
                found = true;
                break;
            }
        }
        if (!found) {
            pool_sizes[pool_size_count].type = type;
            pool_sizes[pool_size_count].descriptorCount = b.count * m_sets_per_pool;
            ++pool_size_count;
        }
    }
    if (pool_size_count == 0) return false;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // Allow individual sets to be freed — useful if allocator wants to free a single set
    // without resetting the whole frame. Not required for reset() path but harmless.
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = m_sets_per_pool;
    pool_info.poolSizeCount = pool_size_count;
    pool_info.pPoolSizes = pool_sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!NF_VK_CHECK(vkCreateDescriptorPool(m_device->context().device, &pool_info, nullptr, &pool))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create descriptor allocator pool ({} sets)", m_sets_per_pool);
        return false;
    }
    out.pool = pool;
    out.remaining = m_sets_per_pool;
    NF_LOG_TRACE(LogCategory::RHI, "Descriptor allocator: new pool ({} sets, {} types)", m_sets_per_pool, pool_size_count);
    return true;
}

std::unique_ptr<DescriptorSet> VulkanDescriptorAllocator::allocate(const DescriptorSetLayout& layout) {
    const auto& vk_layout = static_cast<const VulkanDescriptorSetLayout&>(layout);
    if (!vk_layout.valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "DescriptorAllocator::allocate with invalid layout");
        return nullptr;
    }

    // Try existing pools first
    for (Pool& p : m_pools) {
        if (p.remaining == 0) continue;
        VkDescriptorSetLayout set_layout = vk_layout.handle();
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = p.pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &set_layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult res = vkAllocateDescriptorSets(m_device->context().device, &alloc_info, &set);
        if (res == VK_SUCCESS) {
            --p.remaining;
            ++m_allocated;
            return std::make_unique<VulkanDescriptorSetPooled>(set);
        }
        if (res == VK_ERROR_OUT_OF_POOL_MEMORY || res == VK_ERROR_FRAGMENTED_POOL) {
            p.remaining = 0; // exhausted/fragmented, try next pool
            continue;
        }
        NF_LOG_ERROR(LogCategory::RHI, "vkAllocateDescriptorSets failed: {}", vk_result_string(res));
        return nullptr;
    }

    // Need a new pool sized for this layout
    Pool new_pool{};
    if (!create_pool_for_layout(vk_layout, new_pool)) {
        return nullptr;
    }
    m_pools.push_back(new_pool);
    Pool& p = m_pools.back();
    VkDescriptorSetLayout set_layout = vk_layout.handle();
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = p.pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!NF_VK_CHECK(vkAllocateDescriptorSets(m_device->context().device, &alloc_info, &set))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to allocate descriptor set from fresh pool");
        return nullptr;
    }
    --p.remaining;
    ++m_allocated;
    return std::make_unique<VulkanDescriptorSetPooled>(set);
}

void VulkanDescriptorAllocator::reset() {
    if (!m_device) return;
    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) {
        m_pools.clear();
        m_allocated = 0;
        return;
    }
    // Recycle pools instead of destroying them: vkResetDescriptorPool returns
    // every set to the pool in one call, so steady-state frames allocate with
    // zero vkCreate/vkDestroy churn. Same safety contract as before — the
    // caller must guarantee no submitted work still references these sets
    // (fence discipline), since reset frees them for immediate reuse.
    for (Pool& p : m_pools) {
        if (p.pool != VK_NULL_HANDLE) {
            vkResetDescriptorPool(vk_device, p.pool, 0);
            p.remaining = m_sets_per_pool;
        }
    }
    m_allocated = 0;
    NF_LOG_TRACE(LogCategory::RHI, "Descriptor allocator reset ({} pools recycled)", m_pools.size());
}

// ---------------------------------------------------------------------------
// UploadContext
// ---------------------------------------------------------------------------

VulkanUploadContext::VulkanUploadContext(VulkanDevice* device)
    : m_device(device) {
    m_alive.attach(m_device);
    NF_ASSERT(m_device, "VulkanUploadContext requires a device");
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = m_device->context().graphics_family;
    if (!NF_VK_CHECK(vkCreateCommandPool(m_device->context().device, &pool_info, nullptr, &m_pool))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create upload command pool");
        return;
    }
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = m_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    if (!NF_VK_CHECK(vkAllocateCommandBuffers(m_device->context().device, &alloc_info, &m_cmd))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to allocate upload command buffer");
        vkDestroyCommandPool(m_device->context().device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
        return;
    }
}

VulkanUploadContext::~VulkanUploadContext() {
    if (!m_device) return;
    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;
    if (m_submitted) {
        // The GPU may still be reading this command buffer. The caller is
        // responsible for waiting on the fence before destroying the context.
        // To avoid use-after-free, wait here as a safety net (init-time path).
        // For per-frame async, the caller will have waited on the fence first.
        if (vkDeviceWaitIdle) {
            // Not ideal for performance, but safe for shutdown / init-time uploads
            vkDeviceWaitIdle(vk_device);
        }
    }
    if (m_cmd != VK_NULL_HANDLE && m_pool != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(vk_device, m_pool, 1, &m_cmd);
        m_cmd = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

void VulkanUploadContext::ensure_began() {
    if (m_began) return;
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    NF_VK_CHECK(vkBeginCommandBuffer(m_cmd, &begin_info));
    m_began = true;
}

void VulkanUploadContext::copy_buffer(const Buffer& src, Buffer& dst,
                                      usize src_offset, usize dst_offset, usize size) {
    ensure_began();
    const auto& vk_src = static_cast<const VulkanBuffer&>(src);
    const auto& vk_dst = static_cast<const VulkanBuffer&>(dst);
    VkBufferCopy region{};
    region.srcOffset = static_cast<VkDeviceSize>(src_offset);
    region.dstOffset = static_cast<VkDeviceSize>(dst_offset);
    region.size = static_cast<VkDeviceSize>(size);
    vkCmdCopyBuffer(m_cmd, vk_src.handle(), vk_dst.handle(), 1, &region);
}

void VulkanUploadContext::copy_buffer_to_texture(const Buffer& src, Texture& dst,
                                                 usize buffer_offset,
                                                 u32 tex_x, u32 tex_y, u32 tex_w, u32 tex_h) {
    ensure_began();
    const auto& vk_src = static_cast<const VulkanBuffer&>(src);
    auto& vk_dst = static_cast<VulkanTexture&>(dst);
    const VkImageAspectFlags aspect = aspect_for_format(vk_dst.format());
    const VkImageLayout old_layout = vk_dst.layout();
    if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Transition UNDEFINED / SHADER_READ etc -> TRANSFER_DST
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = old_layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vk_dst.image();
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkAccessFlags src_access = 0;
        VkAccessFlags dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            src_access = 0;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            src_access = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            src_access = VK_ACCESS_MEMORY_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        vkCmdPipelineBarrier(m_cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    VkBufferImageCopy region{};
    region.bufferOffset = static_cast<VkDeviceSize>(buffer_offset);
    region.bufferRowLength = 0;
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

std::unique_ptr<Fence> VulkanUploadContext::submit() {
    if (!m_began) {
        NF_LOG_WARN(LogCategory::RHI, "UploadContext::submit with no work recorded");
        auto fence = m_device->create_fence(false);
        // Signal immediately so caller waiting doesn't hang
        return fence;
    }
    if (m_submitted) {
        NF_LOG_ERROR(LogCategory::RHI, "UploadContext::submit called twice");
        return nullptr;
    }
    NF_VK_CHECK(vkEndCommandBuffer(m_cmd));
    m_began = false;
    m_submitted = true;

    auto fence = m_device->create_fence(false);
    if (!fence) return nullptr;
    auto* vk_fence = static_cast<VulkanFence*>(fence.get());

    VkCommandBuffer cmd = m_cmd;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    VkFence vk_f = vk_fence->handle();
    if (!NF_VK_CHECK(vkQueueSubmit(m_device->context().graphics_queue, 1, &submit_info, vk_f))) {
        return nullptr;
    }
    return fence;
}

} // namespace nf::rhi
