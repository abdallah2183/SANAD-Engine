#pragma once

// NF/Assets/GltfImport.hpp — glTF 2.0 import pipeline (Phase 14).
//
// Parses .gltf (JSON + external .bin / embedded base64) and .glb, and converts
// mesh data into the engine's CPU-pure MeshAsset (ready to cook to .nfmesh or
// to turn into a rendering::StaticMesh via rendering::make_static_mesh).
//
// Scope (documented, not accidental):
//   - Triangle primitives only; other modes are counted in
//     primitives_skipped and ignored (no silent mis-render).
//   - POSITION (float vec3) is required; NORMAL/TEXCOORD_0/TANGENT are
//     optional — missing normals are computed smooth, the rest default.
//   - Materials: baseColor/metallic/roughness factors + name (textures are a
//     later phase; slots reference material order).
//   - Nodes: local TRS + mesh binding for scene instantiation.
//   - Memory import resolves embedded (base64) buffers only; external .bin
//     references need import_gltf_file with the .gltf path beside its .bin.
//
// All functions are synchronous and thread-safe (no shared state). Failure
// yields ok=false plus a human-readable error; nothing throws.

#include <NF/Assets/MeshAsset.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace nf::assets {

/// PBR material factors in glTF material order (slot i == materials[i]).
struct GltfMaterialInfo {
    std::string name;
    float base_color[4] = {1, 1, 1, 1};
    float metallic = 1.0f;
    float roughness = 1.0f;
};

/// One glTF node: local transform + optional mesh binding (index into the
/// result's meshes, -1 when the node carries no mesh).
struct GltfNodeInfo {
    std::string name;
    float translation[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1}; // xyzw quaternion
    float scale[3] = {1, 1, 1};
    int mesh_index = -1;
    int parent_index = -1;
};

struct GltfImportResult {
    std::vector<std::unique_ptr<MeshAsset>> meshes; // one per converted glTF mesh
    std::vector<GltfMaterialInfo> materials;        // glTF material order
    std::vector<GltfNodeInfo> nodes;                // glTF node order
    u32 primitives_skipped = 0; // non-triangle / unusable primitives
    bool ok = false;
    std::string error;
};

/// Parses glTF/GLB bytes. External buffer files cannot resolve from memory;
/// embedded base64 buffers work.
GltfImportResult import_gltf_memory(const void* data, usize size,
                                    const std::string& logical_path = "<memory>");

/// Loads `path` (.gltf or .glb); sibling .bin files resolve beside it.
GltfImportResult import_gltf_file(const std::string& path);

} // namespace nf::assets
