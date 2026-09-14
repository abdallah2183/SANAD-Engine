// NF/Platform/Platform.cpp

#include <NF/Platform/Platform.hpp>
#include <NF/Core/Logger.hpp>

namespace nf {

#if defined(_WIN32) || defined(_WIN64)

PlatformType get_platform_type() { return PlatformType::Windows; }
std::string_view platform_name() { return "Windows"; }

#elif defined(__linux__)

PlatformType get_platform_type() { return PlatformType::Linux; }
std::string_view platform_name() { return "Linux"; }

#elif defined(__APPLE__)

PlatformType get_platform_type() { return PlatformType::MacOS; }
std::string_view platform_name() { return "macOS"; }

#else

PlatformType get_platform_type() { return PlatformType::Windows; }
std::string_view platform_name() { return "Unknown"; }

#endif

bool platform_init() {
    NF_LOG_INFO(LogCategory::Platform, "Platform initialized: {}", platform_name());
    return true;
}

void platform_shutdown() {
    NF_LOG_INFO(LogCategory::Platform, "Platform shutting down: {}", platform_name());
}

} // namespace nf
