// NF/Platform/Windows/Platform_Win.cpp

#include <NF/Platform/Platform.hpp>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace nf {

// On Windows, platform init mostly means setting up COM, DPI awareness, etc.
// This is called from platform_init(), but since that's already generic,
// we only store Windows-specific initialization here if needed later.

bool platform_init_win() {
    // Set DPI awareness for proper scaling
    SetProcessDPIAware();
    return true;
}

} // namespace nf
