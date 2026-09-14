#include <NF/Assets/MeshAsset.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <NF/Core/Logger.hpp>

#include <cstring>
#include <fstream>

namespace nf::assets {

static constexpr uint32_t kMagic = 0x4E464D45; // 'NFME'
static constexpr uint32_t kVersion = 1;

std::unique_ptr<rendering::StaticMesh> MeshAsset::to_static_mesh(const std::string& name) const {
    auto mesh = std::make_unique<rendering::StaticMesh>(name.empty() ? logical_path : name);
    auto& lod = mesh->lod(0);
    lod.vertices.resize(vertices.size());
    for (size_t i=0;i<vertices.size();++i){
        auto& src = vertices[i];
        auto& dst = lod.vertices[i];
        std::memcpy(dst.position, src.position, sizeof(dst.position));
        std::memcpy(dst.normal, src.normal, sizeof(dst.normal));
        std::memcpy(dst.tangent, src.tangent, sizeof(dst.tangent));
        std::memcpy(dst.uv0, src.uv0, sizeof(dst.uv0));
        std::memcpy(dst.uv1, src.uv1, sizeof(dst.uv1));
    }
    lod.indices = indices;
    lod.submeshes.resize(submeshes.size());
    for (size_t i=0;i<submeshes.size();++i){
        auto& src = submeshes[i];
        auto& dst = lod.submeshes[i];
        dst.index_offset = src.index_offset;
        dst.index_count = src.index_count;
        dst.vertex_offset = src.vertex_offset;
        dst.vertex_count = src.vertex_count;
        dst.material_slot = src.material_slot;
        dst.bounds.min_x = src.bounds.min_x; dst.bounds.min_y = src.bounds.min_y; dst.bounds.min_z = src.bounds.min_z;
        dst.bounds.max_x = src.bounds.max_x; dst.bounds.max_y = src.bounds.max_y; dst.bounds.max_z = src.bounds.max_z;
        dst.sphere.cx = src.sphere.cx; dst.sphere.cy = src.sphere.cy; dst.sphere.cz = src.sphere.cz; dst.sphere.radius = src.sphere.radius;
    }
    lod.bounds.min_x = bounds.min_x; lod.bounds.min_y = bounds.min_y; lod.bounds.min_z = bounds.min_z;
    lod.bounds.max_x = bounds.max_x; lod.bounds.max_y = bounds.max_y; lod.bounds.max_z = bounds.max_z;
    lod.sphere.cx = sphere.cx; lod.sphere.cy = sphere.cy; lod.sphere.cz = sphere.cz; lod.sphere.radius = sphere.radius;
    return mesh;
}

std::unique_ptr<MeshAsset> MeshAsset::from_static_mesh(const rendering::StaticMesh& mesh, AssetId id, const std::string& logical_path) {
    auto asset = std::make_unique<MeshAsset>();
    asset->id = id;
    asset->logical_path = logical_path;
    if (!mesh.lods().empty()) {
        const auto& lod = mesh.lods()[0];
        asset->vertices.resize(lod.vertices.size());
        for (size_t i=0;i<lod.vertices.size();++i){
            auto& src = lod.vertices[i];
            auto& dst = asset->vertices[i];
            std::memcpy(dst.position, src.position, sizeof(dst.position));
            std::memcpy(dst.normal, src.normal, sizeof(dst.normal));
            std::memcpy(dst.tangent, src.tangent, sizeof(dst.tangent));
            std::memcpy(dst.uv0, src.uv0, sizeof(dst.uv0));
            std::memcpy(dst.uv1, src.uv1, sizeof(dst.uv1));
        }
        asset->indices = lod.indices;
        asset->submeshes.resize(lod.submeshes.size());
        for (size_t i=0;i<lod.submeshes.size();++i){
            auto& src = lod.submeshes[i];
            auto& dst = asset->submeshes[i];
            dst.index_offset = src.index_offset;
            dst.index_count = src.index_count;
            dst.vertex_offset = src.vertex_offset;
            dst.vertex_count = src.vertex_count;
            dst.material_slot = src.material_slot;
            dst.bounds.min_x = src.bounds.min_x; dst.bounds.min_y = src.bounds.min_y; dst.bounds.min_z = src.bounds.min_z;
            dst.bounds.max_x = src.bounds.max_x; dst.bounds.max_y = src.bounds.max_y; dst.bounds.max_z = src.bounds.max_z;
            dst.sphere.cx = src.sphere.cx; dst.sphere.cy = src.sphere.cy; dst.sphere.cz = src.sphere.cz; dst.sphere.radius = src.sphere.radius;
        }
        asset->bounds.min_x = lod.bounds.min_x; asset->bounds.min_y = lod.bounds.min_y; asset->bounds.min_z = lod.bounds.min_z;
        asset->bounds.max_x = lod.bounds.max_x; asset->bounds.max_y = lod.bounds.max_y; asset->bounds.max_z = lod.bounds.max_z;
        asset->sphere.cx = lod.sphere.cx; asset->sphere.cy = lod.sphere.cy; asset->sphere.cz = lod.sphere.cz; asset->sphere.radius = lod.sphere.radius;
    }
    return asset;
}

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
