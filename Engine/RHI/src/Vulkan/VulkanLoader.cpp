// NF/RHI/Vulkan/VulkanLoader.cpp — Dynamic Vulkan function loading.

#include "VulkanLoader.hpp"

#include <NF/RHI/RHI.hpp> // device_lost() latch
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

namespace {

// --- Per-device dispatch ownership -----------------------------------------
//
// A device-level entry point is only valid for the VkDevice it was resolved
// from, so the shared globals cannot describe two devices at once. These
// snapshots keep the tables each device installed, in the order the devices
// were initialised: the back of the vector is what the globals currently hold,
// and tearing a device down restores whatever is beneath it.
//
// The crash this fixes: a second device is created and destroyed (the RHI
// hardening test does exactly that), and every entry point the first device
// then calls was still pointing into the dead device's dispatch structures —
// an access violation several tests later, in code that had never changed.

/// Named-field snapshot of every instance entry point. Fields are named rather
/// than indexed, so adding one to the X-macro lists above needs no second list
/// to keep in step.
struct InstanceTable {
#define NF_VK_FIELD(fn) PFN_##fn fn = nullptr;
    NF_VK_INSTANCE_FUNCS(NF_VK_FIELD)
    NF_VK_INSTANCE_FUNCS_OPTIONAL(NF_VK_FIELD)
#undef NF_VK_FIELD
};

/// Same, for every device entry point.
struct DeviceTable {
#define NF_VK_FIELD(fn) PFN_##fn fn = nullptr;
    NF_VK_DEVICE_FUNCS(NF_VK_FIELD)
    NF_VK_DEVICE_FUNCS_OPTIONAL(NF_VK_FIELD)
#undef NF_VK_FIELD
};

/// One live device: the instance that keys it, and the tables it installed.
struct LoadedDevice {
    VkInstance instance = VK_NULL_HANDLE;
    InstanceTable instance_fns;
    DeviceTable device_fns;
};

/// Devices in initialisation order. back() is the active one.
std::vector<LoadedDevice> g_devices;

void capture_instance_table(InstanceTable& out) {
#define NF_VK_FIELD(fn) out.fn = fn;
    NF_VK_INSTANCE_FUNCS(NF_VK_FIELD)
    NF_VK_INSTANCE_FUNCS_OPTIONAL(NF_VK_FIELD)
#undef NF_VK_FIELD
}

void install_instance_table(const InstanceTable& in) {
#define NF_VK_FIELD(fn) fn = in.fn;
    NF_VK_INSTANCE_FUNCS(NF_VK_FIELD)
    NF_VK_INSTANCE_FUNCS_OPTIONAL(NF_VK_FIELD)
#undef NF_VK_FIELD
}

void capture_device_table(DeviceTable& out) {
#define NF_VK_FIELD(fn) out.fn = fn;
    NF_VK_DEVICE_FUNCS(NF_VK_FIELD)
    NF_VK_DEVICE_FUNCS_OPTIONAL(NF_VK_FIELD)
#undef NF_VK_FIELD
}

void install_device_table(const DeviceTable& in) {
#define NF_VK_FIELD(fn) fn = in.fn;
    NF_VK_DEVICE_FUNCS(NF_VK_FIELD)
    NF_VK_DEVICE_FUNCS_OPTIONAL(NF_VK_FIELD)
#undef NF_VK_FIELD
}

/// Re-installs the active device's tables (or clears them when none is left).
void apply_active_device() {
    if (g_devices.empty()) {
        const InstanceTable no_instance{};
        const DeviceTable no_device{};
        install_instance_table(no_instance);
        install_device_table(no_device);
        return;
    }
    install_instance_table(g_devices.back().instance_fns);
    install_device_table(g_devices.back().device_fns);
}

/// Finds the entry for `instance`, or nullptr. The instance is the key because
/// a device and its instance are created and destroyed together.
LoadedDevice* find_device(VkInstance instance) {
    for (auto& d : g_devices) {
        if (d.instance == instance) return &d;
    }
    return nullptr;
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
    // A live device's entry points live inside vulkan-1.dll. Freeing the
    // library out from under them is the same use-after-free as the dispatch
    // bug above, so the only safe time to unload is with no device loaded.
    if (!g_devices.empty()) {
        NF_LOG_WARN(LogCategory::RHI,
            "unload_vulkan_loader skipped: {} device(s) still hold resolved entry points",
            g_devices.size());
        return;
    }
    if (g_vulkan_module) {
        FreeLibrary(g_vulkan_module);
        g_vulkan_module = nullptr;
    }
#else
    if (!g_devices.empty()) {
        return;
    }
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

bool vulkan_dispatch_owned_by(VkInstance instance) {
    if (g_devices.empty()) return instance == VK_NULL_HANDLE;
    return g_devices.back().instance == instance;
}

// --- Instance functions ---------------------------------------------------

bool load_instance_functions(VkInstance instance) {
    if (!vkGetInstanceProcAddr) {
        NF_LOG_ERROR(LogCategory::RHI, "load_instance_functions called before loader init");
        return false;
    }

    // Take ownership of the globals for this instance. A re-initialising
    // device keeps its existing slot rather than pushing a second one.
    LoadedDevice* slot = find_device(instance);
    if (!slot) {
        g_devices.push_back(LoadedDevice{});
        slot = &g_devices.back();
        slot->instance = instance;
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

    capture_instance_table(slot->instance_fns);
    NF_LOG_DEBUG(LogCategory::RHI, "Vulkan instance entry points installed ({} device(s) live)",
                 g_devices.size());
    return true;
}

void release_device_functions(VkInstance instance) {
    for (auto it = g_devices.begin(); it != g_devices.end(); ++it) {
        if (it->instance != instance) continue;
        g_devices.erase(it);
        // Whatever device is left beneath this one becomes the active one
        // again; with none left, the globals are cleared so a stray call fails
        // on a null pointer instead of dispatching into a dead device.
        apply_active_device();
        NF_LOG_DEBUG(LogCategory::RHI, "Vulkan entry points released ({} device(s) live)",
                     g_devices.size());
        return;
    }
}

// --- Device functions -----------------------------------------------------

bool load_device_functions(VkDevice device, VkInstance instance) {
    if (!vkGetDeviceProcAddr) {
        NF_LOG_ERROR(LogCategory::RHI, "load_device_functions called before instance init");
        return false;
    }

    LoadedDevice* slot = find_device(instance);
    if (!slot) {
        NF_LOG_ERROR(LogCategory::RHI,
            "load_device_functions: instance was never registered — call load_instance_functions first");
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
    failed |= !load_dev_fn(vkCmdBlitImage,              vkGetDeviceProcAddr, device, "vkCmdBlitImage");
    failed |= !load_dev_fn(vkCmdPushConstants,           vkGetDeviceProcAddr, device, "vkCmdPushConstants");
    failed |= !load_dev_fn(vkCreateDescriptorSetLayout,    vkGetDeviceProcAddr, device, "vkCreateDescriptorSetLayout");
    failed |= !load_dev_fn(vkDestroyDescriptorSetLayout,   vkGetDeviceProcAddr, device, "vkDestroyDescriptorSetLayout");
    failed |= !load_dev_fn(vkCreateDescriptorPool,         vkGetDeviceProcAddr, device, "vkCreateDescriptorPool");
    failed |= !load_dev_fn(vkDestroyDescriptorPool,        vkGetDeviceProcAddr, device, "vkDestroyDescriptorPool");
    // Adding a function to NF_VK_DEVICE_FUNCS in VulkanLoader.hpp only declares
    // and zero-initialises the pointer — this hand-written list is what actually
    // resolves it. A missing entry here leaves a null pointer that crashes on
    // first call instead of failing to load.
    failed |= !load_dev_fn(vkResetDescriptorPool,          vkGetDeviceProcAddr, device, "vkResetDescriptorPool");
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
    load_opt_dev_fn(vkCmdBeginDebugUtilsLabelEXT,  vkGetDeviceProcAddr, device, "vkCmdBeginDebugUtilsLabelEXT");
    load_opt_dev_fn(vkCmdEndDebugUtilsLabelEXT,    vkGetDeviceProcAddr, device, "vkCmdEndDebugUtilsLabelEXT");
    load_opt_dev_fn(vkCmdInsertDebugUtilsLabelEXT, vkGetDeviceProcAddr, device, "vkCmdInsertDebugUtilsLabelEXT");

    capture_device_table(slot->device_fns);
    NF_LOG_DEBUG(LogCategory::RHI, "Vulkan device entry points installed ({} device(s) live)",
                 g_devices.size());
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

    // A lost device is terminal and latches: every later submit fails too, so
    // the caller must be able to stop instead of looping on error lines.
    if (result == VK_ERROR_DEVICE_LOST) {
        nf::rhi::record_device_lost();
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
