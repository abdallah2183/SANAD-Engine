// NF/RHI/Vulkan/VulkanLoader.cpp — Dynamic Vulkan function loading.

#include "VulkanLoader.hpp"

#include <NF/Core/Logger.hpp>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace nf::rhi {

// --- Definitions of every shared function pointer --------------------------
#define NF_VK_DEFINE(fn) PFN_##fn fn = nullptr;
NF_VK_LOADER_FUNCS(NF_VK_DEFINE)
NF_VK_INSTANCE_FUNCS(NF_VK_DEFINE)
NF_VK_INSTANCE_FUNCS_OPTIONAL(NF_VK_DEFINE)
NF_VK_DEVICE_FUNCS(NF_VK_DEFINE)
NF_VK_DEVICE_FUNCS_OPTIONAL(NF_VK_DEFINE)
#undef NF_VK_DEFINE

namespace {

#ifdef _WIN32
    HMODULE g_vulkan_module = nullptr;
#else
    void* g_vulkan_module = nullptr;
#endif

// Generic loader helpers — these replace the X-macro loading patterns that
// broke under /Zc:preprocessor due to ## token-pasting edge cases.

template <typename Fn>
bool load_fn(Fn& dst, PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
    dst = reinterpret_cast<Fn>(gipa(instance, name));
    if (!dst) {
        NF_LOG_ERROR(LogCategory::RHI, "Missing Vulkan function: {}", name);
        return false;
    }
    return true;
}

template <typename Fn>
bool load_dev_fn(Fn& dst, PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char* name) {
    dst = reinterpret_cast<Fn>(gdpa(device, name));
    if (!dst) {
        NF_LOG_ERROR(LogCategory::RHI, "Missing Vulkan device function: {}", name);
        return false;
    }
    return true;
}

/// Like load_dev_fn but tolerant of absence: extension entry points are NULL
/// on a logical device created without the extension (headless devices have
/// no VK_KHR_swapchain). Calling such a function without enabling its
/// extension is invalid anyway, so a null pointer here is correct state, not
/// a load failure.
template <typename Fn>
bool load_dev_fn_optional(Fn& dst, PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char* name) {
    dst = reinterpret_cast<Fn>(gdpa(device, name));
    if (!dst) {
        NF_LOG_DEBUG(LogCategory::RHI, "Optional Vulkan device function absent: {}", name);
        return false;
    }
    return true;
}

template <typename Fn>
void load_opt_fn(Fn& dst, PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
    dst = reinterpret_cast<Fn>(gipa(instance, name));
    if (!dst) {
        NF_LOG_DEBUG(LogCategory::RHI, "Optional Vulkan function unavailable: {}", name);
    }
}

template <typename Fn>
void load_opt_dev_fn(Fn& dst, PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char* name) {
    dst = reinterpret_cast<Fn>(gdpa(device, name));
    if (!dst) {
        NF_LOG_DEBUG(LogCategory::RHI, "Optional Vulkan device function unavailable: {}", name);
    }
}

} // namespace

// --- Loader ---------------------------------------------------------------

bool load_vulkan_loader() {
    if (g_vulkan_module) {
        return true; // Already loaded
    }

#ifdef _WIN32
    g_vulkan_module = LoadLibraryW(L"vulkan-1.dll");
    if (!g_vulkan_module) {
        NF_LOG_WARN(LogCategory::RHI,
            "vulkan-1.dll not found — no Vulkan-capable driver installed");
        return false;
    }

    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_vulkan_module, "vkGetInstanceProcAddr"));
#else
    g_vulkan_module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_vulkan_module) {
        g_vulkan_module = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!g_vulkan_module) {
        NF_LOG_WARN(LogCategory::RHI, "libvulkan not found");
        return false;
    }

    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(g_vulkan_module, "vkGetInstanceProcAddr"));
