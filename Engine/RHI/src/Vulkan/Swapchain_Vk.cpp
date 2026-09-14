// NF/RHI/src/Vulkan/Swapchain_Vk.cpp — Vulkan swapchain implementation

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <vector>

namespace nf::rhi {

VulkanSwapchain::VulkanSwapchain(VulkanDevice* device, const SwapchainDesc& desc)
    : m_device(device), m_desc(desc) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanSwapchain requires a device");

    const VulkanContext& ctx = m_device->context();

    VkSurfaceCapabilitiesKHR capabilities{};
    if (!NF_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            ctx.physical_device, ctx.surface, &capabilities))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to query surface capabilities");
        return;
    }

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device, ctx.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (format_count > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device, ctx.surface,
                                              &format_count, formats.data());
    }

    u32 present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physical_device, ctx.surface,
                                               &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    if (present_mode_count > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physical_device, ctx.surface,
                                                   &present_mode_count, present_modes.data());
    }

    if (formats.empty()) {
        NF_LOG_ERROR(LogCategory::RHI, "Surface exposes no supported formats");
        return;
    }

    // --- Surface format ----------------------------------------------------
    VkSurfaceFormatKHR surface_format = formats[0];
    const VkFormat requested = to_vk_format(desc.format);

    for (const auto& available : formats) {
        if (available.format == requested &&
            available.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = available;
            break;
        }
    }

    // Fall back to the first usable format if the request is unsupported.
    if (surface_format.format != requested) {
        NF_LOG_WARN(LogCategory::RHI,
            "Requested swapchain format unavailable; using driver format {}",
            static_cast<int>(surface_format.format));
    }

    // --- Present mode ------------------------------------------------------
    const VkPresentModeKHR requested_mode = to_vk_present_mode(desc.present);
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // Guaranteed by spec

    if (std::find(present_modes.begin(), present_modes.end(), requested_mode) != present_modes.end()) {
        present_mode = requested_mode;
    } else {
        NF_LOG_WARN(LogCategory::RHI,
            "Requested present mode unavailable; falling back to FIFO (vsync)");
    }

    // --- Extent ------------------------------------------------------------
    VkExtent2D extent = capabilities.currentExtent;

    // Some drivers report UINT32_MAX, meaning "choose whatever you like".
    if (capabilities.currentExtent.width == u32_max ||
        capabilities.currentExtent.height == u32_max) {
        extent.width = std::clamp(desc.width, capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(desc.height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }

    if (extent.width == 0 || extent.height == 0) {
        NF_LOG_ERROR(LogCategory::RHI, "Surface extent is zero — window is likely minimized");
        return;
    }

    // --- Image count -------------------------------------------------------
    u32 image_count = std::max(desc.image_count, capabilities.minImageCount);
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = ctx.surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const u32 queue_families[2] = { ctx.graphics_family, ctx.present_family };
    if (ctx.graphics_family != ctx.present_family) {
        // Images are shared between two queues, so ownership must be explicit.
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_families;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = VK_NULL_HANDLE;

    if (!NF_VK_CHECK(vkCreateSwapchainKHR(ctx.device, &create_info, nullptr, &m_swapchain))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create swapchain");
        return;
    }

    m_width = extent.width;
    m_height = extent.height;
    m_format = from_vk_format(surface_format.format);

    // --- Wrap the swapchain images ----------------------------------------
    u32 swapchain_image_count = 0;
    vkGetSwapchainImagesKHR(ctx.device, m_swapchain, &swapchain_image_count, nullptr);

    std::vector<VkImage> images(swapchain_image_count);
    vkGetSwapchainImagesKHR(ctx.device, m_swapchain, &swapchain_image_count, images.data());

    m_textures.reserve(images.size());
    for (u32 i = 0; i < images.size(); ++i) {
        auto texture = std::make_unique<VulkanTexture>(
            m_device, images[i], surface_format.format, m_width, m_height, /*owned=*/false);

        if (!texture->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "Failed to create view for swapchain image {}", i);
            release_textures();
            vkDestroySwapchainKHR(ctx.device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
            return;
        }

        texture->set_layout(VK_IMAGE_LAYOUT_UNDEFINED);
        m_textures.push_back(std::move(texture));
    }

    NF_LOG_INFO(LogCategory::RHI, "Swapchain created: {}x{}, {} images, present mode {}",
                m_width, m_height, m_textures.size(), static_cast<int>(present_mode));
}

VulkanSwapchain::~VulkanSwapchain() {
    if (!m_device) return;

    release_textures();

    const VkDevice vk_device = m_device->context().device;
    if (vk_device != VK_NULL_HANDLE && m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

void VulkanSwapchain::release_textures() {
    // Views must be destroyed before the swapchain that owns their images.
    m_textures.clear();
}

u32 VulkanSwapchain::acquire_next_image(const Semaphore& signal_semaphore) {
    const auto* semaphore = static_cast<const VulkanSemaphore*>(&signal_semaphore);
    if (!semaphore || !semaphore->valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "acquire_next_image needs a valid semaphore");
        return u32_max;
    }

    u32 image_index = 0;
    const VkResult result = vkAcquireNextImageKHR(
        m_device->context().device, m_swapchain, u64_max,
        semaphore->handle(), VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Window resized or surface lost — caller must recreate the swapchain.
        return u32_max;
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to acquire swapchain image: {}",
                     vk_result_string(result));
        return u32_max;
    }

    return image_index;
}

void VulkanSwapchain::present(u32 image_index,
                              std::span<const Semaphore* const> wait_semaphores) {
    if (image_index >= m_textures.size()) {
        NF_LOG_ERROR(LogCategory::RHI, "present() got out-of-range image index {}", image_index);
        return;
    }

    std::vector<VkSemaphore> vk_semaphores;
    vk_semaphores.reserve(wait_semaphores.size());

    for (const Semaphore* sem : wait_semaphores) {
        const auto* vk_sem = static_cast<const VulkanSemaphore*>(sem);
        if (!vk_sem || !vk_sem->valid()) {
            NF_LOG_ERROR(LogCategory::RHI, "present() received an invalid semaphore");
            return;
        }
        vk_semaphores.push_back(vk_sem->handle());
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = static_cast<u32>(vk_semaphores.size());
    present_info.pWaitSemaphores = vk_semaphores.data();
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &m_swapchain;
    present_info.pImageIndices = &image_index;

    const VkResult result = vkQueuePresentKHR(m_device->present_queue(), &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // Not fatal: the next acquire will report it and the caller rebuilds.
        return;
    }

    if (result != VK_SUCCESS) {
        NF_LOG_ERROR(LogCategory::RHI, "Present failed: {}", vk_result_string(result));
    }
}

Texture* VulkanSwapchain::get_texture(u32 index) {
    if (index >= m_textures.size()) {
        return nullptr;
    }
    return m_textures[index].get();
}

} // namespace nf::rhi
