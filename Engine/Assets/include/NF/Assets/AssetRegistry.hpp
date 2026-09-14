#pragma once

#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/AssetTypes.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace nf::assets {

class AssetRegistry {
public:
    static constexpr uint32_t kCurrentVersion = 1;
    static constexpr const char* kRegistryFileName = "AssetRegistry.nfreg";

    AssetRegistry() = default;

    // Registry manipulation
    bool add(const AssetMetadata& meta, std::string& out_error);
    bool remove(AssetId id);
    bool contains(AssetId id) const;
    bool contains_path(const std::string& logical_path) const;

    const AssetMetadata* find(AssetId id) const;
    const AssetMetadata* find_by_path(const std::string& logical_path) const;
    AssetMetadata* find(AssetId id);
    AssetMetadata* find_by_path(const std::string& logical_path);

    size_t size() const { return m_entries.size(); }
    const std::unordered_map<AssetId, AssetMetadata>& entries() const { return m_entries; }

    void clear() { m_entries.clear(); m_path_to_id.clear(); }

    // Persistence (uses VFS logical paths, e.g. "content://AssetRegistry.nfreg" or "cache://...")
    // Save is atomic: writes to temp then renames
    bool save(VirtualFileSystem& vfs, const std::string& logical_path, std::string& out_error) const;
    bool load(VirtualFileSystem& vfs, const std::string& logical_path, std::string& out_error);

    // Direct file path save/load (for Tools that may not have VFS mounted)
    bool save_to_physical(const std::filesystem::path& physical_path, std::string& out_error) const;
    bool load_from_physical(const std::filesystem::path& physical_path, std::string& out_error);

    uint32_t version() const { return m_version; }
    void set_version(uint32_t v) { m_version = v; }

private:
    uint32_t m_version = kCurrentVersion;
    std::unordered_map<AssetId, AssetMetadata> m_entries;
    std::unordered_map<std::string, AssetId> m_path_to_id;
};

} // namespace nf::assets
