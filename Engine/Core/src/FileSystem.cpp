// NF/Core/FileSystem.cpp

#include <NF/Core/FileSystem.hpp>
#include <NF/Core/Logger.hpp>

#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace nf {

std::string FileSystem::s_engine_root;
std::string FileSystem::s_project_root;

// --- Existence ---

bool FileSystem::exists(std::string_view path) {
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

bool FileSystem::is_file(std::string_view path) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(path), ec);
}

bool FileSystem::is_directory(std::string_view path) {
    std::error_code ec;
    return fs::is_directory(fs::path(path), ec);
}

// --- Read ---

std::vector<u8> FileSystem::read_bytes(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    // tellg() answers -1 when it fails; casting that to an unsigned size
    // gives SIZE_MAX, and the allocation below then throws std::length_error
    // — an uncaught exception that takes the process down. An unreadable
    // size is an unreadable file, not a huge one.
    const std::streampos end = file.tellg();
    if (end == std::streampos(-1)) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    if (!file) {
        return {};
    }
    const usize size = static_cast<usize>(end);

    std::vector<u8> buffer(size);
    if (size > 0) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    }
    return buffer;
}

std::string FileSystem::read_text(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    // Same guard as read_bytes: a failed tellg() must not become SIZE_MAX.
    const std::streampos end = file.tellg();
    if (end == std::streampos(-1)) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    if (!file) {
        return {};
    }
    const usize size = static_cast<usize>(end);

    std::string text(size, '\0');
    if (size > 0) {
        file.read(text.data(), static_cast<std::streamsize>(size));
    }
    return text;
}

bool FileSystem::read_bytes(std::string_view path, std::vector<u8>& out) {
    out = read_bytes(path);
    return !out.empty();
}

// --- Write ---

bool FileSystem::write_bytes(std::string_view path, const u8* data, usize size) {
    std::ofstream file(std::string(path), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    if (size > 0) {
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    return true;
}

bool FileSystem::write_text(std::string_view path, std::string_view text) {
    return write_bytes(path, reinterpret_cast<const u8*>(text.data()), text.size());
}

// --- Directory operations ---

bool FileSystem::create_directory(std::string_view path) {
    std::error_code ec;
    return fs::create_directory(fs::path(path), ec);
}

bool FileSystem::create_directories(std::string_view path) {
    std::error_code ec;
    return fs::create_directories(fs::path(path), ec);
}

std::vector<std::string> FileSystem::list_directory(std::string_view path) {
    std::vector<std::string> result;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(fs::path(path), ec)) {
        result.push_back(entry.path().string());
    }
    return result;
}

// --- Path operations ---

std::string_view FileSystem::extension(std::string_view path) {
    auto pos = path.rfind('.');
    if (pos == std::string_view::npos) return {};
    return path.substr(pos);
}

std::string_view FileSystem::filename(std::string_view path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) return path;
    return path.substr(pos + 1);
}

std::string_view FileSystem::stem(std::string_view path) {
    auto fname = filename(path);
    auto pos = fname.rfind('.');
    if (pos == std::string_view::npos) return fname;
    return fname.substr(0, pos);
}

std::string FileSystem::parent_path(std::string_view path) {
    fs::path p(path);
    return p.parent_path().string();
}

// --- Engine paths ---

std::string FileSystem::engine_root() {
    if (s_engine_root.empty()) {
        // Auto-detect: walk up from executable looking for the marker file
        // For development, use the working directory
        s_engine_root = get_working_directory();
    }
    return s_engine_root;
}

std::string FileSystem::engine_bin() {
    return engine_root() + "/bin";
}

std::string FileSystem::engine_content() {
    return engine_root() + "/Content";
}

std::string FileSystem::project_root() {
    if (s_project_root.empty()) {
        s_project_root = get_working_directory();
    }
    return s_project_root;
}

// --- Scratch directory ---

namespace {

// The directory the running executable lives in. Falls back to the working
// directory when the platform will not say.
std::filesystem::path executable_directory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__linux__)
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return self.parent_path();
    }
#endif
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : cwd;
}

// Reads an environment variable. MSVC deprecates getenv(), and the project
// treats warnings as errors, so the secure form is used there.
std::string environment_variable(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer) {
        std::string value(buffer);
        std::free(buffer);
        return value;
    }
    return {};
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

// The user's own folder. It is the one place a denied process can nearly
// always write: it is the user's space, and it is never inside the engine's
// install directory (a scratch file next to the executables would collide with
// the "do not package over the running player" check).
std::filesystem::path home_directory() {
#if defined(_WIN32)
    const std::string profile = environment_variable("USERPROFILE");
    if (!profile.empty()) {
        return std::filesystem::path(profile);
    }
#endif
    const std::string home = environment_variable("HOME");
    if (!home.empty()) {
        return std::filesystem::path(home);
    }
    return {};
}

// Creates `dir` and reports whether it is now there.
bool make_directory(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec && std::filesystem::is_directory(dir, ec);
}

} // namespace

std::filesystem::path FileSystem::scratch_directory() {
    static const std::filesystem::path dir = [] {
        namespace fs = std::filesystem;
        // Probe the system temp directory: a directory that already exists
        // proves nothing, so a fresh one is created and removed.
        const fs::path sys = fs::temp_directory_path();
        const fs::path probe = sys / "nf_scratch_probe";
        std::error_code ec;
        fs::create_directories(probe, ec);
        if (!ec) {
            std::error_code rm;
            fs::remove_all(probe, rm);
            return sys;
        }
        // Denied. The candidates below are tried in order, and the first one
        // that can be created wins.
        //
        // The user's own folder comes first: it is theirs, and a scratch file
        // there cannot collide with the engine's install tree.
        const fs::path home = home_directory() / ".sanad_scratch";
        if (make_directory(home)) {
            return home;
        }
        // Then the working directory — where the process was started from,
        // which for the editor is the project the user opened. It is skipped
        // when it is INSIDE the executable directory: the packager refuses to
        // write a package over the running player, so a scratch project there
        // could never be built.
        const fs::path exe_dir = executable_directory();
        std::error_code cwd_ec;
        const fs::path cwd = fs::current_path(cwd_ec);
        if (!cwd_ec) {
            const fs::path candidate = cwd / ".nf_scratch";
            const fs::path relative = candidate.lexically_relative(exe_dir);
            const bool inside_exe_dir =
                !relative.empty() && *relative.begin() != ".." && relative != candidate;
            if (!inside_exe_dir && make_directory(candidate)) {
                return candidate;
            }
        }
        // Last resort: a directory beside the executable. The process was
        // just started from there, so it can write there even when the rest
        // of the disk is closed to it.
        const fs::path fallback = exe_dir / ".nf_scratch";
        make_directory(fallback);
        return fallback;
    }();
    return dir;
}

// --- Working directory ---

bool FileSystem::set_working_directory(std::string_view path) {
    std::error_code ec;
    fs::current_path(fs::path(path), ec);
    return !ec;
}

std::string FileSystem::get_working_directory() {
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    return cwd.string();
}

} // namespace nf
