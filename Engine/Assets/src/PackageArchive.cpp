#include <NF/Assets/PackageArchive.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace nf::assets {

namespace {

#pragma pack(push, 1)
struct PackageHeader {
    uint8_t magic[8];
    uint32_t version;
    uint32_t entry_count;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t toc_offset;
    uint64_t toc_size;
    uint32_t crc32;
    uint8_t reserved[12];
};
#pragma pack(pop)

static_assert(sizeof(PackageHeader) == 64, "PackageHeader must be 64 bytes");

uint32_t compute_crc32(const void* data, size_t size) {
    static const uint32_t table[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };
    uint32_t crc = ~0u;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ table[crc & 0x0F];
        crc = (crc >> 4) ^ table[crc & 0x0F];
    }
    return ~crc;
}

} // namespace

std::string PackageArchive::normalize_key(std::string_view path) {
    std::string s(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') {
        s.erase(0, 1);
    }
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::unique_ptr<PackageArchive> PackageArchive::open_file(const std::filesystem::path& path, std::string& out_error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        out_error = "Failed to open package file: " + path.string();
        return nullptr;
    }

    PackageHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (file.gcount() != sizeof(header)) {
        out_error = "File too small to contain package header: " + path.string();
        return nullptr;
    }

    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        out_error = "Invalid package magic header: " + path.string();
        return nullptr;
    }

    if (header.version != kCurrentVersion) {
        out_error = "Unsupported package version: " + std::to_string(header.version);
        return nullptr;
    }

    file.seekg(static_cast<std::streamoff>(header.toc_offset));
    std::vector<uint8_t> toc_bytes(static_cast<size_t>(header.toc_size));
    file.read(reinterpret_cast<char*>(toc_bytes.data()), static_cast<std::streamsize>(toc_bytes.size()));
    if (static_cast<uint64_t>(file.gcount()) != header.toc_size) {
        out_error = "Failed to read complete Table of Contents: " + path.string();
        return nullptr;
    }

    uint32_t toc_crc = compute_crc32(toc_bytes.data(), toc_bytes.size());
    if (toc_crc != header.crc32) {
        out_error = "Package Table of Contents CRC mismatch in: " + path.string();
        return nullptr;
    }

    auto archive = std::make_unique<PackageArchive>();
    archive->m_file_path = path;

    const uint8_t* ptr = toc_bytes.data();
    const uint8_t* end = ptr + toc_bytes.size();

    for (uint32_t i = 0; i < header.entry_count; ++i) {
        if (ptr + sizeof(uint16_t) > end) {
            out_error = "Malformed TOC entry (path length overflow)";
            return nullptr;
        }
        uint16_t path_len = 0;
        std::memcpy(&path_len, ptr, sizeof(uint16_t));
        ptr += sizeof(uint16_t);

        if (ptr + path_len > end) {
            out_error = "Malformed TOC entry (path string overflow)";
            return nullptr;
        }
        std::string vpath(reinterpret_cast<const char*>(ptr), path_len);
        ptr += path_len;

        PackageEntry entry{};
        entry.virtual_path = vpath;

        if (ptr + sizeof(uint64_t) * 3 + sizeof(uint32_t) * 2 > end) {
            out_error = "Malformed TOC entry (metadata overflow)";
            return nullptr;
        }

        std::memcpy(&entry.offset, ptr, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        std::memcpy(&entry.uncompressed_size, ptr, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        std::memcpy(&entry.compressed_size, ptr, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        std::memcpy(&entry.crc32, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        std::memcpy(&entry.flags, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        std::string key = normalize_key(vpath);
        archive->m_entries[key] = entry;
    }

    return archive;
}

std::unique_ptr<PackageArchive> PackageArchive::open_memory(std::vector<uint8_t> data, std::string& out_error) {
    if (data.size() < sizeof(PackageHeader)) {
        out_error = "Memory buffer too small for package header";
        return nullptr;
    }

    PackageHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));

    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        out_error = "Invalid package magic header in memory";
        return nullptr;
    }

    if (header.version != kCurrentVersion) {
        out_error = "Unsupported package version: " + std::to_string(header.version);
        return nullptr;
    }

    if (header.toc_offset + header.toc_size > data.size()) {
        out_error = "Table of Contents exceeds memory buffer size";
        return nullptr;
    }

    const uint8_t* toc_bytes = data.data() + header.toc_offset;
    uint32_t toc_crc = compute_crc32(toc_bytes, static_cast<size_t>(header.toc_size));
    if (toc_crc != header.crc32) {
        out_error = "Memory package Table of Contents CRC mismatch";
        return nullptr;
    }

    auto archive = std::make_unique<PackageArchive>();
    const uint8_t* ptr = toc_bytes;
    const uint8_t* end = ptr + header.toc_size;

    for (uint32_t i = 0; i < header.entry_count; ++i) {
        if (ptr + sizeof(uint16_t) > end) {
            out_error = "Malformed TOC entry (path length overflow)";
            return nullptr;
        }
        uint16_t path_len = 0;
        std::memcpy(&path_len, ptr, sizeof(uint16_t));
        ptr += sizeof(uint16_t);

        if (ptr + path_len > end) {
            out_error = "Malformed TOC entry (path string overflow)";
            return nullptr;
        }
        std::string vpath(reinterpret_cast<const char*>(ptr), path_len);
        ptr += path_len;

        PackageEntry entry{};
        entry.virtual_path = vpath;

        if (ptr + sizeof(uint64_t) * 3 + sizeof(uint32_t) * 2 > end) {
            out_error = "Malformed TOC entry (metadata overflow)";
            return nullptr;
        }

        std::memcpy(&entry.offset, ptr, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        std::memcpy(&entry.uncompressed_size, ptr, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        std::memcpy(&entry.compressed_size, ptr, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        std::memcpy(&entry.crc32, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        std::memcpy(&entry.flags, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        std::string key = normalize_key(vpath);
        archive->m_entries[key] = entry;
    }

    archive->m_memory_buffer = std::move(data);
    return archive;
}

bool PackageArchive::contains(std::string_view relative_path) const {
    std::string key = normalize_key(relative_path);
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.find(key) != m_entries.end();
}

const PackageEntry* PackageArchive::get_entry(std::string_view relative_path) const {
    std::string key = normalize_key(relative_path);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<uint8_t> PackageArchive::read_bytes(std::string_view relative_path) const {
    std::string key = normalize_key(relative_path);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return {};
    }

    const auto& entry = it->second;

    if (!m_memory_buffer.empty()) {
        if (entry.offset + entry.uncompressed_size > m_memory_buffer.size()) {
            return {};
        }
        std::vector<uint8_t> res(static_cast<size_t>(entry.uncompressed_size));
        std::memcpy(res.data(), m_memory_buffer.data() + entry.offset, res.size());
        return res;
    }

    if (!m_file_path.empty()) {
        std::ifstream file(m_file_path, std::ios::binary);
        if (!file.is_open()) {
            return {};
        }
        file.seekg(static_cast<std::streamoff>(entry.offset));
        std::vector<uint8_t> res(static_cast<size_t>(entry.uncompressed_size));
        file.read(reinterpret_cast<char*>(res.data()), static_cast<std::streamsize>(res.size()));
        if (static_cast<size_t>(file.gcount()) != res.size()) {
            return {};
        }
        return res;
    }

    return {};
}

std::string PackageArchive::read_text(std::string_view relative_path) const {
    auto bytes = read_bytes(relative_path);
    if (bytes.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::string> PackageArchive::list_files() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> files;
    files.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries) {
        files.push_back(entry.virtual_path);
    }
    std::sort(files.begin(), files.end());
    return files;
}

void PackageBuilder::add_file(std::string_view virtual_path, std::span<const uint8_t> data) {
    PendingFile pf;
    pf.virtual_path = PackageArchive::normalize_key(virtual_path);
    pf.data.assign(data.begin(), data.end());
    m_pending_files.push_back(std::move(pf));
}

void PackageBuilder::add_file_text(std::string_view virtual_path, std::string_view text) {
    auto span = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    add_file(virtual_path, span);
}

bool PackageBuilder::add_file_from_disk(std::string_view virtual_path, const std::filesystem::path& disk_path, std::string& out_error) {
    std::ifstream file(disk_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        out_error = "Could not open file for reading: " + disk_path.string();
        return false;
    }

    auto sz = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(sz));
    file.read(reinterpret_cast<char*>(buffer.data()), sz);

    add_file(virtual_path, buffer);
    return true;
}

bool PackageBuilder::add_directory(const std::filesystem::path& root_dir, std::string_view mount_prefix, std::string& out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(root_dir, ec) || !std::filesystem::is_directory(root_dir, ec)) {
        out_error = "Directory does not exist: " + root_dir.string();
        return false;
    }

    std::string prefix(mount_prefix);
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root_dir, ec)) {
        if (entry.is_regular_file()) {
            auto rel = std::filesystem::relative(entry.path(), root_dir);
            std::string rel_str = rel.generic_string();
            std::string vpath = prefix + rel_str;
            if (!add_file_from_disk(vpath, entry.path(), out_error)) {
                return false;
            }
        }
    }
    return true;
}

std::vector<uint8_t> PackageBuilder::build_to_memory() {
    std::vector<uint8_t> data_payload;
    std::vector<PackageEntry> entries;
    entries.reserve(m_pending_files.size());

    uint64_t current_data_offset = sizeof(PackageHeader);

    for (const auto& pf : m_pending_files) {
        PackageEntry pe{};
        pe.virtual_path = pf.virtual_path;
        pe.offset = current_data_offset + data_payload.size();
        pe.uncompressed_size = pf.data.size();
        pe.compressed_size = pf.data.size();
        pe.crc32 = compute_crc32(pf.data.data(), pf.data.size());
        pe.flags = 0;

        data_payload.insert(data_payload.end(), pf.data.begin(), pf.data.end());
        entries.push_back(std::move(pe));
    }

    std::vector<uint8_t> toc_bytes;
    for (const auto& pe : entries) {
        uint16_t plen = static_cast<uint16_t>(pe.virtual_path.size());
        const uint8_t* plen_bytes = reinterpret_cast<const uint8_t*>(&plen);
        toc_bytes.insert(toc_bytes.end(), plen_bytes, plen_bytes + sizeof(plen));

        toc_bytes.insert(toc_bytes.end(), pe.virtual_path.begin(), pe.virtual_path.end());

        const uint8_t* pe_data = reinterpret_cast<const uint8_t*>(&pe.offset);
        toc_bytes.insert(toc_bytes.end(), pe_data, pe_data + sizeof(pe.offset));

        const uint8_t* u_size = reinterpret_cast<const uint8_t*>(&pe.uncompressed_size);
        toc_bytes.insert(toc_bytes.end(), u_size, u_size + sizeof(pe.uncompressed_size));

        const uint8_t* c_size = reinterpret_cast<const uint8_t*>(&pe.compressed_size);
        toc_bytes.insert(toc_bytes.end(), c_size, c_size + sizeof(pe.compressed_size));

        const uint8_t* crc_bytes = reinterpret_cast<const uint8_t*>(&pe.crc32);
        toc_bytes.insert(toc_bytes.end(), crc_bytes, crc_bytes + sizeof(pe.crc32));

        const uint8_t* flags_bytes = reinterpret_cast<const uint8_t*>(&pe.flags);
        toc_bytes.insert(toc_bytes.end(), flags_bytes, flags_bytes + sizeof(pe.flags));
    }

    PackageHeader header{};
    std::memcpy(header.magic, PackageArchive::kMagic, sizeof(header.magic));
    header.version = PackageArchive::kCurrentVersion;
    header.entry_count = static_cast<uint32_t>(entries.size());
    header.data_offset = sizeof(PackageHeader);
    header.data_size = data_payload.size();
    header.toc_offset = header.data_offset + header.data_size;
    header.toc_size = toc_bytes.size();
    header.crc32 = compute_crc32(toc_bytes.data(), toc_bytes.size());
    std::memset(header.reserved, 0, sizeof(header.reserved));

    std::vector<uint8_t> full_archive;
    full_archive.resize(sizeof(PackageHeader) + data_payload.size() + toc_bytes.size());

    std::memcpy(full_archive.data(), &header, sizeof(PackageHeader));
    if (!data_payload.empty()) {
        std::memcpy(full_archive.data() + header.data_offset, data_payload.data(), data_payload.size());
    }
    if (!toc_bytes.empty()) {
        std::memcpy(full_archive.data() + header.toc_offset, toc_bytes.data(), toc_bytes.size());
    }

    return full_archive;
}

bool PackageBuilder::build_to_file(const std::filesystem::path& output_path, std::string& out_error) {
    auto bytes = build_to_memory();

    std::error_code ec;
    auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open()) {
        out_error = "Failed to create output package file: " + output_path.string();
        return false;
    }

    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.good()) {
        out_error = "Write error while writing package file: " + output_path.string();
        return false;
    }

    return true;
}

} // namespace nf::assets
