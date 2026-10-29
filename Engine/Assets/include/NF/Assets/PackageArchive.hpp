#pragma once

#include <NF/Core/Types.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nf::assets {

/// Metadata for a single packaged file inside a PackageArchive (.nfpak).
struct PackageEntry {
    std::string virtual_path;
    uint64_t offset = 0;
    uint64_t uncompressed_size = 0;
    uint64_t compressed_size = 0;
    uint32_t crc32 = 0;
    uint32_t flags = 0;
};

/// High-performance read-only binary package archive (.nfpak).
class PackageArchive {
public:
    static constexpr uint8_t kMagic[8] = {'N', 'F', 'P', 'A', 'K', 1, 0, 0};
    static constexpr uint32_t kCurrentVersion = 1;

    PackageArchive() = default;
    ~PackageArchive() = default;

    PackageArchive(const PackageArchive&) = delete;
    PackageArchive& operator=(const PackageArchive&) = delete;

    /// Opens an archive from disk.
    static std::unique_ptr<PackageArchive> open_file(const std::filesystem::path& path, std::string& out_error);

    /// Opens an archive directly from an in-memory buffer.
    static std::unique_ptr<PackageArchive> open_memory(std::vector<uint8_t> data, std::string& out_error);

    /// Checks if a file exists inside the archive.
    bool contains(std::string_view relative_path) const;

    /// Retrieves file metadata if present.
    const PackageEntry* get_entry(std::string_view relative_path) const;

    /// Reads all bytes for the given file.
    std::vector<uint8_t> read_bytes(std::string_view relative_path) const;

    /// Reads file as UTF-8 text.
    std::string read_text(std::string_view relative_path) const;

    /// Returns list of all virtual paths in the archive.
    std::vector<std::string> list_files() const;

    /// Count of packaged files.
    size_t file_count() const { return m_entries.size(); }

    /// Returns path to archive on disk (if opened from file).
    const std::filesystem::path& file_path() const { return m_file_path; }

    /// Normalizes path for archive key lookup (removes leading slash, forward slashes).
    static std::string normalize_key(std::string_view path);

private:
    std::filesystem::path m_file_path;
    std::vector<uint8_t> m_memory_buffer;
    std::unordered_map<std::string, PackageEntry> m_entries;
    mutable std::mutex m_mutex;
};

/// Utility to author and pack .nfpak archives from loose files or memory buffers.
class PackageBuilder {
public:
    PackageBuilder() = default;

    /// Adds a file from raw memory bytes.
    void add_file(std::string_view virtual_path, std::span<const uint8_t> data);

    /// Adds a file from string content.
    void add_file_text(std::string_view virtual_path, std::string_view text);

    /// Reads and adds a file from physical disk.
    bool add_file_from_disk(std::string_view virtual_path, const std::filesystem::path& disk_path, std::string& out_error);

    /// Recursively scans and adds a whole directory tree from disk.
    bool add_directory(const std::filesystem::path& root_dir, std::string_view mount_prefix, std::string& out_error);

    /// Builds and writes the archive to a file on disk.
    bool build_to_file(const std::filesystem::path& output_path, std::string& out_error);

    /// Builds and returns the archive binary data in memory.
    std::vector<uint8_t> build_to_memory();

    /// Pending files count.
    size_t file_count() const { return m_pending_files.size(); }

    /// Clears all pending entries.
    void clear() { m_pending_files.clear(); }

private:
    struct PendingFile {
        std::string virtual_path;
        std::vector<uint8_t> data;
    };
    std::vector<PendingFile> m_pending_files;
};

} // namespace nf::assets
