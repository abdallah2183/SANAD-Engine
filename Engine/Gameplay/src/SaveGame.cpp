#include <NF/Gameplay/SaveGame.hpp>

#include <fstream>
#include <array>

namespace nf::gameplay {

namespace {

// Standard CRC32 table generator
uint32_t calculate_crc32(const uint8_t* data, size_t length) {
    static const auto crc_table = []() {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (size_t j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        return table;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace

void SaveGameArchive::write_bool(bool v) {
    uint8_t byte = v ? 1 : 0;
    write_bytes(&byte, 1);
}

void SaveGameArchive::write_i32(int32_t v) {
    write_bytes(&v, sizeof(v));
}

void SaveGameArchive::write_u32(uint32_t v) {
    write_bytes(&v, sizeof(v));
}

void SaveGameArchive::write_i64(int64_t v) {
    write_bytes(&v, sizeof(v));
}

void SaveGameArchive::write_u64(uint64_t v) {
    write_bytes(&v, sizeof(v));
}

void SaveGameArchive::write_f32(float v) {
    write_bytes(&v, sizeof(v));
}

void SaveGameArchive::write_f64(double v) {
    write_bytes(&v, sizeof(v));
}

void SaveGameArchive::write_string(const std::string& str) {
    uint32_t len = static_cast<uint32_t>(str.size());
    write_u32(len);
    if (len > 0) {
        write_bytes(str.data(), len);
    }
}

void SaveGameArchive::write_bytes(const void* data, size_t size) {
    if (size == 0 || !data) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    m_buffer.insert(m_buffer.end(), p, p + size);
}

bool SaveGameArchive::read_bool(bool& out) {
    uint8_t byte = 0;
    if (!read_bytes(&byte, 1)) return false;
    out = (byte != 0);
    return true;
}

bool SaveGameArchive::read_i32(int32_t& out) {
    return read_bytes(&out, sizeof(out));
}

bool SaveGameArchive::read_u32(uint32_t& out) {
    return read_bytes(&out, sizeof(out));
}

bool SaveGameArchive::read_i64(int64_t& out) {
    return read_bytes(&out, sizeof(out));
}

bool SaveGameArchive::read_u64(uint64_t& out) {
    return read_bytes(&out, sizeof(out));
}

bool SaveGameArchive::read_f32(float& out) {
    return read_bytes(&out, sizeof(out));
}

bool SaveGameArchive::read_f64(double& out) {
    return read_bytes(&out, sizeof(out));
}

bool SaveGameArchive::read_string(std::string& out) {
    uint32_t len = 0;
    if (!read_u32(len)) return false;
    if (m_cursor + len > m_buffer.size()) return false;

    out.assign(reinterpret_cast<const char*>(m_buffer.data() + m_cursor), len);
    m_cursor += len;
    return true;
}

bool SaveGameArchive::read_bytes(void* out, size_t size) {
    if (size == 0) return true;
    if (!out || m_cursor + size > m_buffer.size()) return false;

    std::memcpy(out, m_buffer.data() + m_cursor, size);
    m_cursor += size;
    return true;
}

uint32_t SaveGameArchive::compute_checksum() const {
    return calculate_crc32(m_buffer.data(), m_buffer.size());
}

// --- SaveGameSystem ---

#pragma pack(push, 1)
struct SaveHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t checksum;
    uint32_t payload_size;
};
#pragma pack(pop)

bool SaveGameSystem::save_to_file(const std::filesystem::path& path,
                                 const SaveGameArchive& archive,
                                 std::string& out_error)
{
    std::filesystem::path tmp_path = path;
    tmp_path += ".tmp";

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(tmp_path, std::ios::binary);
    if (!out.is_open()) {
        out_error = "Failed to open temporary file for write: " + tmp_path.string();
        return false;
    }

    SaveHeader header;
    header.magic = SaveGameArchive::kMagic;
    header.version = SaveGameArchive::kCurrentVersion;
    header.checksum = archive.compute_checksum();
    header.payload_size = static_cast<uint32_t>(archive.size());

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (header.payload_size > 0) {
        out.write(reinterpret_cast<const char*>(archive.data()), header.payload_size);
    }

    out.close();
    if (!out) {
        out_error = "Write failure during save execution";
        return false;
    }

    // Atomic replace
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        // Fallback for Windows cross-volume or lock replacement
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            out_error = "Failed to rename temp save file: " + ec.message();
            return false;
        }
    }
    return true;
}

bool SaveGameSystem::load_from_file(const std::filesystem::path& path,
                                   SaveGameArchive& out_archive,
                                   std::string& out_error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        out_error = "Save file not found: " + path.string();
        return false;
    }

    SaveHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || in.gcount() != sizeof(header)) {
        out_error = "Corrupted save file header";
        return false;
    }

    if (header.magic != SaveGameArchive::kMagic) {
        out_error = "Invalid save game magic format";
        return false;
    }

    if (header.version > SaveGameArchive::kCurrentVersion) {
        out_error = "Save file version newer than engine version";
        return false;
    }

    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size > 0) {
        in.read(reinterpret_cast<char*>(payload.data()), header.payload_size);
        if (!in || static_cast<uint32_t>(in.gcount()) != header.payload_size) {
            out_error = "Truncated save file payload";
            return false;
        }
    }

    uint32_t computed = calculate_crc32(payload.data(), payload.size());
    if (computed != header.checksum) {
        out_error = "Save file checksum mismatch (corrupted data)";
        return false;
    }

    out_archive.set_buffer(std::move(payload));
    return true;
}

bool SaveGameSystem::exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !std::filesystem::is_directory(path, ec);
}

bool SaveGameSystem::remove(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

} // namespace nf::gameplay
