// NF/RHI/src/Vulkan/QueryPool_Vk.cpp — Vulkan timestamp query pool

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <vector>

namespace nf::rhi {

VulkanQueryPool::VulkanQueryPool(VulkanDevice* device, u32 count)
    : m_device(device), m_count(count) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanQueryPool requires a device");
    if (count == 0 || !m_device || !vkCreateQueryPool) {
        m_count = 0;
        return;
    }

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    if (!NF_VK_CHECK(vkCreateQueryPool(m_device->context().device, &info, nullptr, &m_pool))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create timestamp query pool ({} queries)", count);
        m_pool = VK_NULL_HANDLE;
        m_count = 0;
    }
}

VulkanQueryPool::~VulkanQueryPool() {
    if (m_device && m_pool != VK_NULL_HANDLE && vkDestroyQueryPool) {
        vkDestroyQueryPool(m_device->context().device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

bool VulkanQueryPool::get_results(std::span<u64> out, u32 first) {
    if (m_pool == VK_NULL_HANDLE || !vkGetQueryPoolResults || out.empty() ||
        first >= m_count || out.size() > static_cast<usize>(m_count - first)) {
        return false;
    }

    // VK_QUERY_RESULT_WITH_AVAILABILITY_BIT writes a 64-bit availability value
    // after every 64-bit timestamp, so the stride is two u64 values. Polling
    // with this bit never waits; VK_NOT_READY simply means the submission that
    // wrote the queries has not completed yet.
    std::vector<u64> raw(out.size() * 2, 0);
    const VkResult result = vkGetQueryPoolResults(
        m_device->context().device, m_pool, first, static_cast<u32>(out.size()),
        static_cast<VkDeviceSize>(raw.size() * sizeof(u64)), raw.data(),
        static_cast<VkDeviceSize>(2 * sizeof(u64)),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS) {
        return false;
    }

    for (usize i = 0; i < out.size(); ++i) {
        if (raw[i * 2 + 1] == 0) {
            return false;
        }
        out[i] = raw[i * 2];
    }
    return true;
}

} // namespace nf::rhi
