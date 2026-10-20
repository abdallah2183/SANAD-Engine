#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf::rendering {

StaticMesh::StaticMesh(std::string name) : m_name(std::move(name)) {
    m_lods.emplace_back(); // default LOD 0
}

MeshLOD& StaticMesh::lod(u32 idx) {
    if (idx >= m_lods.size()) m_lods.resize(idx + 1);
    return m_lods[idx];
}

const MeshLOD& StaticMesh::lod(u32 idx) const {
    // The const overload cannot grow the LOD list (the non-const one does),
    // so an out-of-range index returns a shared empty LOD instead of
    // reading past the end of the vector.
    static const MeshLOD empty{};
    if (idx >= m_lods.size()) return empty;
    return m_lods[idx];
}

AABB StaticMesh::bounds() const {
    if (m_lods.empty()) return {};
    return m_lods[0].bounds;
}

BoundingSphere StaticMesh::bounding_sphere() const {
    if (m_lods.empty()) return {};
    return m_lods[0].sphere;
}

void StaticMesh::compute_lod_bounds(MeshLOD& lod) {
    if (lod.vertices.empty()) {
        lod.bounds = {};
        lod.sphere = {};
        return;
    }
    float min_x = lod.vertices[0].position[0], min_y = lod.vertices[0].position[1], min_z = lod.vertices[0].position[2];
    float max_x = min_x, max_y = min_y, max_z = min_z;
    for (auto& v : lod.vertices) {
        min_x = std::min(min_x, v.position[0]);
        min_y = std::min(min_y, v.position[1]);
        min_z = std::min(min_z, v.position[2]);
        max_x = std::max(max_x, v.position[0]);
        max_y = std::max(max_y, v.position[1]);
        max_z = std::max(max_z, v.position[2]);
    }
    lod.bounds.min_x = min_x; lod.bounds.min_y = min_y; lod.bounds.min_z = min_z;
    lod.bounds.max_x = max_x; lod.bounds.max_y = max_y; lod.bounds.max_z = max_z;
    // Sphere: center = AABB center, radius = max distance to corner
    float cx = (min_x + max_x) * 0.5f;
    float cy = (min_y + max_y) * 0.5f;
    float cz = (min_z + max_z) * 0.5f;
    float max_dist_sq = 0;
    for (auto& v : lod.vertices) {
        float dx = v.position[0] - cx;
        float dy = v.position[1] - cy;
        float dz = v.position[2] - cz;
        float d2 = dx*dx + dy*dy + dz*dz;
        max_dist_sq = std::max(max_dist_sq, d2);
    }
    lod.sphere.cx = cx; lod.sphere.cy = cy; lod.sphere.cz = cz;
    lod.sphere.radius = std::sqrt(max_dist_sq);
    for (auto& sm : lod.submeshes) {
        sm.bounds = lod.bounds;
        sm.sphere = lod.sphere;
    }
}

void StaticMesh::compute_bounds(MeshLOD& lod) {
    compute_lod_bounds(lod);
    if (lod.vertices.empty()) return;
    // A mesh built procedurally gets a single default submesh + material slot
    // so it is drawable the moment it is created (not only after upload()).
    if (lod.submeshes.empty()) {
        SubMesh sm;
        sm.index_offset = 0;
        sm.index_count = static_cast<u32>(lod.indices.size());
        sm.vertex_offset = 0;
        sm.vertex_count = static_cast<u32>(lod.vertices.size());
        sm.material_slot = 0;
        lod.submeshes.push_back(sm);
    }
    if (lod.material_slots.empty()) {
        lod.material_slots.push_back(MaterialSlot{"default"});
    }
    // Submesh bounds follow the LOD bounds for now
    for (auto& sm : lod.submeshes) {
        sm.bounds = lod.bounds;
        sm.sphere = lod.sphere;
    }
}

