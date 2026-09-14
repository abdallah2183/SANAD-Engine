#pragma once

// NF/Rendering/RenderWorld.hpp — Render World (only rendering data, double buffered)
//
// Game World contains everything (Transform, Mesh, Material, Health, Inventory, Quest, etc.)
// Render World contains only what the renderer needs (transform, mesh, material,
// bounds, visibility...). This separation is the foundation for multithreaded
// extraction and for not leaking gameplay state into the renderer.
//
// The hard boundary is the extraction step:
//
//   ECS / Game World → Extraction → RenderWorld → Culling → Draw Submission → Renderer
//
// Nothing past Extraction may read gameplay components.

#include <NF/Core/Types.hpp>
#include <NF/Rendering/Camera.hpp>     // Mat4
#include <NF/Rendering/Handles.hpp>
#include <NF/Rendering/StaticMesh.hpp> // AABB / BoundingSphere

#include <mutex>
#include <string>
#include <vector>

namespace nf::rendering {

// Minimal transform for the legacy game:: path — in a real engine this is a
// full Mat4 (see RenderObject::world below, which is what the new pipeline
// reads). Named RenderTransform (not Transform) so it never shadows the ECS
// scene::Transform in translation units that use both namespaces.
struct RenderTransform {
    float x = 0, y = 0, z = 0;
    float scale = 1.0f;
};

// Everything the renderer needs to draw one object. The legacy fields
// (transform / mesh / material strings) serve the minimal game:: extraction
// path and its tests; the handle-based fields are what the real pipeline
// (extract_render_objects → cull → Renderer3D) consumes.
struct RenderObject {
    u32 id = 0;
    bool visible = true;

    // --- legacy translation-only fields (game:: extraction path) ---
    RenderTransform transform;
    std::string mesh;      // Mesh asset id / name
    std::string material;  // Material name

    // --- pipeline fields (ECS extraction path) ---
    Mat4 world;                    // world transform (translation only for now)
    StaticMeshHandle mesh_handle;
    MaterialHandle material_handle;
    AABB bounds;                   // world-space AABB
    BoundingSphere sphere;         // world-space bounding sphere
    u32 lod = 0;
};

struct RenderWorld {
    std::vector<RenderObject> objects;

    void clear() { objects.clear(); }
    size_t size() const { return objects.size(); }
};

// Double-buffered Render World so Game can write to back while Render reads front
class RenderWorldBuffer {
public:
    RenderWorldBuffer() = default;

    // Game thread writes to back, then swaps
    RenderWorld& back() { return m_back; }
    const RenderWorld& front() const { return m_front; }
    RenderWorld& front() { return m_front; }

    void swap() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::swap(m_front, m_back);
    }

    // For tests that don't need threading, direct access
    void clear_both() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_front.clear();
        m_back.clear();
    }

private:
    RenderWorld m_front;
    RenderWorld m_back;
    std::mutex m_mutex;
};

} // namespace nf::rendering
