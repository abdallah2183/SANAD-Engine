#pragma once

// NF/Test/RHITestCommon.hpp — Shared fixtures for the headless RHI tests.
//
// Every RHI test needs the same two things: a GPU device that works without a
// window, and a way to read pixels back off the GPU. Both live here so the
// tests stay focused on what they are actually verifying.

#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace nf::test {

/// A GPU is not guaranteed on the machine running the tests, so every RHI test
/// degrades to "skipped" rather than failing the suite.
///
/// The device is deliberately a static singleton: creating a VkInstance per
/// test would be slow, and there is no state shared between tests that a
/// fresh device would need to clear.
struct GpuFixture {
    std::unique_ptr<rhi::IGraphicsDevice> device;
    bool available = false;

    GpuFixture() {
        Logger::instance().set_min_level(LogLevel::Warn);

        device = rhi::create_device();
        if (!device) return;

        // Headless: no window, no swapchain. Offscreen rendering and readback
        // are all these tests need.
        // Validation can be forced even in Release via NF_RHI_VALIDATION env var
        // or --validation passed to the test binary (useful for CI).
        bool want_validation = false;
#if defined(_MSC_VER)
        char* buf = nullptr; usize sz = 0;
        if (_dupenv_s(&buf, &sz, "NF_RHI_VALIDATION") == 0 && buf) {
            want_validation = true; std::free(buf);
        }
#else
        if (std::getenv("NF_RHI_VALIDATION")) want_validation = true;
#endif
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        desc.enable_validation = want_validation;
        available = device->init(desc);
        if (!available) device.reset();
        // Report what the device ACTUALLY enabled, not what was requested.
        // Debug builds force validation on regardless of DeviceDesc, so echoing
        // the requested flag reported "off" while validation errors were being
        // printed — actively misleading for anyone debugging a GPU failure.
        const bool validation_active = available && device->validation_enabled();
        // Make GPU availability explicit in test output — a silent skip is
        // indistinguishable from a pass otherwise.
        NF_LOG_WARN(LogCategory::Core, "GpuFixture: device {} (validation: {})",
                    available ? "available" : "UNAVAILABLE", validation_active ? "on" : "off");
    }
};

inline const GpuFixture& gpu() {
    static const GpuFixture fixture;
    return fixture;
}

/// Returns the GPU fixture, marking the current test SKIPPED when no device is
/// available.
///
/// Use this instead of `gpu()` + an early `return`: a bare return is recorded
/// as a PASS, so on a GPU-less machine (CI) the whole GPU suite would report
/// green while verifying nothing.
inline const GpuFixture& require_gpu() {
    const GpuFixture& f = gpu();
    if (!f.available) {
        NF_SKIP("no Vulkan device available");
    }
    return f;
}

/// One RGBA8 pixel as it comes back from the GPU.
struct Pixel {
    u8 r = 0, g = 0, b = 0, a = 0;
};

inline std::vector<u8> load_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};

    const auto size = static_cast<usize>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<u8> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (static_cast<usize>(file.gcount()) != size) return {};
    return data;
}

/// How long a test waits for the GPU before declaring a hang. Generous enough
/// that a slow CI machine never flakes, short enough that a real deadlock is
/// reported as a failure rather than a timeout of the whole suite.
constexpr u64 kGpuTimeoutNs = 5'000'000'000ull;

} // namespace nf::test
