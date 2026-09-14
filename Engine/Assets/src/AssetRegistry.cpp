#include <NF/Assets/AssetRegistry.hpp>

#include <NF/Core/Logger.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace nf::assets {

bool AssetRegistry::add(const AssetMetadata& meta, std::string& out_error) {
    if (!meta.id.valid()) {
        out_error = "AssetId is invalid";
        return false;
    }
    if (meta.logical_path.empty()) {
        out_error = "logical_path is empty";
        return false;
    }
    if (m_entries.find(meta.id) != m_entries.end()) {
        out_error = "Duplicate AssetId: " + meta.id.to_string();
        return false;
    }
    if (m_path_to_id.find(meta.logical_path) != m_path_to_id.end()) {
        out_error = "Duplicate logical_path: " + meta.logical_path;
        return false;
    }
    m_entries[meta.id] = meta;
    m_path_to_id[meta.logical_path] = meta.id;
    return true;
}

bool AssetRegistry::remove(AssetId id) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return false;
    m_path_to_id.erase(it->second.logical_path);
    m_entries.erase(it);
    return true;
}

bool AssetRegistry::contains(AssetId id) const {
    return m_entries.find(id) != m_entries.end();
}

bool AssetRegistry::contains_path(const std::string& logical_path) const {
    return m_path_to_id.find(logical_path) != m_path_to_id.end();
}

const AssetMetadata* AssetRegistry::find(AssetId id) const {
    auto it = m_entries.find(id);
    return it != m_entries.end() ? &it->second : nullptr;
}

const AssetMetadata* AssetRegistry::find_by_path(const std::string& logical_path) const {
    auto it = m_path_to_id.find(logical_path);
    if (it == m_path_to_id.end()) return nullptr;
    return find(it->second);
}

AssetMetadata* AssetRegistry::find(AssetId id) {
    auto it = m_entries.find(id);
    return it != m_entries.end() ? &it->second : nullptr;
}

AssetMetadata* AssetRegistry::find_by_path(const std::string& logical_path) {
    auto it = m_path_to_id.find(logical_path);
    if (it == m_path_to_id.end()) return nullptr;
    return find(it->second);
}

static std::string trim(const std::string& s) {
    size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b-a);
}