#endif

    if (!vkGetInstanceProcAddr) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to resolve vkGetInstanceProcAddr");
        unload_vulkan_loader();
        return false;
    }

    // Core 1.0 entry points that are legal to resolve with a NULL instance.
    bool failed = false;

    failed |= !load_fn(vkCreateInstance,                    vkGetInstanceProcAddr, nullptr, "vkCreateInstance");
    failed |= !load_fn(vkEnumerateInstanceLayerProperties,   vkGetInstanceProcAddr, nullptr, "vkEnumerateInstanceLayerProperties");
    failed |= !load_fn(vkEnumerateInstanceExtensionProperties, vkGetInstanceProcAddr, nullptr, "vkEnumerateInstanceExtensionProperties");

    if (failed) {
        unload_vulkan_loader();
        return false;
    }

    NF_LOG_DEBUG(LogCategory::RHI, "Vulkan loader loaded");
    return true;
}

void unload_vulkan_loader() {
#ifdef _WIN32
    if (g_vulkan_module) {
        FreeLibrary(g_vulkan_module);
        g_vulkan_module = nullptr;
    }
#else
    if (g_vulkan_module) {
        dlclose(g_vulkan_module);
        g_vulkan_module = nullptr;
    }
#endif
    vkGetInstanceProcAddr = nullptr;
}

bool is_vulkan_loader_loaded() {
    return g_vulkan_module != nullptr;
}

// --- Instance functions ---------------------------------------------------