bool StaticMesh::upload(rhi::IGraphicsDevice& device, bool dynamic_vertices) {
    if (m_lods.empty()) {
        NF_LOG_ERROR(LogCategory::Core, "StaticMesh '{}' has no LODs", m_name);
        return false;
    }
    m_vertex_buffers.clear();
    m_index_buffers.clear();
    m_vertex_buffers.reserve(m_lods.size());
    m_index_buffers.reserve(m_lods.size());

    // One upload context for the whole mesh, ONE submit at the end.
    // Buffer::update() on a GPU-only buffer stages the copy and ends in
    // vkQueueWaitIdle (Device_Vk.cpp), so uploading the vertex and the
    // index buffer of every LOD that way stalls the GPU twice per LOD. The
    // upload context copies on the GPU and returns a fence instead, which
    // is what the header promises ("no vkQueueWaitIdle in hot path").
    auto upload = device.create_upload_context();
    if (!upload) {
        NF_LOG_ERROR(LogCategory::Core, "StaticMesh '{}': no upload context available", m_name);
        return false;
    }
    // The staging buffers have to outlive the copy: the GPU reads them
    // asynchronously, so they are kept alive until the fence has signalled.
    std::vector<std::unique_ptr<rhi::Buffer>> staging_buffers;
    staging_buffers.reserve(m_lods.size() * 2);

    for (size_t i = 0; i < m_lods.size(); ++i) {
        auto& lod = m_lods[i];
        if (lod.vertices.empty() || lod.indices.empty()) {
            NF_LOG_WARN(LogCategory::Core, "StaticMesh '{}' LOD {} is empty", m_name, i);
            m_vertex_buffers.push_back(nullptr);
            m_index_buffers.push_back(nullptr);
            continue;
        }
        compute_bounds(lod);
        if (lod.submeshes.empty()) {
            SubMesh sm;
            sm.index_offset = 0;
            sm.index_count = static_cast<u32>(lod.indices.size());
            sm.vertex_offset = 0;
            sm.vertex_count = static_cast<u32>(lod.vertices.size());
            sm.material_slot = 0;
            sm.bounds = lod.bounds;
            sm.sphere = lod.sphere;
            lod.submeshes.push_back(sm);
        }
        // Every submesh must reference a valid slot; meshes built without
        // explicit slots get a single default one.
        if (lod.material_slots.empty()) {
            lod.material_slots.push_back(MaterialSlot{"default"});
        }

        size_t vb_size = lod.vertices.size() * sizeof(Vertex);
        size_t ib_size = lod.indices.size() * sizeof(u32);

        rhi::BufferDesc vb_desc{};
        vb_desc.size = vb_size;
        vb_desc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
        // A dynamic mesh's vertices are rewritten every frame, and the
        // per-frame path is Buffer::update(): a device-local buffer would
        // stage the copy and wait for the queue, which is a full GPU stall
        // per frame. Host-visible memory makes it a memcpy.
        vb_desc.memory = dynamic_vertices ? rhi::MemoryUsage::CPUToGPU
                                          : rhi::MemoryUsage::GPUOnly;
        auto vb = device.create_buffer(vb_desc);
        if (!vb) return false;

        rhi::BufferDesc ib_desc{};
        ib_desc.size = ib_size;
        ib_desc.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
        ib_desc.memory = rhi::MemoryUsage::GPUOnly;
        auto ib = device.create_buffer(ib_desc);
        if (!ib) return false;

        // Stage the vertices and the indices, then hand both copies to the
        // upload context. The staging buffers are host-visible, so writing
        // into them is a plain memcpy — no GPU stall.
        rhi::BufferDesc staging_desc{};
        staging_desc.usage = rhi::BufferUsage::TransferSrc;
        staging_desc.memory = rhi::MemoryUsage::CPUToGPU;

        staging_desc.size = vb_size;
        auto vb_staging = device.create_buffer(staging_desc);
        if (!vb_staging) return false;
        vb_staging->update(lod.vertices.data(), 0, vb_size);

        staging_desc.size = ib_size;
        auto ib_staging = device.create_buffer(staging_desc);
        if (!ib_staging) return false;
        ib_staging->update(lod.indices.data(), 0, ib_size);

        upload->copy_buffer(*vb_staging, *vb, 0, 0, vb_size);
        upload->copy_buffer(*ib_staging, *ib, 0, 0, ib_size);

        staging_buffers.push_back(std::move(vb_staging));
        staging_buffers.push_back(std::move(ib_staging));

        m_vertex_buffers.push_back(std::move(vb));
        m_index_buffers.push_back(std::move(ib));
    }

    // One submit for the whole mesh: the GPU copies every buffer in one go,
    // and the fence tells us when the staging buffers may be released.
    auto fence = upload->submit();
    if (!fence) {
        NF_LOG_ERROR(LogCategory::Core, "StaticMesh '{}': upload submit failed", m_name);
        return false;
    }
    if (!fence->wait()) {
        NF_LOG_ERROR(LogCategory::Core, "StaticMesh '{}': upload did not complete", m_name);
        return false;
    }

    m_uploaded = true;
    NF_LOG_INFO(LogCategory::Core, "StaticMesh '{}' uploaded ({} LODs, {} verts, {} indices)", m_name, m_lods.size(), m_lods[0].vertices.size(), m_lods[0].indices.size());
    return true;
}

