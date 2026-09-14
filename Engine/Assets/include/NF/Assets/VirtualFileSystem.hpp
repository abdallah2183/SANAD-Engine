#pragma once

#include <NF/Core/Types.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nf::assets {

// Simple Result type for VFS operations
template<typename T>
struct VFSResult {
    bool ok = false;
    T value{};
    std::string error;

    static VFSResult success(T v) { return {true, std::move(v), {}}; }
    static VFSResult failure(std::string err) { return {false, {}, std::move(err)}; }
};

template<>
struct VFSResult<void> {
    bool ok = false;
    std::string error;
    static VFSResult success() { return {true, {}}; }
    static VFSResult failure(std::string err) { return {false, std::move(err)}; }
};

template<>
struct VFSResult<bool> {
    bool ok = false;
    bool value = false;
    std::string error;
    static VFSResult success(bool v) { return {true, v, {}}; }
    static VFSResult failure(std::string err) { return {false, false, std::move(err)}; }
};

class VirtualFileSystem {
public:
    VirtualFileSystem() = default;

    // Mount a logical prefix (e.g. "content://") to a physical directory
    // logical must end with "://", physical must be an existing directory or will be created
    VFSResult<void> mount(std::string_view logical, const std::filesystem::path& physical);

    // Resolve a logical path (e.g. "content://Meshes/cube.nfmesh") to a physical path
    // Returns error if the path tries to traverse outside the mount or is malformed
    VFSResult<std::filesystem::path> resolve(std::string_view logical_path) const;

    // File operations (all take logical paths)
    VFSResult<bool> exists(std::string_view logical_path) const;
    VFSResult<std::vector<u8>> read_bytes(std::string_view logical_path) const;
    VFSResult<std::string> read_text(std::string_view logical_path) const;
    VFSResult<void> write_bytes(std::string_view logical_path, std::span<const u8> data);
    VFSResult<void> write_text(std::string_view logical_path, std::string_view text);
    VFSResult<void> create_directories(std::string_view logical_path);
    VFSResult<bool> is_directory(std::string_view logical_path) const;

    // For testing: list mounts
    std::vector<std::string> mounts() const;

private:
    struct Mount {
        std::string logical; // e.g. "content://"
        std::filesystem::path physical; // canonical physical path
    };
    std::vector<Mount> m_mounts;

    std::string normalize_logical(std::string_view path) const;
    bool is_within_mount(const std::filesystem::path& mount_physical, const std::filesystem::path& target) const;
    VFSResult<std::filesystem::path> resolve_internal(std::string_view logical_path) const;
};

} // namespace nf::assets
