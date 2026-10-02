// NF/RHI/RHI.cpp — Device factory + global validation tracking

#include <NF/RHI/RHI.hpp>
#include <NF/Core/Logger.hpp>

#include <atomic>

#if defined(NF_VULKAN_ENABLED)
    #include <NF/RHI/Vulkan/Device_Vk.hpp>
#endif

namespace nf::rhi {

namespace {
// Incremented by the Vulkan debug messenger on every validation error.
// Deliberately global: it must survive across device instances so tests can
// snapshot it around any section of work and assert the delta is zero.
std::atomic<u32> g_validation_errors{0};
// Latched: a lost device never comes back, so this stays true once set. See
// device_lost() in the header for why the render loop must act on it.
std::atomic<bool> g_device_lost{false};
} // namespace

void record_validation_error() { g_validation_errors.fetch_add(1, std::memory_order_relaxed); }
u32 validation_error_count() { return g_validation_errors.load(std::memory_order_relaxed); }
void reset_validation_error_count() { g_validation_errors.store(0, std::memory_order_relaxed); }

void record_device_lost() { g_device_lost.store(true, std::memory_order_relaxed); }
bool device_lost() { return g_device_lost.load(std::memory_order_relaxed); }
void reset_device_lost() { g_device_lost.store(false, std::memory_order_relaxed); }

std::unique_ptr<IGraphicsDevice> create_device() {
#if defined(NF_VULKAN_ENABLED)
    NF_LOG_INFO(LogCategory::RHI, "Creating Vulkan graphics device");
    auto device = std::make_unique<VulkanDevice>();
    return device;
#else
    NF_LOG_ERROR(LogCategory::RHI, "No RHI backend enabled");
    return nullptr;
#endif
}

} // namespace nf::rhi
