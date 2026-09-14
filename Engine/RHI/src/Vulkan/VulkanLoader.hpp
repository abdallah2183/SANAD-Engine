#pragma once

// NF/RHI/Vulkan/VulkanLoader.hpp — Dynamic Vulkan function loading.
//
// NOVAForge loads every Vulkan entry point at runtime instead of linking
// against vulkan-1.lib. Two reasons this matters:
//
//   1. Dedicated-server / headless builds (design doc §"Dedicated server
//      buildable without GPU") must be able to link and boot with no Vulkan
//      loader present on the machine. Static linking would make vulkan-1.dll a
//      load-time dependency of the whole engine.
//   2. We control exactly which extensions we probe for, so optional ones
//      (debug utils, object naming) degrade to no-ops instead of failing.
//
// The pointers below live in VulkanLoader.cpp and are shared by every Vulkan
// translation unit in this backend.

#include <NF/Core/Types.hpp>

#ifndef VK_NO_PROTOTYPES
    #define VK_NO_PROTOTYPES
#endif

#ifdef _WIN32
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR
    #endif
#endif

#include <vulkan/vulkan.h>

namespace nf::rhi {

// ---------------------------------------------------------------------------
// Optional debug-utility functions.
// Loaded only when the VK_EXT_debug_utils instance extension is present; they
// are allowed to remain null and every caller must null-check before use.
// ---------------------------------------------------------------------------
#define NF_VK_INSTANCE_FUNCS_OPTIONAL(X) \
    X(vkCreateDebugUtilsMessengerEXT)    \
    X(vkDestroyDebugUtilsMessengerEXT)

#define NF_VK_DEVICE_FUNCS_OPTIONAL(X)  \
    X(vkSetDebugUtilsObjectNameEXT)     \
    X(vkCmdBeginDebugUtilsLabelEXT)     \
    X(vkCmdEndDebugUtilsLabelEXT)       \
    X(vkCmdInsertDebugUtilsLabelEXT)

// ---------------------------------------------------------------------------
// Loader-level: resolved straight off the module (vkGetInstanceProcAddr is the
// only symbol we fetch with the platform's GetProcAddress/dlsym).
// ---------------------------------------------------------------------------
// The first three are resolved with a NULL instance, which is what lets us
// query layers/extensions *before* the instance exists.
#define NF_VK_LOADER_FUNCS(X)                  \
    X(vkGetInstanceProcAddr)                   \
    X(vkCreateInstance)                        \
    X(vkEnumerateInstanceLayerProperties)      \
    X(vkEnumerateInstanceExtensionProperties)

// ---------------------------------------------------------------------------
// Instance-level: resolved with vkGetInstanceProcAddr(instance, ...).
// ---------------------------------------------------------------------------
#if defined(_WIN32)
    #define NF_VK_SURFACE_FUNCS(X) X(vkCreateWin32SurfaceKHR)
#elif defined(__linux__)
    #define NF_VK_SURFACE_FUNCS(X) X(vkCreateXcbSurfaceKHR)
#else
    #define NF_VK_SURFACE_FUNCS(X)
#endif

#define NF_VK_INSTANCE_FUNCS(X)                          \
    X(vkDestroyInstance)                                 \
    X(vkGetDeviceProcAddr)                               \
    X(vkEnumeratePhysicalDevices)                        \
    X(vkGetPhysicalDeviceProperties)                     \
    X(vkGetPhysicalDeviceFeatures)                       \
    X(vkGetPhysicalDeviceMemoryProperties)               \
    X(vkGetPhysicalDeviceQueueFamilyProperties)          \
    X(vkGetPhysicalDeviceFormatProperties)               \
    X(vkEnumerateDeviceExtensionProperties)              \
    X(vkDestroySurfaceKHR)                               \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)              \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)         \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)              \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)         \
    X(vkCreateDevice)                                    \
    NF_VK_SURFACE_FUNCS(X)

