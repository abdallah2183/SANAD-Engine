// NF/Core/FileSystem.cpp

#include <NF/Core/FileSystem.hpp>
#include <NF/Core/Logger.hpp>

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

    auto end = file.tellg();
    file.seekg(0, std::ios::beg);
    usize size = static_cast<usize>(end - file.tellg());

    std::vector<u8> buffer(size);
    if (size > 0) {
        file.read(reinterpret_cast<char*>(buffer.data()), size);
    }
    return buffer;
}

std::string FileSystem::read_text(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    auto end = file.tellg();
    file.seekg(0, std::ios::beg);
    usize size = static_cast<usize>(end - file.tellg());

    std::string text(size, '\0');
    if (size > 0) {
        file.read(text.data(), size);
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
