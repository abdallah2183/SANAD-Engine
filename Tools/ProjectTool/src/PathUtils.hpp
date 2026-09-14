#pragma once

// Internal helpers shared by the scaffolder and the packager. Not part of the
// public API — deliberately not installed, because nothing outside this library
// should be reaching into the filesystem this way.

#include <NF/Project/ProjectCooker.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace nf::project::detail {

inline bool write_text(const std::filesystem::path& path,
                       const std::string& text,
                       std::string& out_error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        out_error = "failed to create '" + path.string() + "'";
        return false;
    }
    f << text;
    if (!f) {
        out_error = "failed to write '" + path.string() + "'";
        return false;
    }
    return true;
}

// Recursive copy that reports the first failure with the offending relative
// path. std::filesystem::copy's error reporting does not say which file failed,
// which is useless when copying a whole content tree.
inline bool copy_tree(const std::filesystem::path& from,
                      const std::filesystem::path& to,
                      std::string& out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(from, ec)) {
        out_error = "source directory does not exist: '" + from.string() + "'";
        return false;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const auto rel = std::filesystem::relative(it->path(), from, ec);
        if (ec) break;
        const auto dst = to / rel;
        if (it->is_directory(ec)) {
            std::filesystem::create_directories(dst, ec);
        } else if (it->is_regular_file(ec)) {
            std::filesystem::create_directories(dst.parent_path(), ec);
            std::filesystem::copy_file(it->path(), dst,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            out_error = "failed to copy '" + rel.generic_string() + "': " + ec.message();
            return false;
        }
    }
    if (ec) {
        out_error = "failed to walk '" + from.string() + "': " + ec.message();
        return false;
    }
    return true;
}

// FNV-1a over a file's bytes, streamed so a large asset does not have to be
// held in memory to be fingerprinted.
inline std::string fingerprint_file(const std::filesystem::path& path, std::string& out_error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out_error = "failed to open '" + path.string() + "'";
        return {};
    }
    uint64_t hash = 14695981039346656037ULL;
    char buf[8192];
    while (in) {
        in.read(buf, sizeof(buf));
        const auto got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ULL;
        }
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(hex);
}

} // namespace nf::project::detail
