#pragma once

// NF/Core/FileSystem.hpp — File system abstraction

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace nf {

namespace fs = std::filesystem;

class FileSystem {
public:
    // Existence
    static bool exists(std::string_view path);
    static bool is_file(std::string_view path);
    static bool is_directory(std::string_view path);

    // Read
    static std::vector<u8> read_bytes(std::string_view path);
    static std::string read_text(std::string_view path);
    static bool read_bytes(std::string_view path, std::vector<u8>& out);

    // Write
    static bool write_bytes(std::string_view path, const u8* data, usize size);
    static bool write_text(std::string_view path, std::string_view text);

    // Directory operations
    static bool create_directory(std::string_view path);
    static bool create_directories(std::string_view path);
    static std::vector<std::string> list_directory(std::string_view path);

    // Path operations
    static std::string_view extension(std::string_view path);
    static std::string_view filename(std::string_view path);
    static std::string_view stem(std::string_view path);
    static std::string parent_path(std::string_view path);

    // Engine paths
    static std::string engine_root();
    static std::string engine_bin();
    static std::string engine_content();
    static std::string project_root();

    // Working directory
    static bool set_working_directory(std::string_view path);
    static std::string get_working_directory();

private:
    // Cached paths (lazily initialized)
    static std::string s_engine_root;
    static std::string s_project_root;
};

} // namespace nf
