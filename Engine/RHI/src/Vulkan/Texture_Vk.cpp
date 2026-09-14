// NF/RHI/src/Vulkan/Texture_Vk.cpp — Vulkan texture implementation

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <atomic>

namespace nf::rhi {

namespace {

std::atomic<u64>& texture_serial_counter() {
    static std::atomic<u64> counter{1}; // 0 = "no serial" sentinel, never issued
    return counter;
}

} // namespace

u64 next_texture_serial() {
    return texture_serial_counter().fetch_add(1, std::memory_order_relaxed);
}

namespace {

bool is_depth_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

VkImageAspectFlags aspect_for(VkFormat format) {
    if (is_depth_format(format)) {
        // Combined depth/stencil formats carry both aspects.
        if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

} // namespace

VulkanTexture::VulkanTexture(VulkanDevice* device, const TextureDesc& desc)
    : m_device(device),
      m_vk_format(to_vk_format(desc.format)),
      m_format(desc.format),
      m_width(desc.width),
      m_height(desc.height),
      m_mip_levels(desc.mip_levels > 0 ? desc.mip_levels : 1),
      m_owned(true) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanTexture requires a device");
    NF_ASSERT(desc.width > 0 && desc.height > 0, "Invalid texture dimensions");
    NF_ASSERT(m_vk_format != VK_FORMAT_UNDEFINED, "Invalid texture format");

    const VkDevice vk_device = m_device->context().device;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent = { desc.width, desc.height, 1 };
    image_info.mipLevels = m_mip_levels;
    image_info.arrayLayers = desc.array_layers > 0 ? desc.array_layers : 1;
    image_info.format = m_vk_format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = to_vk_image_usage(desc.usage);
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;

    if (!NF_VK_CHECK(vkCreateImage(vk_device, &image_info, nullptr, &m_image))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create image {}x{}", desc.width, desc.height);
        return;
    }

    VkMemoryRequirements mem_reqs{};
    vkGetImageMemoryRequirements(vk_device, m_image, &mem_reqs);

    const u32 memory_type = m_device->find_memory_type(
        mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == u32_max) {
        NF_LOG_ERROR(LogCategory::RHI, "No device-local memory type for image");
        release();
        return;
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = memory_type;

    if (!NF_VK_CHECK(vkAllocateMemory(vk_device, &alloc_info, nullptr, &m_memory))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to allocate image memory");
        release();
        return;
    }

    if (!NF_VK_CHECK(vkBindImageMemory(vk_device, m_image, m_memory, 0))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to bind image memory");
        release();
        return;
    }

    m_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (!create_view(is_depth_format(m_vk_format))) {
        release();
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Texture created: {}x{}", desc.width, desc.height);
}

// Swapchain-owned image wrapper: we create a view but never own the image.
VulkanTexture::VulkanTexture(VulkanDevice* device, VkImage image, VkFormat format,
                             u32 width, u32 height, bool owned)
    : m_device(device),
      m_image(image),
      m_vk_format(format),
      m_format(from_vk_format(format)),
      m_width(width),
      m_height(height),
      m_layout(VK_IMAGE_LAYOUT_UNDEFINED),
      m_owned(owned) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanTexture requires a device");
    NF_ASSERT(image != VK_NULL_HANDLE, "VulkanTexture requires a valid image");

    if (!create_view(is_depth_format(format))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create view for external image");
    }
}

VulkanTexture::~VulkanTexture() {
    release();
}

bool VulkanTexture::create_view(bool is_depth) {
    const VkImageAspectFlags aspect = is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = m_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = m_vk_format;
    view_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
    };
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = m_mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    return NF_VK_CHECK(
        vkCreateImageView(m_device->context().device, &view_info, nullptr, &m_view));
}

void VulkanTexture::release() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;

    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk_device, m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }

    // Swapchain images are owned by the swapchain and destroyed with it.
    if (m_owned) {
        if (m_image != VK_NULL_HANDLE) {
            vkDestroyImage(vk_device, m_image, nullptr);
            m_image = VK_NULL_HANDLE;
        }
        if (m_memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk_device, m_memory, nullptr);
            m_memory = VK_NULL_HANDLE;
        }
    }
}

} // namespace nf::rhi
