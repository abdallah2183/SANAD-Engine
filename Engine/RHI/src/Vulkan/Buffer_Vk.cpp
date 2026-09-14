// NF/RHI/src/Vulkan/Buffer_Vk.cpp — Vulkan buffer implementation

#include "VulkanObjects.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <cstring>

namespace nf::rhi {

namespace {

VkMemoryPropertyFlags memory_props_for(MemoryUsage usage) {
    switch (usage) {
        case MemoryUsage::GPUOnly:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        case MemoryUsage::CPUOnly:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        case MemoryUsage::CPUToGPU:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        case MemoryUsage::GPUToCPU:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        default:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
}

} // namespace

VulkanBuffer::VulkanBuffer(VulkanDevice* device, const BufferDesc& desc)
    : m_device(device), m_size(desc.size), m_memory_usage(desc.memory) {
    m_alive.attach(m_device);

    NF_ASSERT(m_device, "VulkanBuffer requires a device");
    NF_ASSERT(desc.size > 0, "Cannot create a zero-sized buffer");

    const VkDevice vk_device = m_device->context().device;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = static_cast<VkDeviceSize>(desc.size);
    buffer_info.usage = to_vk_buffer_usage(desc.usage);
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // A GPU-local buffer that is expected to receive uploads must be a
    // transfer destination, otherwise the staging path below cannot target it.
    if (desc.memory == MemoryUsage::GPUOnly) {
        buffer_info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    if (!NF_VK_CHECK(vkCreateBuffer(vk_device, &buffer_info, nullptr, &m_buffer))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create buffer of {} bytes", desc.size);
        return;
    }

    VkMemoryRequirements mem_reqs{};
    vkGetBufferMemoryRequirements(vk_device, m_buffer, &mem_reqs);

    const u32 memory_type = m_device->find_memory_type(
        mem_reqs.memoryTypeBits, memory_props_for(desc.memory));
    if (memory_type == u32_max) {
        NF_LOG_ERROR(LogCategory::RHI, "No suitable memory type for buffer");
        release();
        return;
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = memory_type;

    if (!NF_VK_CHECK(vkAllocateMemory(vk_device, &alloc_info, nullptr, &m_memory))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to allocate {} bytes for buffer", mem_reqs.size);
        release();
        return;
    }

    if (!NF_VK_CHECK(vkBindBufferMemory(vk_device, m_buffer, m_memory, 0))) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to bind buffer memory");
        release();
        return;
    }

    NF_LOG_TRACE(LogCategory::RHI, "Buffer created: {} bytes", desc.size);
}

VulkanBuffer::~VulkanBuffer() {
    release();
}

void VulkanBuffer::release() {
    if (!m_device) return;

    const VkDevice vk_device = m_device->context().device;
    if (vk_device == VK_NULL_HANDLE) return;

    if (m_mapped) {
        vkUnmapMemory(vk_device, m_memory);
        m_mapped = nullptr;
    }
    if (m_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk_device, m_buffer, nullptr);
        m_buffer = VK_NULL_HANDLE;
    }
    if (m_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk_device, m_memory, nullptr);
        m_memory = VK_NULL_HANDLE;
    }
}

void* VulkanBuffer::map() {
    if (m_mapped) {
        return m_mapped;
    }

    if (m_memory_usage == MemoryUsage::GPUOnly) {
        NF_LOG_ERROR(LogCategory::RHI,
            "Cannot map a GPU-only buffer — use update(), which stages the upload");
        return nullptr;
    }

    void* data = nullptr;
    // Map the WHOLE memory object, not just the used range: flush/invalidate
    // with VK_WHOLE_SIZE is only valid when the mapping's end is either
    // nonCoherentAtomSize-aligned or the end of the memory object. Small
    // buffers (a 48-byte material block, a 4-byte staging texel) would end
    // mid-alignment and make every subsequent flush fail validation.
    if (!NF_VK_CHECK(vkMapMemory(m_device->context().device, m_memory, 0,
                                  VK_WHOLE_SIZE, 0, &data))) {
        return nullptr;
    }

    // Readback memory must be invalidated at map time: without it the CPU may
    // observe stale cached bytes instead of what the GPU's last transfer
    // wrote, which makes readback verification flaky or silently wrong.
    if (m_memory_usage == MemoryUsage::GPUToCPU) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = m_memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (!NF_VK_CHECK(vkInvalidateMappedMemoryRanges(m_device->context().device, 1, &range))) {
            NF_LOG_WARN(LogCategory::RHI, "InvalidateMappedMemoryRanges failed on readback map");
        }
    }

    m_mapped = data;
    return m_mapped;
}

void VulkanBuffer::unmap() {
    if (!m_mapped) return;

    vkUnmapMemory(m_device->context().device, m_memory);
    m_mapped = nullptr;
}

void VulkanBuffer::update(const void* data, usize offset, usize size) {
    NF_ASSERT(data, "update() called with null data");
    NF_ASSERT(offset + size <= m_size, "update() range exceeds buffer size");

    if (!data || size == 0) return;

    // Fast path: host-visible memory, write straight into it.
    if (m_memory_usage != MemoryUsage::GPUOnly) {
        void* dst = map();
        if (!dst) return;

        std::memcpy(static_cast<u8*>(dst) + offset, data, size);

        // Flush CPU writes so the GPU is guaranteed to observe them. On
        // HOST_COHERENT memory this is redundant but harmless; on non-coherent
        // host-visible memory it is required for CPU→GPU uploads (uniform
        // buffers updated per frame) to ever be seen by shaders.
        if (m_memory_usage == MemoryUsage::CPUToGPU || m_memory_usage == MemoryUsage::GPUToCPU) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = m_memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            vkFlushMappedMemoryRanges(m_device->context().device, 1, &range);
        }
        return;
    }

    // Slow path: device-local memory is not host-visible, so stage the upload
    // through a temporary host-visible buffer and copy on the GPU.
    BufferDesc staging_desc{};
    staging_desc.size = size;
    staging_desc.usage = BufferUsage::TransferSrc;
    staging_desc.memory = MemoryUsage::CPUToGPU;

    auto staging = std::make_unique<VulkanBuffer>(m_device, staging_desc);
    if (!staging->valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create staging buffer for upload");
        return;
    }

    staging->update(data, 0, size);

    const VkBuffer src = staging->handle();
    const VkBuffer dst = m_buffer;
    const VkDeviceSize dst_offset = static_cast<VkDeviceSize>(offset);
    const VkDeviceSize copy_size = static_cast<VkDeviceSize>(size);

    const bool ok = m_device->execute_single_time_commands(
        [src, dst, dst_offset, copy_size](VkCommandBuffer cmd) {
            VkBufferCopy copy{};
            copy.srcOffset = 0;
            copy.dstOffset = dst_offset;
            copy.size = copy_size;
            vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
        });

    if (!ok) {
        NF_LOG_ERROR(LogCategory::RHI, "Buffer upload copy failed");
    }
}

} // namespace nf::rhi
