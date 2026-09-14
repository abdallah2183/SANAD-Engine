#pragma once

// NF/Rendering/MeshUpload.hpp — the two directions of the mesh asset boundary.
//
// Phase 11, W1. assets::MeshAsset is CPU-pure data (bytes in, bytes out) and
// knows nothing about the renderer. Everything that turns a StaticMesh into an
// asset, or an asset back into a StaticMesh, lives here, on the rendering side
// of the boundary — the layer that legitimately knows both types.
//
// Before W1 these were MeshAsset::from_static_mesh / to_static_mesh, member
// functions that forced Engine/Assets to include Engine/Rendering headers and
// (via AssetManager's device) NF/RHI as well. One type, one owner: the mesh
// type belongs to Rendering, the file format belongs to Assets, and the glue
// lives with the type that has to know both.

#include <NF/Assets/MeshAsset.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <memory>
#include <string>

namespace nf::rendering {

/// StaticMesh -> MeshAsset (the cook direction). Flattens LOD 0.
std::unique_ptr<assets::MeshAsset> make_mesh_asset(const StaticMesh& mesh, assets::AssetId id,
                                                   const std::string& logical_path);

/// MeshAsset -> StaticMesh (the runtime direction). The GPU upload is a
/// separate, explicit step (StaticMesh::upload): a CPU asset is loadable and
/// comparable without a device, which is the point of W1.
std::unique_ptr<StaticMesh> make_static_mesh(const assets::MeshAsset& asset, const std::string& name);

} // namespace nf::rendering
