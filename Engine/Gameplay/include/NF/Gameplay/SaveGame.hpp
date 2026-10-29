#pragma once

#include <NF/Core/Types.hpp>

#include <string>
#include <vector>
#include <filesystem>
#include <cstring>

namespace nf::gameplay {

/// Versioned, checksummed binary archive for game save states.
class SaveGameArchive {
public:
    static constexpr uint32_t kMagic = 0x4E465356u; // 'NFSV'
    static constexpr uint32_t kCurrentVersion = 1;

    SaveGameArchive() = default;

    // Serialization
    void write_bool(bool v);
    void write_i32(int32_t v);
    void write_u32(uint32_t v);
    void write_i64(int64_t v);
    void write_u64(uint64_t v);
    void write_f32(float v);
    void write_f64(double v);
    void write_string(const std::string& str);
    void write_bytes(const void* data, size_t size);

    // Deserialization
    bool read_bool(bool& out);
    bool read_i32(int32_t& out);
    bool read_u32(uint32_t& out);
    bool read_i64(int64_t& out);
    bool read_u64(uint64_t& out);
    bool read_f32(float& out);
    bool read_f64(double& out);
    bool read_string(std::string& out);
    bool read_bytes(void* out, size_t size);

    // Buffer inspection
    size_t size() const { return m_buffer.size(); }
    const uint8_t* data() const { return m_buffer.data(); }
    size_t read_cursor() const { return m_cursor; }
    void reset_cursor() { m_cursor = 0; }
    void clear() { m_buffer.clear(); m_cursor = 0; }

    uint32_t compute_checksum() const;

    // Direct buffer initialization for loading
    void set_buffer(std::vector<uint8_t> buffer) {
        m_buffer = std::move(buffer);
        m_cursor = 0;
    }

private:
    std::vector<uint8_t> m_buffer;
    size_t m_cursor = 0;
};

/// File management and persistence utility for SaveGameArchive slots.
class SaveGameSystem {
public:
    /// Atomically saves an archive to a physical file with checksum and header validation.
    static bool save_to_file(const std::filesystem::path& path,
                             const SaveGameArchive& archive,
                             std::string& out_error);

    /// Loads and validates a save game file, checking magic and CRC32 checksum.
    static bool load_from_file(const std::filesystem::path& path,
                               SaveGameArchive& out_archive,
                               std::string& out_error);

    /// Checks if a save file exists.
    static bool exists(const std::filesystem::path& path);

    /// Deletes a save file.
    static bool remove(const std::filesystem::path& path);
};

} // namespace nf::gameplay