// ---------------------------------------------------------------------------
// Device-level: resolved with vkGetDeviceProcAddr(device, ...).
// ---------------------------------------------------------------------------
#define NF_VK_DEVICE_FUNCS(X)                \
    X(vkDestroyDevice)                       \
    X(vkGetDeviceQueue)                      \
    X(vkDeviceWaitIdle)                      \
    X(vkQueueWaitIdle)                       \
    X(vkQueueSubmit)                         \
    X(vkQueuePresentKHR)                     \
    X(vkCreateCommandPool)                   \
    X(vkDestroyCommandPool)                  \
    X(vkAllocateCommandBuffers)              \
    X(vkFreeCommandBuffers)                  \
    X(vkResetCommandBuffer)                  \
    X(vkBeginCommandBuffer)                  \
    X(vkEndCommandBuffer)                    \
    X(vkAllocateMemory)                      \
    X(vkFreeMemory)                          \
    X(vkMapMemory)                           \
    X(vkUnmapMemory)                         \
    X(vkFlushMappedMemoryRanges)             \
    X(vkInvalidateMappedMemoryRanges)        \
    X(vkCreateBuffer)                        \
    X(vkDestroyBuffer)                       \
    X(vkGetBufferMemoryRequirements)         \
    X(vkBindBufferMemory)                    \
    X(vkCreateImage)                         \
    X(vkDestroyImage)                        \
    X(vkGetImageMemoryRequirements)          \
    X(vkBindImageMemory)                     \
    X(vkCreateImageView)                     \
    X(vkDestroyImageView)                    \
    X(vkCreateShaderModule)                  \
    X(vkDestroyShaderModule)                 \
    X(vkCreatePipelineLayout)                \
    X(vkDestroyPipelineLayout)               \
    X(vkCreateGraphicsPipelines)             \
    X(vkDestroyPipeline)                     \
    X(vkCreateRenderPass)                    \
    X(vkDestroyRenderPass)                   \
    X(vkCreateFramebuffer)                   \
    X(vkDestroyFramebuffer)                  \
    X(vkCreateSemaphore)                     \
    X(vkDestroySemaphore)                    \
    X(vkCreateFence)                         \
    X(vkDestroyFence)                        \
    X(vkWaitForFences)                       \
    X(vkResetFences)                         \
    X(vkCreateSwapchainKHR)                  \
    X(vkDestroySwapchainKHR)                 \
    X(vkGetSwapchainImagesKHR)               \
    X(vkAcquireNextImageKHR)                 \
    X(vkCmdBeginRenderPass)                  \
    X(vkCmdEndRenderPass)                    \
    X(vkCmdBindPipeline)                     \
    X(vkCmdBindVertexBuffers)                \
    X(vkCmdBindIndexBuffer)                  \
    X(vkCmdSetViewport)                      \
    X(vkCmdSetScissor)                       \
    X(vkCmdDraw)                             \
    X(vkCmdDrawIndexed)                      \
    X(vkCmdCopyBuffer)                       \
    X(vkCmdCopyBufferToImage)                \
    X(vkCmdCopyImageToBuffer)                \
    X(vkCmdBlitImage)                        \
    X(vkCmdPipelineBarrier)                  \
    X(vkCmdPushConstants)                    \
    X(vkCreateDescriptorSetLayout)           \
    X(vkDestroyDescriptorSetLayout)          \
    X(vkCreateDescriptorPool)                \
    X(vkDestroyDescriptorPool)               \
    X(vkAllocateDescriptorSets)             \
    X(vkFreeDescriptorSets)                 \
    X(vkUpdateDescriptorSets)               \
    X(vkCreateSampler)                      \
    X(vkDestroySampler)                     \
    X(vkCmdBindDescriptorSets)

// --- Declarations (definitions live in VulkanLoader.cpp) -------------------
#define NF_VK_DECLARE(fn) extern PFN_##fn fn;
NF_VK_LOADER_FUNCS(NF_VK_DECLARE)
NF_VK_INSTANCE_FUNCS(NF_VK_DECLARE)
NF_VK_INSTANCE_FUNCS_OPTIONAL(NF_VK_DECLARE)
NF_VK_DEVICE_FUNCS(NF_VK_DECLARE)
NF_VK_DEVICE_FUNCS_OPTIONAL(NF_VK_DECLARE)
#undef NF_VK_DECLARE

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

/// Loads the platform Vulkan loader (vulkan-1.dll) and resolves
/// vkGetInstanceProcAddr. Returns false when no Vulkan loader is installed —
/// callers must treat that as "no GPU available", not as a fatal error.
bool load_vulkan_loader();

/// Resolves all instance-level entry points. Requires a valid VkInstance.
/// Optional debug functions are allowed to fail silently.
bool load_instance_functions(VkInstance instance);

/// Resolves all device-level entry points. Requires a valid VkDevice.
bool load_device_functions(VkDevice device);

/// Releases the platform Vulkan loader module.
void unload_vulkan_loader();

/// True once load_vulkan_loader() has succeeded.
bool is_vulkan_loader_loaded();

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

const char* vk_result_string(VkResult result);

/// Logs a descriptive error including file/line and returns true on success.
bool vk_check_impl(VkResult result, const char* expr, const char* file, u32 line);

#define NF_VK_CHECK(expr) \
    ::nf::rhi::vk_check_impl((expr), #expr, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Debug object naming (no-ops when VK_EXT_debug_utils is unavailable)
// ---------------------------------------------------------------------------

void vk_set_object_name(VkDevice device, VkObjectType type, u64 handle, const char* name);

/// GPU debugging labels. These show up in RenderDoc / Nsight / Radeon GPU
/// Profiler and are the backbone of the frame profiler we will build later.
/// Both are no-ops when VK_EXT_debug_utils is unavailable.
void vk_begin_label(VkCommandBuffer cmd, const char* name, const float* color = nullptr);
void vk_end_label(VkCommandBuffer cmd);

/// RAII wrapper that closes the label automatically at scope exit.
class VkScopedLabel {
public:
    VkScopedLabel(VkCommandBuffer cmd, const char* name, const float* color = nullptr)
        : m_cmd(cmd) {
        vk_begin_label(cmd, name, color);
    }
    ~VkScopedLabel() { vk_end_label(m_cmd); }

    VkScopedLabel(const VkScopedLabel&) = delete;
    VkScopedLabel& operator=(const VkScopedLabel&) = delete;

private:
    VkCommandBuffer m_cmd;
};

} // namespace nf::rhi