rhi::Buffer* StaticMesh::vertex_buffer(u32 lod) {
    if (lod >= m_vertex_buffers.size()) return nullptr;
    return m_vertex_buffers[lod].get();
}
rhi::Buffer* StaticMesh::index_buffer(u32 lod) {
    if (lod >= m_index_buffers.size()) return nullptr;
    return m_index_buffers[lod].get();
}
const rhi::Buffer* StaticMesh::vertex_buffer(u32 lod) const {
    if (lod >= m_vertex_buffers.size()) return nullptr;
    return m_vertex_buffers[lod].get();
}
const rhi::Buffer* StaticMesh::index_buffer(u32 lod) const {
    if (lod >= m_index_buffers.size()) return nullptr;
    return m_index_buffers[lod].get();
}

std::unique_ptr<StaticMesh> StaticMesh::create_cube(float size) {
    auto mesh = std::make_unique<StaticMesh>("Cube");
    auto& lod = mesh->lod(0);
    float hs = size * 0.5f;
    // 8 corners
    struct Pos { float x,y,z; };
    Pos corners[8] = {
        {-hs,-hs,-hs}, { hs,-hs,-hs}, { hs, hs,-hs}, {-hs, hs,-hs},
        {-hs,-hs, hs}, { hs,-hs, hs}, { hs, hs, hs}, {-hs, hs, hs}
    };
    struct Face { int idx[4]; float nx,ny,nz; };
    Face faces[6] = {
        {{0,1,2,3},  0,0,-1}, // -Z
        {{5,4,7,6},  0,0, 1}, // +Z
        {{4,0,3,7}, -1,0, 0}, // -X
        {{1,5,6,2},  1,0, 0}, // +X
        {{3,2,6,7},  0,1, 0}, // +Y
        {{4,5,1,0},  0,-1,0}, // -Y
    };
    for (auto& f : faces) {
        u32 base = static_cast<u32>(lod.vertices.size());
        for (int i=0;i<4;++i){
            Vertex v{};
            v.position[0]=corners[f.idx[i]].x; v.position[1]=corners[f.idx[i]].y; v.position[2]=corners[f.idx[i]].z;
            v.normal[0]=f.nx; v.normal[1]=f.ny; v.normal[2]=f.nz;
            v.tangent[0]=1.0f; v.tangent[1]=0.0f; v.tangent[2]=0.0f; v.tangent[3]=1.0f;
            v.uv0[0]=(i==0||i==3)?0.0f:1.0f; v.uv0[1]=(i==0||i==1)?0.0f:1.0f;
            lod.vertices.push_back(v);
        }
        lod.indices.push_back(base+0); lod.indices.push_back(base+1); lod.indices.push_back(base+2);
        lod.indices.push_back(base+2); lod.indices.push_back(base+3); lod.indices.push_back(base+0);
    }
    mesh->compute_bounds(lod);
    return mesh;
}

