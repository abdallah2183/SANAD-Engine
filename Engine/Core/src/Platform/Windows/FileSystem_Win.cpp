// NF/Core/Platform/Windows/FileSystem_Win.cpp — Windows-specific FS utilities
// Most FS operations are portable via std::filesystem; this file holds any
// platform-specific code (e.g., native path resolution, long path support).

#include <NF/Core/FileSystem.hpp>
#include <NF/Core/Logger.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace nf {

// On Windows, enable long path support and normalize separators
std::string normalize_path_win(std::string_view path) {
    // Convert forward slashes to backslashes for native API calls
    std::string result(path);
    for (char& c : result) {
        if (c == '/') c = '\\';
    }
    return result;
}

} // namespace nf