bool load_instance_functions(VkInstance instance) {
    if (!vkGetInstanceProcAddr) {
        NF_LOG_ERROR(LogCategory::RHI, "load_instance_functions called before loader init");
        return false;
    }

    bool failed = false;

    failed |= !load_fn(vkDestroyInstance,                        vkGetInstanceProcAddr, instance, "vkDestroyInstance");
    failed |= !load_fn(vkGetDeviceProcAddr,                       vkGetInstanceProcAddr, instance, "vkGetDeviceProcAddr");
    failed |= !load_fn(vkEnumeratePhysicalDevices,                vkGetInstanceProcAddr, instance, "vkEnumeratePhysicalDevices");
    failed |= !load_fn(vkGetPhysicalDeviceProperties,             vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceProperties");
    failed |= !load_fn(vkGetPhysicalDeviceFeatures,               vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceFeatures");
    failed |= !load_fn(vkGetPhysicalDeviceMemoryProperties,        vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceMemoryProperties");
    failed |= !load_fn(vkGetPhysicalDeviceQueueFamilyProperties,   vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    failed |= !load_fn(vkGetPhysicalDeviceFormatProperties,        vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceFormatProperties");
    failed |= !load_fn(vkEnumerateDeviceExtensionProperties,       vkGetInstanceProcAddr, instance, "vkEnumerateDeviceExtensionProperties");
    failed |= !load_fn(vkDestroySurfaceKHR,                      vkGetInstanceProcAddr, instance, "vkDestroySurfaceKHR");
    failed |= !load_fn(vkGetPhysicalDeviceSurfaceSupportKHR,      vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    failed |= !load_fn(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    failed |= !load_fn(vkGetPhysicalDeviceSurfaceFormatsKHR,      vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    failed |= !load_fn(vkGetPhysicalDeviceSurfacePresentModesKHR, vkGetInstanceProcAddr, instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    failed |= !load_fn(vkCreateDevice,                           vkGetInstanceProcAddr, instance, "vkCreateDevice");
#ifdef _WIN32
    failed |= !load_fn(vkCreateWin32SurfaceKHR,                   vkGetInstanceProcAddr, instance, "vkCreateWin32SurfaceKHR");
#endif

    if (failed) {
        return false;
    }

    // Optional: debug utils. Absence is normal on release drivers.
    load_opt_fn(vkCreateDebugUtilsMessengerEXT,    vkGetInstanceProcAddr, instance, "vkCreateDebugUtilsMessengerEXT");
    load_opt_fn(vkDestroyDebugUtilsMessengerEXT,   vkGetInstanceProcAddr, instance, "vkDestroyDebugUtilsMessengerEXT");

    return true;
}

// --- Device functions -----------------------------------------------------

bool load_device_functions(VkDevice device) {
    if (!vkGetDeviceProcAddr) {
        NF_LOG_ERROR(LogCategory::RHI, "load_device_functions called before instance init");
        return false;
    }

    bool failed = false;

    failed |= !load_dev_fn(vkDestroyDevice,              vkGetDeviceProcAddr, device, "vkDestroyDevice");
    failed |= !load_dev_fn(vkGetDeviceQueue,             vkGetDeviceProcAddr, device, "vkGetDeviceQueue");
    failed |= !load_dev_fn(vkDeviceWaitIdle,             vkGetDeviceProcAddr, device, "vkDeviceWaitIdle");
    failed |= !load_dev_fn(vkQueueWaitIdle,              vkGetDeviceProcAddr, device, "vkQueueWaitIdle");
    failed |= !load_dev_fn(vkQueueSubmit,               vkGetDeviceProcAddr, device, "vkQueueSubmit");
    load_dev_fn_optional(vkQueuePresentKHR, vkGetDeviceProcAddr, device, "vkQueuePresentKHR");
    failed |= !load_dev_fn(vkCreateCommandPool,          vkGetDeviceProcAddr, device, "vkCreateCommandPool");
    failed |= !load_dev_fn(vkDestroyCommandPool,         vkGetDeviceProcAddr, device, "vkDestroyCommandPool");
    failed |= !load_dev_fn(vkAllocateCommandBuffers,     vkGetDeviceProcAddr, device, "vkAllocateCommandBuffers");
    failed |= !load_dev_fn(vkFreeCommandBuffers,         vkGetDeviceProcAddr, device, "vkFreeCommandBuffers");
    failed |= !load_dev_fn(vkResetCommandBuffer,        vkGetDeviceProcAddr, device, "vkResetCommandBuffer");
    failed |= !load_dev_fn(vkBeginCommandBuffer,        vkGetDeviceProcAddr, device, "vkBeginCommandBuffer");
    failed |= !load_dev_fn(vkEndCommandBuffer,          vkGetDeviceProcAddr, device, "vkEndCommandBuffer");
    failed |= !load_dev_fn(vkAllocateMemory,             vkGetDeviceProcAddr, device, "vkAllocateMemory");
    failed |= !load_dev_fn(vkFreeMemory,                vkGetDeviceProcAddr, device, "vkFreeMemory");
    failed |= !load_dev_fn(vkMapMemory,                 vkGetDeviceProcAddr, device, "vkMapMemory");
    failed |= !load_dev_fn(vkUnmapMemory,               vkGetDeviceProcAddr, device, "vkUnmapMemory");
    failed |= !load_dev_fn(vkFlushMappedMemoryRanges,    vkGetDeviceProcAddr, device, "vkFlushMappedMemoryRanges");
    failed |= !load_dev_fn(vkInvalidateMappedMemoryRanges, vkGetDeviceProcAddr, device, "vkInvalidateMappedMemoryRanges");
    failed |= !load_dev_fn(vkCreateBuffer,              vkGetDeviceProcAddr, device, "vkCreateBuffer");
    failed |= !load_dev_fn(vkDestroyBuffer,             vkGetDeviceProcAddr, device, "vkDestroyBuffer");
    failed |= !load_dev_fn(vkGetBufferMemoryRequirements, vkGetDeviceProcAddr, device, "vkGetBufferMemoryRequirements");
    failed |= !load_dev_fn(vkBindBufferMemory,           vkGetDeviceProcAddr, device, "vkBindBufferMemory");
    failed |= !load_dev_fn(vkCreateImage,                vkGetDeviceProcAddr, device, "vkCreateImage");
    failed |= !load_dev_fn(vkDestroyImage,               vkGetDeviceProcAddr, device, "vkDestroyImage");
    failed |= !load_dev_fn(vkGetImageMemoryRequirements,  vkGetDeviceProcAddr, device, "vkGetImageMemoryRequirements");
    failed |= !load_dev_fn(vkBindImageMemory,            vkGetDeviceProcAddr, device, "vkBindImageMemory");
    failed |= !load_dev_fn(vkCreateImageView,            vkGetDeviceProcAddr, device, "vkCreateImageView");
    failed |= !load_dev_fn(vkDestroyImageView,           vkGetDeviceProcAddr, device, "vkDestroyImageView");
    failed |= !load_dev_fn(vkCreateShaderModule,         vkGetDeviceProcAddr, device, "vkCreateShaderModule");
    failed |= !load_dev_fn(vkDestroyShaderModule,        vkGetDeviceProcAddr, device, "vkDestroyShaderModule");
    failed |= !load_dev_fn(vkCreatePipelineLayout,       vkGetDeviceProcAddr, device, "vkCreatePipelineLayout");
    failed |= !load_dev_fn(vkDestroyPipelineLayout,      vkGetDeviceProcAddr, device, "vkDestroyPipelineLayout");
    failed |= !load_dev_fn(vkCreateGraphicsPipelines,     vkGetDeviceProcAddr, device, "vkCreateGraphicsPipelines");
    failed |= !load_dev_fn(vkDestroyPipeline,            vkGetDeviceProcAddr, device, "vkDestroyPipeline");
    failed |= !load_dev_fn(vkCreateRenderPass,            vkGetDeviceProcAddr, device, "vkCreateRenderPass");
    failed |= !load_dev_fn(vkDestroyRenderPass,          vkGetDeviceProcAddr, device, "vkDestroyRenderPass");
    failed |= !load_dev_fn(vkCreateFramebuffer,          vkGetDeviceProcAddr, device, "vkCreateFramebuffer");
    failed |= !load_dev_fn(vkDestroyFramebuffer,         vkGetDeviceProcAddr, device, "vkDestroyFramebuffer");
    failed |= !load_dev_fn(vkCreateSemaphore,            vkGetDeviceProcAddr, device, "vkCreateSemaphore");
    failed |= !load_dev_fn(vkDestroySemaphore,           vkGetDeviceProcAddr, device, "vkDestroySemaphore");
    failed |= !load_dev_fn(vkCreateFence,                vkGetDeviceProcAddr, device, "vkCreateFence");
    failed |= !load_dev_fn(vkDestroyFence,               vkGetDeviceProcAddr, device, "vkDestroyFence");
    failed |= !load_dev_fn(vkWaitForFences,               vkGetDeviceProcAddr, device, "vkWaitForFences");
    failed |= !load_dev_fn(vkResetFences,                vkGetDeviceProcAddr, device, "vkResetFences");
    load_dev_fn_optional(vkCreateSwapchainKHR, vkGetDeviceProcAddr, device, "vkCreateSwapchainKHR");
    load_dev_fn_optional(vkDestroySwapchainKHR, vkGetDeviceProcAddr, device, "vkDestroySwapchainKHR");
    load_dev_fn_optional(vkGetSwapchainImagesKHR, vkGetDeviceProcAddr, device, "vkGetSwapchainImagesKHR");
    load_dev_fn_optional(vkAcquireNextImageKHR, vkGetDeviceProcAddr, device, "vkAcquireNextImageKHR");
    failed |= !load_dev_fn(vkCmdBeginRenderPass,          vkGetDeviceProcAddr, device, "vkCmdBeginRenderPass");
    failed |= !load_dev_fn(vkCmdEndRenderPass,            vkGetDeviceProcAddr, device, "vkCmdEndRenderPass");
    failed |= !load_dev_fn(vkCmdBindPipeline,            vkGetDeviceProcAddr, device, "vkCmdBindPipeline");
    failed |= !load_dev_fn(vkCmdBindVertexBuffers,       vkGetDeviceProcAddr, device, "vkCmdBindVertexBuffers");
    failed |= !load_dev_fn(vkCmdBindIndexBuffer,          vkGetDeviceProcAddr, device, "vkCmdBindIndexBuffer");
    failed |= !load_dev_fn(vkCmdSetViewport,             vkGetDeviceProcAddr, device, "vkCmdSetViewport");
    failed |= !load_dev_fn(vkCmdSetScissor,              vkGetDeviceProcAddr, device, "vkCmdSetScissor");
    failed |= !load_dev_fn(vkCmdDraw,                    vkGetDeviceProcAddr, device, "vkCmdDraw");
    failed |= !load_dev_fn(vkCmdDrawIndexed,             vkGetDeviceProcAddr, device, "vkCmdDrawIndexed");
    failed |= !load_dev_fn(vkCmdCopyBuffer,              vkGetDeviceProcAddr, device, "vkCmdCopyBuffer");
    failed |= !load_dev_fn(vkCmdCopyBufferToImage,       vkGetDeviceProcAddr, device, "vkCmdCopyBufferToImage");
    failed |= !load_dev_fn(vkCmdCopyImageToBuffer,       vkGetDeviceProcAddr, device, "vkCmdCopyImageToBuffer");
    failed |= !load_dev_fn(vkCmdPipelineBarrier,         vkGetDeviceProcAddr, device, "vkCmdPipelineBarrier");
    failed |= !load_dev_fn(vkCmdPushConstants,           vkGetDeviceProcAddr, device, "vkCmdPushConstants");
    failed |= !load_dev_fn(vkCreateDescriptorSetLayout,    vkGetDeviceProcAddr, device, "vkCreateDescriptorSetLayout");
    failed |= !load_dev_fn(vkDestroyDescriptorSetLayout,   vkGetDeviceProcAddr, device, "vkDestroyDescriptorSetLayout");
    failed |= !load_dev_fn(vkCreateDescriptorPool,         vkGetDeviceProcAddr, device, "vkCreateDescriptorPool");
    failed |= !load_dev_fn(vkDestroyDescriptorPool,        vkGetDeviceProcAddr, device, "vkDestroyDescriptorPool");
    failed |= !load_dev_fn(vkAllocateDescriptorSets,      vkGetDeviceProcAddr, device, "vkAllocateDescriptorSets");
    failed |= !load_dev_fn(vkFreeDescriptorSets,           vkGetDeviceProcAddr, device, "vkFreeDescriptorSets");
    failed |= !load_dev_fn(vkUpdateDescriptorSets,        vkGetDeviceProcAddr, device, "vkUpdateDescriptorSets");
    failed |= !load_dev_fn(vkCreateSampler,               vkGetDeviceProcAddr, device, "vkCreateSampler");
    failed |= !load_dev_fn(vkDestroySampler,              vkGetDeviceProcAddr, device, "vkDestroySampler");
    failed |= !load_dev_fn(vkCmdBindDescriptorSets,       vkGetDeviceProcAddr, device, "vkCmdBindDescriptorSets");

    if (failed) {
        return false;
    }

    // Optional: object naming + command buffer labels (profiling aids).
    load_opt_dev_fn(vkSetDebugUtilsObjectNameEXT,   vkGetDeviceProcAddr, device, "vkSetDebugUtilsObjectNameEXT");
    load_opt_dev_fn(vkCmdBeginDebugUtilsLabelEXT,   vkGetDeviceProcAddr, device, "vkCmdBeginDebugUtilsLabelEXT");
    load_opt_dev_fn(vkCmdEndDebugUtilsLabelEXT,     vkGetDeviceProcAddr, device, "vkCmdEndDebugUtilsLabelEXT");
    load_opt_dev_fn(vkCmdInsertDebugUtilsLabelEXT,  vkGetDeviceProcAddr, device, "vkCmdInsertDebugUtilsLabelEXT");

    return true;
}

// --- Error helpers --------------------------------------------------------

const char* vk_result_string(VkResult result) {
    switch (result) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_EVENT_SET:                      return "VK_EVENT_SET";
        case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY:       return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE:  return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT:    return "VK_ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_INVALID_SHADER_NV:        return "VK_ERROR_INVALID_SHADER_NV";
        default:                                return "VK_ERROR_UNKNOWN";
    }
}

bool vk_check_impl(VkResult result, const char* expr, const char* file, u32 line) {
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return true;
    }

    NF_LOG_ERROR(LogCategory::RHI, "Vulkan call failed: {} returned {} ({}:{})",
                 expr, vk_result_string(result), file, line);
    return false;
}

// --- Debug naming ---------------------------------------------------------

void vk_set_object_name(VkDevice device, VkObjectType type, u64 handle, const char* name) {
    if (!vkSetDebugUtilsObjectNameEXT || !name || !handle) {
        return;
    }

    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;

    vkSetDebugUtilsObjectNameEXT(device, &info);
}

} // namespace nf::rhi