std::unique_ptr<StaticMesh> StaticMesh::create_quad(float size) {
    auto mesh = std::make_unique<StaticMesh>("Quad");
    auto& lod = mesh->lod(0);
    float hs = size * 0.5f;
    Vertex v0{}; v0.position[0]=-hs; v0.position[1]=-hs; v0.position[2]=0; v0.normal[2]=1; v0.uv0[0]=0; v0.uv0[1]=0;
    Vertex v1{}; v1.position[0]= hs; v1.position[1]=-hs; v1.position[2]=0; v1.normal[2]=1; v1.uv0[0]=1; v1.uv0[1]=0;
    Vertex v2{}; v2.position[0]= hs; v2.position[1]= hs; v2.position[2]=0; v2.normal[2]=1; v2.uv0[0]=1; v2.uv0[1]=1;
    Vertex v3{}; v3.position[0]=-hs; v3.position[1]= hs; v3.position[2]=0; v3.normal[2]=1; v3.uv0[0]=0; v3.uv0[1]=1;
    for (auto* v : {&v0,&v1,&v2,&v3}) { v->tangent[0]=1; v->tangent[3]=1; }
    lod.vertices = {v0,v1,v2,v3};
    // Wound to match create_cube's +Z face (bottom-right, bottom-left, top-left,
    // top-right, not the reverse): Mat4::perspective negates m[1][1], so with
    // FrontFace::CW a face whose vertex order reads counterclockwise in world XY
    // is the one visible from +Z. The natural (0,1,2) order has the opposite
    // sense, and the quad was back-face culled from every camera on the +Z side
    // — which is the only thing create_quad is ever used for.
    lod.indices = {1,0,3, 3,2,1};
    mesh->compute_bounds(lod);
    return mesh;
}

std::unique_ptr<StaticMesh> StaticMesh::create_sphere(float radius, u32 segments) {
    auto mesh = std::make_unique<StaticMesh>("Sphere");
    auto& lod = mesh->lod(0);
    if (segments < 3) segments = 8;
    for (u32 y=0; y<=segments; ++y){
        float v = float(y)/segments;
        float phi = v * 3.14159265359f;
        for (u32 x=0; x<=segments; ++x){
            float u = float(x)/segments;
            float theta = u * 2.0f * 3.14159265359f;
            float sx = std::sin(phi) * std::cos(theta);
            float sy = std::cos(phi);
            float sz = std::sin(phi) * std::sin(theta);
            Vertex vert{};
            vert.position[0]=sx*radius; vert.position[1]=sy*radius; vert.position[2]=sz*radius;
            vert.normal[0]=sx; vert.normal[1]=sy; vert.normal[2]=sz;
            vert.tangent[0]= -std::sin(theta); vert.tangent[1]=0; vert.tangent[2]= std::cos(theta); vert.tangent[3]=1;
            vert.uv0[0]=u; vert.uv0[1]=v;
            lod.vertices.push_back(vert);
        }
    }
    for (u32 y=0; y<segments; ++y){
        for (u32 x=0; x<segments; ++x){
            u32 i0 = y*(segments+1)+x;
            u32 i1 = i0+1;
            u32 i2 = (y+1)*(segments+1)+x;
            u32 i3 = i2+1;
            lod.indices.push_back(i0); lod.indices.push_back(i2); lod.indices.push_back(i1);
            lod.indices.push_back(i1); lod.indices.push_back(i2); lod.indices.push_back(i3);
        }
    }
    mesh->compute_bounds(lod);
    return mesh;
}

} // namespace nf::rendering
