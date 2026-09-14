#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Core/Logger.hpp>

namespace nf::rendering {

StaticMeshHandle MeshLibrary::add(std::unique_ptr<StaticMesh> mesh) {
    if (!mesh) return kInvalidMeshHandle;
    m_meshes.push_back(std::move(mesh));
    return StaticMeshHandle{static_cast<u32>(m_meshes.size() - 1)};
}

bool MeshLibrary::replace(StaticMeshHandle handle, std::unique_ptr<StaticMesh> mesh) {
    if (!handle.valid() || handle.id >= m_meshes.size() || !mesh) return false;
    m_meshes[handle.id] = std::move(mesh);
    return true;
}

StaticMesh* MeshLibrary::get(StaticMeshHandle handle) {
    if (!handle.valid() || handle.id >= m_meshes.size()) return nullptr;
    return m_meshes[handle.id].get();
}

const StaticMesh* MeshLibrary::get(StaticMeshHandle handle) const {
    if (!handle.valid() || handle.id >= m_meshes.size()) return nullptr;
    return m_meshes[handle.id].get();
}

bool MeshLibrary::upload_all(rhi::IGraphicsDevice& device) {
    for (auto& mesh : m_meshes) {
        if (!mesh) return false;
        if (mesh->is_uploaded()) continue;
        if (!mesh->upload(device)) {
            NF_LOG_ERROR(LogCategory::Core, "MeshLibrary: failed to upload mesh '{}'", mesh->name());
            return false;
        }
    }
    return true;
}

bool MeshLibrary::uploaded() const {
    for (const auto& mesh : m_meshes) {
        if (!mesh || !mesh->is_uploaded()) return false;
    }
    return !m_meshes.empty();
}

} // namespace nf::rendering
