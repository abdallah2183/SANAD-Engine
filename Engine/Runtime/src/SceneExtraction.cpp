// NF/Runtime/SceneExtraction.cpp — the game-world → render-world bridge (Phase 11, W2)

#include <NF/Runtime/SceneExtraction.hpp>

#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Scene/Transform.hpp>

namespace nf::runtime {

void extract_render_objects(const ecs::World& world, const rendering::MeshLibrary& meshes,
                            rendering::RenderWorld& out) {
    out.clear();

    for (ecs::Entity e : world.query<scene::Transform, rendering::MeshComponent>()) {
        const scene::Transform* tr = world.get<scene::Transform>(e);
        const rendering::MeshComponent* mc = world.get<rendering::MeshComponent>(e);
        if (tr == nullptr || mc == nullptr) continue;
        if (!mc->visible) continue;
        if (!mc->mesh.valid()) continue;

        const rendering::StaticMesh* mesh = meshes.get(mc->mesh);
        if (mesh == nullptr) continue;

        rendering::RenderObject ro;
        ro.id = e.id;
        ro.visible = true;

        ro.transform.x = tr->world_x;
        ro.transform.y = tr->world_y;
        ro.transform.z = tr->world_z;

        // `RenderObject::world` is rendering::Mat4 — the rendering module's own
        // flat column-major matrix (Camera.hpp), NOT nf::Mat4 (Core's row-major
        // [4][4]). Indices 12/13/14 are the translation column here. The two
        // Mat4 types are not interchangeable; see the note in Phase11_Plan.md.
        ro.world = rendering::Mat4{};
        ro.world.m[12] = tr->world_x;
        ro.world.m[13] = tr->world_y;
        ro.world.m[14] = tr->world_z;

        ro.mesh_handle = mc->mesh;
        ro.material_handle = mc->material;
        ro.lod = mc->lod;

        ro.bounds = rendering::transform_aabb(mesh->bounds(), tr->world_x, tr->world_y, tr->world_z);
        ro.sphere = rendering::transform_sphere(mesh->bounding_sphere(), tr->world_x, tr->world_y,
                                                tr->world_z);

        out.objects.push_back(std::move(ro));
    }
}

} // namespace nf::runtime
