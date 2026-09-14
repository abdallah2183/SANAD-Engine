#pragma once

// NF/Platform/Platform.hpp — Platform abstraction layer

#include <NF/Core/Types.hpp>

#include <string>
#include <string_view>

namespace nf {

enum class PlatformType {
    Windows,
    Linux,
    MacOS,
    Android,
    iOS
};

PlatformType get_platform_type();
std::string_view platform_name();

// Initialize platform-specific subsystems
bool platform_init();
void platform_shutdown();

} // namespace nf
