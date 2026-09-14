#pragma once

#include <NF/Assets/AssetId.hpp>
#include <NF/Core/Types.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nf::assets {

// Cooked mesh vertex — must match rendering::Vertex layout (position, normal, tangent, uv0, uv1)
struct AssetVertex {
    float position[3] = {0,0,0};
    float normal[3] = {0,0,1};
    float tangent[4] = {1,0,0,1};
    float uv0[2] = {0,0};
    float uv1[2] = {0,0};
};

struct AssetAABB {
    float min_x=0, min_y=0, min_z=0;
    float max_x=0, max_y=0, max_z=0;
};

struct AssetSphere {
    float cx=0, cy=0, cz=0;
    float radius=0;
};

struct AssetSubMesh {
    uint32_t index_offset=0;
    uint32_t index_count=0;
    uint32_t vertex_offset=0;
    uint32_t vertex_count=0;
    uint32_t material_slot=0;
    AssetAABB bounds{};
    AssetSphere sphere{};
};

// Cooked mesh format .nfmesh (v1) — CPU-pure data since Phase 11 W1.
//
// The conversions to and from rendering::StaticMesh deliberately do NOT live
// here: they are rendering::make_mesh_asset / rendering::make_static_mesh
// (NF/Rendering/MeshUpload.hpp), because a class that turns meshes into assets
// has to know the mesh type, and this module must not. Before W1 the
// conversions were members and dragged Engine/Rendering into Engine/Assets.
struct MeshAsset {
    AssetId id;
    std::string logical_path;
    std::vector<AssetVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<AssetSubMesh> submeshes;
    AssetAABB bounds;
    AssetSphere sphere;
    uint32_t format_version = 1;

    bool save_to_bytes(std::vector<uint8_t>& out) const;
    static std::unique_ptr<MeshAsset> load_from_bytes(std::span<const uint8_t> data, std::string& out_error);
    bool save_to_file(const std::string& physical_path, std::string& out_error) const;
    static std::unique_ptr<MeshAsset> load_from_file(const std::string& physical_path, std::string& out_error);
};

} // namespace nf::assets
