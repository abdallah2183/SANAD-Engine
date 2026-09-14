#include <NF/Assets/MeshAsset.hpp>

#include <NF/Core/Logger.hpp>

#include <cstring>
#include <fstream>

namespace nf::assets {

static constexpr uint32_t kMagic = 0x4E464D45; // 'NFME'
static constexpr uint32_t kVersion = 1;

// The StaticMesh <-> MeshAsset conversions moved to Rendering/src/MeshUpload.cpp
// in Phase 11 W1 — see the comment on struct MeshAsset in the header.

bool MeshAsset::save_to_bytes(std::vector<uint8_t>& out) const {
    out.clear();
    size_t total = sizeof(kMagic) + sizeof(kVersion) + 16 + 4 + 4 + 4 + vertices.size()*sizeof(AssetVertex) + indices.size()*sizeof(uint32_t) + submeshes.size()*sizeof(AssetSubMesh) + sizeof(bounds) + sizeof(sphere) + 4;
    total += 4 + logical_path.size();
    out.reserve(total);
    auto append = [&](const void* data, size_t sz){ size_t off = out.size(); out.resize(off+sz); std::memcpy(out.data()+off, data, sz); };
    append(&kMagic, sizeof(kMagic));
    append(&kVersion, sizeof(kVersion));
    append(id.uuid.bytes.data(), 16);
    uint32_t path_len = static_cast<uint32_t>(logical_path.size());
    append(&path_len, sizeof(path_len));
    if (!logical_path.empty()) append(logical_path.data(), logical_path.size());
    uint32_t vert_count = static_cast<uint32_t>(vertices.size());
    uint32_t idx_count = static_cast<uint32_t>(indices.size());
    uint32_t sub_count = static_cast<uint32_t>(submeshes.size());
    append(&vert_count, sizeof(vert_count));
    append(&idx_count, sizeof(idx_count));
    append(&sub_count, sizeof(sub_count));
    if (!vertices.empty()) append(vertices.data(), vertices.size()*sizeof(AssetVertex));
    if (!indices.empty()) append(indices.data(), indices.size()*sizeof(uint32_t));
    if (!submeshes.empty()) append(submeshes.data(), submeshes.size()*sizeof(AssetSubMesh));
    append(&bounds, sizeof(bounds));
    append(&sphere, sizeof(sphere));
    return true;
}

std::unique_ptr<MeshAsset> MeshAsset::load_from_bytes(std::span<const uint8_t> data, std::string& out_error) {
    if (data.size() < sizeof(kMagic)+sizeof(kVersion)+16+4) {
        out_error = "File too small";
        return nullptr;
    }
    size_t off = 0;
    auto read = [&](void* dst, size_t sz) -> bool {
        if (off+sz > data.size()) return false;
        std::memcpy(dst, data.data()+off, sz);
        off+=sz;
        return true;
    };
    uint32_t magic=0, version=0;
    if (!read(&magic,sizeof(magic)) || magic!=kMagic) { out_error="Invalid magic"; return nullptr; }
    if (!read(&version,sizeof(version)) || version!=kVersion) { out_error="Unsupported version"; return nullptr; }
    AssetId id;
    if (!read(id.uuid.bytes.data(),16)) { out_error="Failed to read AssetId"; return nullptr; }
    uint32_t path_len=0;
    if (!read(&path_len,sizeof(path_len))) { out_error="Failed to read path_len"; return nullptr; }
    if (off+path_len > data.size()) { out_error="Path len out of bounds"; return nullptr; }
    std::string logical_path;
    if (path_len>0) {
        logical_path.assign(reinterpret_cast<const char*>(data.data()+off), path_len);
        off+=path_len;
    }
    uint32_t vert_count=0, idx_count=0, sub_count=0;
    if (!read(&vert_count,sizeof(vert_count)) || !read(&idx_count,sizeof(idx_count)) || !read(&sub_count,sizeof(sub_count))) {
        out_error="Failed to read counts"; return nullptr;
    }
    // Basic sanity
    if (vert_count > 1000000 || idx_count > 10000000 || sub_count > 1000) {
        out_error="Counts too large (corrupt?)";
        return nullptr;
    }
    auto asset = std::make_unique<MeshAsset>();
    asset->id = id;
    asset->logical_path = logical_path;
    asset->vertices.resize(vert_count);
    asset->indices.resize(idx_count);
    asset->submeshes.resize(sub_count);
    if (vert_count>0 && !read(asset->vertices.data(), vert_count*sizeof(AssetVertex))) { out_error="Failed to read vertices"; return nullptr; }
    if (idx_count>0 && !read(asset->indices.data(), idx_count*sizeof(uint32_t))) { out_error="Failed to read indices"; return nullptr; }
    if (sub_count>0 && !read(asset->submeshes.data(), sub_count*sizeof(AssetSubMesh))) { out_error="Failed to read submeshes"; return nullptr; }
    if (!read(&asset->bounds,sizeof(asset->bounds)) || !read(&asset->sphere,sizeof(asset->sphere))) { out_error="Failed to read bounds"; return nullptr; }
    return asset;
}

bool MeshAsset::save_to_file(const std::string& physical_path, std::string& out_error) const {
    std::vector<uint8_t> bytes;
    if (!save_to_bytes(bytes)) { out_error="Failed to serialize"; return false; }
    std::ofstream out(physical_path, std::ios::binary | std::ios::trunc);
    if (!out) { out_error="Failed to open file for writing: " + physical_path; return false; }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) { out_error="Failed to write file: " + physical_path; return false; }
    return true;
}

std::unique_ptr<MeshAsset> MeshAsset::load_from_file(const std::string& physical_path, std::string& out_error) {
    std::ifstream in(physical_path, std::ios::binary | std::ios::ate);
    if (!in) { out_error="Failed to open file for reading: " + physical_path; return nullptr; }
    auto size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    if (size>0) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (!in) { out_error="Failed to read file: " + physical_path; return nullptr; }
    }
    return load_from_bytes(std::span<const uint8_t>(data), out_error);
}

} // namespace nf::assets
