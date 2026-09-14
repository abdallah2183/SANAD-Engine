#include <NF/Rendering/MeshUpload.hpp>

#include <cstring>

namespace nf::rendering {

std::unique_ptr<assets::MeshAsset> make_mesh_asset(const StaticMesh& mesh, assets::AssetId id,
                                                   const std::string& logical_path) {
    auto asset = std::make_unique<assets::MeshAsset>();
    asset->id = id;
    asset->logical_path = logical_path;
    if (!mesh.lods().empty()) {
        const auto& lod = mesh.lods()[0];
        asset->vertices.resize(lod.vertices.size());
        for (size_t i = 0; i < lod.vertices.size(); ++i) {
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
        for (size_t i = 0; i < lod.submeshes.size(); ++i) {
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

std::unique_ptr<StaticMesh> make_static_mesh(const assets::MeshAsset& asset, const std::string& name) {
    auto mesh = std::make_unique<StaticMesh>(name.empty() ? asset.logical_path : name);
    auto& lod = mesh->lod(0);
    lod.vertices.resize(asset.vertices.size());
    for (size_t i = 0; i < asset.vertices.size(); ++i) {
        auto& src = asset.vertices[i];
        auto& dst = lod.vertices[i];
        std::memcpy(dst.position, src.position, sizeof(dst.position));
        std::memcpy(dst.normal, src.normal, sizeof(dst.normal));
        std::memcpy(dst.tangent, src.tangent, sizeof(dst.tangent));
        std::memcpy(dst.uv0, src.uv0, sizeof(dst.uv0));
        std::memcpy(dst.uv1, src.uv1, sizeof(dst.uv1));
    }
    lod.indices = asset.indices;
    lod.submeshes.resize(asset.submeshes.size());
    for (size_t i = 0; i < asset.submeshes.size(); ++i) {
        auto& src = asset.submeshes[i];
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
    lod.bounds.min_x = asset.bounds.min_x; lod.bounds.min_y = asset.bounds.min_y; lod.bounds.min_z = asset.bounds.min_z;
    lod.bounds.max_x = asset.bounds.max_x; lod.bounds.max_y = asset.bounds.max_y; lod.bounds.max_z = asset.bounds.max_z;
    lod.sphere.cx = asset.sphere.cx; lod.sphere.cy = asset.sphere.cy; lod.sphere.cz = asset.sphere.cz; lod.sphere.radius = asset.sphere.radius;
    return mesh;
}

} // namespace nf::rendering
