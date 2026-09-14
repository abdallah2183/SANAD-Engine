// NF/RHI/src/Vulkan/VulkanContext.cpp — GPU debug label helpers.
//
// These wrap VK_EXT_debug_utils. They are the hooks a frame profiler and
// external capture tools (RenderDoc, Nsight, RGP) read, so they are compiled
// into all builds but become no-ops when the driver lacks the extension.

#include "VulkanLoader.hpp"

#include <NF/Core/Logger.hpp>

#include <cstring>

namespace nf::rhi {

void vk_begin_label(VkCommandBuffer cmd, const char* name, const float* color) {
    if (!vkCmdBeginDebugUtilsLabelEXT || !cmd || !name) {
        return;
    }

    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;

    if (color) {
        label.color[0] = color[0];
        label.color[1] = color[1];
        label.color[2] = color[2];
        label.color[3] = color[3];
    } else {
        label.color[0] = label.color[1] = label.color[2] = 0.0f;
        label.color[3] = 1.0f;
    }

    vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
}

void vk_end_label(VkCommandBuffer cmd) {
    if (!vkCmdEndDebugUtilsLabelEXT || !cmd) {
        return;
    }
    vkCmdEndDebugUtilsLabelEXT(cmd);
}

} // namespace nf::rhi
