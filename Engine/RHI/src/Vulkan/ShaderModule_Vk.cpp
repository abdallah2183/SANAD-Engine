// NF/RHI/src/Vulkan/ShaderModule_Vk.cpp — Vulkan shader module implementation

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

namespace nf::rhi {

VulkanShaderModule::VulkanShaderModule(VulkanDevice* device, const ShaderModuleDesc& desc)
    : m_device(device), m_stage(desc.stage) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanShaderModule requires a device");

    if (desc.code.empty()) {
        NF_LOG_ERROR(LogCategory::RHI, "Shader module created with empty SPIR-V");
        return;
    }

    // SPIR-V is a word stream, so the byte size must be a multiple of 4 and
    // the buffer must be suitably aligned.
    if (desc.code.size() % 4 != 0) {
        NF_LOG_ERROR(LogCategory::RHI,
            "SPIR-V size {} is not a multiple of 4 — the binary is corrupt",
            desc.code.size());
        return;
    }

    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = desc.code.size();
    create_info.pCode = reinterpret_cast<const u32*>(desc.code.data());

    if (!NF_VK_CHECK(vkCreateShaderModule(m_device->context().device, &create_info,
                                           nullptr, &m_module))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create shader module ({} bytes of SPIR-V)",
                     desc.code.size());
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Shader module created ({} bytes of SPIR-V)",
                 desc.code.size());
}

VulkanShaderModule::~VulkanShaderModule() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;

    if (m_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(vk_device, m_module, nullptr);
        m_module = VK_NULL_HANDLE;
    }
}

} // namespace nf::rhi
