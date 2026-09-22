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
//   - Skins: joints, root, inverse bind matrices; per-vertex JOINTS_0 /
//     WEIGHTS_0 land in mesh_skins (parallel to meshes). A mesh without a
//     skin binding is reported as skin_index == -1 — static is a fact, not a
//     fallback (no silent substitution).
//   - Animations: channels for translation/rotation/scale samplers. STEP and
//     LINEAR are imported; CUBICSPLINE and morph-weight channels are counted
//     in anim_channels_skipped rather than silently dropped.
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
    int skin_index = -1; // skin bound to this node, -1 when none
};

/// Which local transform component an animation channel drives.
enum class GltfAnimationPath {
    Translation,
    Rotation,
    Scale,
};

/// One animation channel: the sampled curve for (node, path). Times are in
/// seconds and sorted ascending; values are 3 floats per key (TRS paths) or
/// 4 floats per key (rotation, xyzw quaternion order, matching glTF).
struct GltfAnimationChannel {
    int node = -1;
    GltfAnimationPath path = GltfAnimationPath::Translation;
    std::vector<float> times;
    std::vector<float> values; // component count per key (3, or 4 for Rotation)
};

/// One glTF animation: a flat list of channels plus the duration implied by
/// the samplers (glTF has no explicit clip duration; this is the maximum
/// input time across all channels).
struct GltfAnimationInfo {
    std::string name;
    float duration = 0.0f;
    std::vector<GltfAnimationChannel> channels;
};

/// One glTF skin: the joint nodes in skin order, the skin's root node, and
/// the inverse bind matrices (16 floats per joint, column-major as in glTF).
struct GltfSkinInfo {
    std::string name;
    std::vector<int> joint_nodes; // node indices, glTF joint order
    int root_node = -1;           // -1 when the file does not name a skeleton root
    std::vector<float> inverse_bind_matrices; // 16 per joint
};

/// Per-vertex skin binding for the mesh at the matching index of
/// `GltfImportResult::meshes`. `skin_index == -1` means the mesh is static —
/// recorded explicitly, never substituted with a fake rig.
struct GltfMeshSkin {
    int skin_index = -1;
    u32 vertex_count = 0;
    std::vector<u16> joints;  // 4 per vertex, joint indices into GltfSkinInfo
    std::vector<float> weights; // 4 per vertex, normalized to sum 1
};

struct GltfImportResult {
    std::vector<std::unique_ptr<MeshAsset>> meshes; // one per converted glTF mesh
    std::vector<GltfMaterialInfo> materials;        // glTF material order
    std::vector<GltfNodeInfo> nodes;                // glTF node order
    std::vector<GltfSkinInfo> skins;                // glTF skin order
    std::vector<GltfAnimationInfo> animations;      // glTF animation order
    std::vector<GltfMeshSkin> mesh_skins;           // parallel to meshes
    u32 primitives_skipped = 0; // non-triangle / unusable primitives
    u32 anim_channels_skipped = 0; // unsupported channels (CUBICSPLINE, morph weights)
    u32 skin_bindings_rejected = 0; // primitives with malformed JOINTS_0/WEIGHTS_0
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
