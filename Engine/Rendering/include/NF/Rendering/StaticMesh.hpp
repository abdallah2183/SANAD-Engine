#pragma once

// NF/Rendering/StaticMesh.hpp — Production Static Mesh (RHI-independent asset)

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nf::rendering {

struct AABB {
    float min_x = 0, min_y = 0, min_z = 0;
    float max_x = 0, max_y = 0, max_z = 0;

    bool contains(float x, float y, float z) const {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y && z >= min_z && z <= max_z;
    }
    float center_x() const { return (min_x + max_x) * 0.5f; }
    float center_y() const { return (min_y + max_y) * 0.5f; }
    float center_z() const { return (min_z + max_z) * 0.5f; }
    float extent_x() const { return (max_x - min_x) * 0.5f; }
    float extent_y() const { return (max_y - min_y) * 0.5f; }
    float extent_z() const { return (max_z - min_z) * 0.5f; }
};

struct BoundingSphere {
    float cx = 0, cy = 0, cz = 0;
    float radius = 0;
};

struct Vertex {
    float position[3] = {0,0,0};
    float normal[3] = {0,0,1};
    float tangent[4] = {1,0,0,1};
    float uv0[2] = {0,0};
    float uv1[2] = {0,0}; // optional
};

struct SubMesh {
    u32 index_offset = 0;
    u32 index_count = 0;
    u32 vertex_offset = 0;
    u32 vertex_count = 0;
    u32 material_slot = 0; // index into the owning mesh's material_slots
    AABB bounds;
    BoundingSphere sphere;
};

/// One draw-visible surface group of a mesh. A SubMesh references a slot by
/// index; the slot carries the identity of the material to use. Slots keep
/// meshes decoupled from any specific Material instance.
struct MaterialSlot {
    std::string name = "default";
};

struct MeshLOD {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::vector<SubMesh> submeshes;
    std::vector<MaterialSlot> material_slots;
    AABB bounds;
    BoundingSphere sphere;
};

// StaticMesh — CPU asset + GPU buffers (uploaded via RHI)
class StaticMesh {
public:
    explicit StaticMesh(std::string name = "Unnamed");

    const std::string& name() const { return m_name; }
    void set_name(const std::string& n) { m_name = n; }

    // CPU data
    std::vector<MeshLOD>& lods() { return m_lods; }
    const std::vector<MeshLOD>& lods() const { return m_lods; }
    MeshLOD& lod(u32 idx = 0);
    const MeshLOD& lod(u32 idx = 0) const;

    AABB bounds() const;
    BoundingSphere bounding_sphere() const;

    // GPU upload — creates vertex/index buffers for each LOD (uses UploadContext internally, no vkQueueWaitIdle in hot path)
    bool upload(rhi::IGraphicsDevice& device);
    bool is_uploaded() const { return m_uploaded; }

    // GPU buffers for a given LOD (valid after upload)
    rhi::Buffer* vertex_buffer(u32 lod = 0);
    rhi::Buffer* index_buffer(u32 lod = 0);
    const rhi::Buffer* vertex_buffer(u32 lod = 0) const;
    const rhi::Buffer* index_buffer(u32 lod = 0) const;

    // Procedural helpers
    static std::unique_ptr<StaticMesh> create_cube(float size = 1.0f);
    static std::unique_ptr<StaticMesh> create_quad(float size = 1.0f);
    static std::unique_ptr<StaticMesh> create_sphere(float radius = 0.5f, u32 segments = 16);

private:
    void compute_bounds(MeshLOD& lod);

    std::string m_name;
    std::vector<MeshLOD> m_lods;
    std::vector<std::unique_ptr<rhi::Buffer>> m_vertex_buffers;
    std::vector<std::unique_ptr<rhi::Buffer>> m_index_buffers;
    bool m_uploaded = false;
};

} // namespace nf::rendering
