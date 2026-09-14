#include <NF/Rendering/MeshAsset.hpp>
#include <NF/Core/Logger.hpp>

#include <cstring>
#include <fstream>
#include <vector>

namespace nf::rendering::mesh_asset {

namespace {

template <typename T>
bool write_pod(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return out.good();
}

template <typename T>
bool read_pod(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return in.good();
}

bool write_u32(std::ofstream& out, u32 v) { return write_pod(out, v); }
bool read_u32(std::ifstream& in, u32& v) { return read_pod(in, v); }

bool write_f32(std::ofstream& out, float v) { return write_pod(out, v); }
bool read_f32(std::ifstream& in, float& v) { return read_pod(in, v); }

} // namespace

bool save_mesh_asset(const StaticMesh& mesh, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        NF_LOG_ERROR(LogCategory::Core, "save_mesh_asset: cannot open '{}'", path.string());
        return false;
    }

    out.write(kMagic, 4);

    const std::string& name = mesh.name();
    if (!write_u32(out, static_cast<u32>(name.size()))) return false;
    out.write(name.data(), static_cast<std::streamsize>(name.size()));

    const auto& lods = mesh.lods();
    if (!write_u32(out, static_cast<u32>(lods.size()))) return false;

    for (const MeshLOD& lod : lods) {
        // Vertices — field by field, never a raw struct dump
        if (!write_u32(out, static_cast<u32>(lod.vertices.size()))) return false;
        for (const Vertex& v : lod.vertices) {
            for (float f : v.position) if (!write_f32(out, f)) return false;
            for (float f : v.normal)   if (!write_f32(out, f)) return false;
            for (float f : v.tangent)  if (!write_f32(out, f)) return false;
            for (float f : v.uv0)      if (!write_f32(out, f)) return false;
            for (float f : v.uv1)      if (!write_f32(out, f)) return false;
        }

        if (!write_u32(out, static_cast<u32>(lod.indices.size()))) return false;
        for (u32 idx : lod.indices) {
            if (!write_u32(out, idx)) return false;
        }

        if (!write_u32(out, static_cast<u32>(lod.submeshes.size()))) return false;
        for (const SubMesh& sm : lod.submeshes) {
            if (!write_u32(out, sm.index_offset)) return false;
            if (!write_u32(out, sm.index_count)) return false;
            if (!write_u32(out, sm.vertex_offset)) return false;
            if (!write_u32(out, sm.vertex_count)) return false;
            if (!write_u32(out, sm.material_slot)) return false;
            const float aabb[6] = {sm.bounds.min_x, sm.bounds.min_y, sm.bounds.min_z,
                                   sm.bounds.max_x, sm.bounds.max_y, sm.bounds.max_z};
            for (float f : aabb) if (!write_f32(out, f)) return false;
            const float sph[4] = {sm.sphere.cx, sm.sphere.cy, sm.sphere.cz, sm.sphere.radius};
            for (float f : sph) if (!write_f32(out, f)) return false;
        }

        const float aabb[6] = {lod.bounds.min_x, lod.bounds.min_y, lod.bounds.min_z,
                               lod.bounds.max_x, lod.bounds.max_y, lod.bounds.max_z};
        for (float f : aabb) if (!write_f32(out, f)) return false;
        const float sph[4] = {lod.sphere.cx, lod.sphere.cy, lod.sphere.cz, lod.sphere.radius};
        for (float f : sph) if (!write_f32(out, f)) return false;
    }

    if (!out.good()) {
        NF_LOG_ERROR(LogCategory::Core, "save_mesh_asset: write failed for '{}'", path.string());
        return false;
    }
    return true;
}

bool load_mesh_asset(const std::filesystem::path& path, StaticMesh& out_mesh) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        NF_LOG_ERROR(LogCategory::Core, "load_mesh_asset: cannot open '{}'", path.string());
        return false;
    }

    char magic[4] = {};
    in.read(magic, 4);
    if (!in.good() || std::memcmp(magic, kMagic, 4) != 0) {
        NF_LOG_ERROR(LogCategory::Core, "load_mesh_asset: bad magic in '{}'", path.string());
        return false;
    }

    u32 name_len = 0;
    if (!read_u32(in, name_len) || name_len > 4096) return false;
    std::string name(name_len, '\0');
    if (name_len) in.read(name.data(), static_cast<std::streamsize>(name_len));
    if (!in.good()) return false;

    u32 lod_count = 0;
    if (!read_u32(in, lod_count) || lod_count == 0 || lod_count > 64) return false;

    out_mesh = StaticMesh(name);
    auto& lods = out_mesh.lods();
    lods.clear();

    for (u32 li = 0; li < lod_count; ++li) {
        MeshLOD lod;

        u32 vertex_count = 0;
        if (!read_u32(in, vertex_count) || vertex_count > 100'000'000u) return false;
        lod.vertices.resize(vertex_count);
        for (Vertex& v : lod.vertices) {
            for (float& f : v.position) if (!read_f32(in, f)) return false;
            for (float& f : v.normal)   if (!read_f32(in, f)) return false;
            for (float& f : v.tangent)  if (!read_f32(in, f)) return false;
            for (float& f : v.uv0)      if (!read_f32(in, f)) return false;
            for (float& f : v.uv1)      if (!read_f32(in, f)) return false;
        }

        u32 index_count = 0;
        if (!read_u32(in, index_count) || index_count > 300'000'000u) return false;
        lod.indices.resize(index_count);
        for (u32& idx : lod.indices) {
            if (!read_u32(in, idx)) return false;
        }

        u32 submesh_count = 0;
        if (!read_u32(in, submesh_count) || submesh_count > 1'000'000u) return false;
        lod.submeshes.resize(submesh_count);
        for (SubMesh& sm : lod.submeshes) {
            if (!read_u32(in, sm.index_offset)) return false;
            if (!read_u32(in, sm.index_count)) return false;
            if (!read_u32(in, sm.vertex_offset)) return false;
            if (!read_u32(in, sm.vertex_count)) return false;
            if (!read_u32(in, sm.material_slot)) return false;
            float aabb[6] = {};
            for (float& f : aabb) if (!read_f32(in, f)) return false;
            sm.bounds = AABB{aabb[0], aabb[1], aabb[2], aabb[3], aabb[4], aabb[5]};
            float sph[4] = {};
            for (float& f : sph) if (!read_f32(in, f)) return false;
            sm.sphere = BoundingSphere{sph[0], sph[1], sph[2], sph[3]};
        }

        float aabb[6] = {};
        for (float& f : aabb) if (!read_f32(in, f)) return false;
        lod.bounds = AABB{aabb[0], aabb[1], aabb[2], aabb[3], aabb[4], aabb[5]};
        float sph[4] = {};
        for (float& f : sph) if (!read_f32(in, f)) return false;
        lod.sphere = BoundingSphere{sph[0], sph[1], sph[2], sph[3]};

        lods.push_back(std::move(lod));
    }

    return in.good();
}

std::unique_ptr<StaticMesh> load_mesh_asset(const std::filesystem::path& path) {
    auto mesh = std::make_unique<StaticMesh>();
    if (!load_mesh_asset(path, *mesh)) return nullptr;
    return mesh;
}

} // namespace nf::rendering::mesh_asset
