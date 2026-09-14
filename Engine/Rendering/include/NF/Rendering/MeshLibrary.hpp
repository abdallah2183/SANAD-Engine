#pragma once

// NF/Rendering/MeshLibrary.hpp — owns runtime StaticMeshes behind handles
//
// Mesh asset → Cook/Import → Runtime Mesh → GPU Upload:
//   - Assets are cooked/imported into StaticMesh objects (see MeshAsset.hpp)
//   - The library takes ownership and hands out StaticMeshHandles
//   - upload_all() pushes every mesh to the GPU through the RHI
//
// The library is deliberately RHI-agnostic: it only forwards to
// StaticMesh::upload(), so the mesh abstraction never binds to Vulkan.

#include <NF/Core/Types.hpp>
#include <NF/Rendering/Handles.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <memory>
#include <vector>

namespace nf::rendering {

class MeshLibrary {
public:
    MeshLibrary() = default;

    MeshLibrary(const MeshLibrary&) = delete;
    MeshLibrary& operator=(const MeshLibrary&) = delete;

    /// Takes ownership. Returns the handle the mesh is known by from now on.
    StaticMeshHandle add(std::unique_ptr<StaticMesh> mesh);

    /// Replaces the mesh behind a handle in place (Hot Reload): every other
    /// handle stays valid. The old mesh is destroyed on assignment, so the
    /// caller must guarantee the GPU is idle first.
    bool replace(StaticMeshHandle handle, std::unique_ptr<StaticMesh> mesh);

    StaticMesh* get(StaticMeshHandle handle);
    const StaticMesh* get(StaticMeshHandle handle) const;

    /// Uploads every not-yet-uploaded mesh to the GPU. Returns false if any
    /// upload fails. Safe to call again — already-uploaded meshes are skipped.
    bool upload_all(rhi::IGraphicsDevice& device);

    bool uploaded() const;

    size_t size() const { return m_meshes.size(); }

private:
    std::vector<std::unique_ptr<StaticMesh>> m_meshes;
};

} // namespace nf::rendering
