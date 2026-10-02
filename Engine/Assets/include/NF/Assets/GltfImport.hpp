#pragma once

// NF/Assets/GltfImport.hpp — glTF 2.0 import pipeline (Phase 14).
//
// Parses .gltf (JSON + external .bin / embedded base64) and .glb, and converts
// mesh data into the engine's CPU-pure MeshAsset (ready to cook to .nfmesh or
// to turn into a rendering::StaticMesh via rendering::make_static_mesh).
//
// The payload types (GltfMaterialInfo, GltfNodeInfo, GltfSkinInfo,
// GltfAnimationInfo, GltfMeshSkin) and the result now live in MeshImport.hpp,
// which owns the read side for every mesh format. `GltfImportResult` survives
// as an alias, so glTF callers read the same result the unified dispatcher
// publishes — see MeshImport.hpp for the full format table and the
// no-silent-substitution ledger.
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
//   - Animations: LINEAR translation/rotation/scale channels are imported.
//     STEP is rejected rather than silently changed to linear interpolation;
//     CUBICSPLINE, STEP, and morph-weight channels are counted in
//     anim_channels_skipped.
//   - Memory import resolves embedded (base64) buffers only; external .bin
//     references need import_gltf_file with the .gltf path beside its .bin.
//
// All functions are synchronous and thread-safe (no shared state). Failure
// yields ok=false plus a human-readable error; nothing throws.

#include <NF/Assets/MeshImport.hpp>

namespace nf::assets {

/// The glTF reader publishes the unified import payload. The alias is kept so
/// that "a glTF import result" and "an import result" are never two different
/// types a caller has to convert between.
using GltfImportResult = MeshImportResult;

/// Parses glTF/GLB bytes. External buffer files cannot resolve from memory;
/// embedded base64 buffers work.
GltfImportResult import_gltf_memory(const void* data, usize size,
                                    const std::string& logical_path = "<memory>");

/// Loads `path` (.gltf or .glb); sibling .bin files resolve beside it.
GltfImportResult import_gltf_file(const std::string& path);

} // namespace nf::assets