bool AssetRegistry::save_to_physical(const std::filesystem::path& physical_path, std::string& out_error) const {
    std::filesystem::path tmp = physical_path;
    tmp += ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(tmp.parent_path(), ec);
    if (ec) {
        out_error = "Failed to create directories for '" + physical_path.string() + "': " + ec.message();
        return false;
    }

    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        out_error = "Failed to open file for writing: '" + tmp.string() + "'";
        return false;
    }

    out << "# NOVAForge Asset Registry\n";
    out << "version: " << m_version << "\n";
    out << "count: " << m_entries.size() << "\n";
    for (auto& [id, meta] : m_entries) {
        out << "---\n";
        out << "id: " << meta.id.to_string() << "\n";
        out << "type: " << to_string(meta.type) << "\n";
        out << "logical: " << meta.logical_path << "\n";
        out << "cooked: " << meta.cooked_path << "\n";
        out << "fingerprint: " << meta.fingerprint << "\n";
        out << "format: " << meta.format << "\n";
        out << "version: " << meta.version << "\n";
    }
    out.close();
    if (!out) {
        out_error = "Failed to write registry file: '" + tmp.string() + "'";
        std::filesystem::remove(tmp, ec);
        return false;
    }

    // Atomic replace: rename temp to final
    std::filesystem::rename(tmp, physical_path, ec);
    if (ec) {
        // Try remove and rename again (for Windows where target exists)
        std::filesystem::remove(physical_path, ec);
        std::filesystem::rename(tmp, physical_path, ec);
        if (ec) {
            out_error = "Failed to atomically replace '" + physical_path.string() + "': " + ec.message();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

bool AssetRegistry::load_from_physical(const std::filesystem::path& physical_path, std::string& out_error) {
    std::ifstream in(physical_path, std::ios::binary);
    if (!in) {
        out_error = "Failed to open registry file for reading: '" + physical_path.string() + "'";
        return false;
    }

    std::string line;
    // Header
    if (!std::getline(in, line) || line.rfind("# NOVAForge Asset Registry",0) != 0) {
        out_error = "Invalid registry header";
        return false;
    }
    if (!std::getline(in, line)) { out_error = "Missing version line"; return false; }
    {
        auto pos = line.find(":");
        if (pos==std::string::npos) { out_error = "Invalid version line"; return false; }
        std::string vstr = trim(line.substr(pos+1));
        try { m_version = static_cast<uint32_t>(std::stoul(vstr)); } catch(...) { out_error = "Invalid version number"; return false; }
        if (m_version != kCurrentVersion) {
            out_error = "Unsupported registry version: " + vstr + " (expected " + std::to_string(kCurrentVersion) + ")";
            return false;
        }
    }
    if (!std::getline(in, line)) { out_error = "Missing count line"; return false; }
    size_t expected_count = 0;
    {
        auto pos = line.find(":");
        if (pos==std::string::npos) { out_error = "Invalid count line"; return false; }
        try { expected_count = static_cast<size_t>(std::stoul(trim(line.substr(pos+1)))); } catch(...) { out_error = "Invalid count number"; return false; }
    }

    clear();

    for (size_t i=0;i<expected_count;++i){
        if (!std::getline(in, line) || trim(line) != "---") { out_error = "Expected '---' separator"; clear(); return false; }
        AssetMetadata meta;
        // id
        if (!std::getline(in, line)) { out_error="Missing id"; clear(); return false; }
        {
            auto pos = line.find(":");
            if (pos==std::string::npos) { out_error="Invalid id line"; clear(); return false; }
            std::string id_str = trim(line.substr(pos+1));
            meta.id = AssetId::from_string(id_str);
            if (!meta.id.valid()) { out_error="Invalid AssetId: "+id_str; clear(); return false; }
        }
        // type
        if (!std::getline(in, line)) { out_error="Missing type"; clear(); return false; }
        {
            auto pos = line.find(":");
            std::string tstr = trim(line.substr(pos+1));
            meta.type = asset_type_from_string(tstr);
            if (meta.type==AssetType::Unknown) { out_error="Unknown asset type: "+tstr; clear(); return false; }
        }
        // logical
        if (!std::getline(in, line)) { out_error="Missing logical"; clear(); return false; }
        {
            auto pos = line.find(":");
            meta.logical_path = trim(line.substr(pos+1));
            if (meta.logical_path.empty()) { out_error="Empty logical_path"; clear(); return false; }
        }
        // cooked
        if (!std::getline(in, line)) { out_error="Missing cooked"; clear(); return false; }
        {
            auto pos = line.find(":");
            meta.cooked_path = trim(line.substr(pos+1));
        }
        // fingerprint
        if (!std::getline(in, line)) { out_error="Missing fingerprint"; clear(); return false; }
        {
            auto pos = line.find(":");
            meta.fingerprint = trim(line.substr(pos+1));
        }
        // format
        if (!std::getline(in, line)) { out_error="Missing format"; clear(); return false; }
        {
            auto pos = line.find(":");
            meta.format = trim(line.substr(pos+1));
        }
        // version
        if (!std::getline(in, line)) { out_error="Missing version"; clear(); return false; }
        {
            auto pos = line.find(":");
            try { meta.version = static_cast<uint32_t>(std::stoul(trim(line.substr(pos+1)))); } catch(...) { out_error="Invalid meta version"; clear(); return false; }
        }

        std::string add_err;
        if (!add(meta, add_err)) { out_error = "Failed to add entry " + std::to_string(i) + ": " + add_err; clear(); return false; }
    }

    return true;
}

bool AssetRegistry::save(VirtualFileSystem& vfs, const std::string& logical_path, std::string& out_error) const {
    auto r = vfs.resolve(logical_path);
    if (!r.ok) { out_error = r.error; return false; }
    return save_to_physical(r.value, out_error);
}

bool AssetRegistry::load(VirtualFileSystem& vfs, const std::string& logical_path, std::string& out_error) {
    auto r = vfs.resolve(logical_path);
    if (!r.ok) { out_error = r.error; return false; }
    return load_from_physical(r.value, out_error);
}

} // namespace nf::assets
