#include <NF/Rendering/Extraction.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>

namespace nf::rendering {

void extract_render_world(const game::GameWorld& game_world, RenderWorld& render_world) {
    render_world.clear();
    render_world.objects.reserve(game_world.entities.size());

    for (const auto& e : game_world.entities) {
        // Only entities with mesh+material and visible are rendered
        if (!e.visible) continue;
        if (!e.mesh.has_value() || !e.material.has_value()) continue;

        RenderObject ro;
        ro.id = e.id;
        ro.transform.x = e.transform.x;
        ro.transform.y = e.transform.y;
        ro.transform.z = e.transform.z;
        ro.mesh = e.mesh->mesh_id;
        ro.material = e.material->material_name;
        ro.visible = e.visible;
        render_world.objects.push_back(std::move(ro));
    }
}

void extract_render_world_double_buffered(const game::GameWorld& game_world, RenderWorldBuffer& buffer) {
    // Write to back, then swap so front becomes the newly extracted world
    RenderWorld& back = buffer.back();
    extract_render_world(game_world, back);
    buffer.swap();
}

void extract_render_objects(const ecs::World& world, const MeshLibrary& meshes, RenderWorld& out) {
    out.clear();

    for (ecs::Entity e : world.query<scene::Transform, MeshComponent>()) {
        const scene::Transform* tr = world.get<scene::Transform>(e);
        const MeshComponent* mc = world.get<MeshComponent>(e);
        if (!tr || !mc || !mc->visible) continue;
        if (!mc->mesh.valid()) continue;
        const StaticMesh* mesh = meshes.get(mc->mesh);
        if (!mesh) continue;

        RenderObject ro;
        ro.id = e.id;
        ro.visible = true;

        // World transform: translation only for this milestone (matches the
        // scene::Transform model). Full TRS matrices land with the transform
        // upgrade; the RenderObject::world field already carries a Mat4 so
        // nothing downstream changes shape when that happens.
        ro.transform.x = tr->world_x;
        ro.transform.y = tr->world_y;
        ro.transform.z = tr->world_z;
        ro.world = Mat4::identity();
        ro.world.m[12] = tr->world_x;
        ro.world.m[13] = tr->world_y;
        ro.world.m[14] = tr->world_z;

        ro.mesh_handle = mc->mesh;
        ro.material_handle = mc->material;
        ro.lod = mc->lod;

        // World-space bounds from the mesh's local bounds
        ro.bounds = transform_aabb(mesh->bounds(), tr->world_x, tr->world_y, tr->world_z);
        ro.sphere = transform_sphere(mesh->bounding_sphere(), tr->world_x, tr->world_y, tr->world_z);

        out.objects.push_back(std::move(ro));
    }
}

} // namespace nf::rendering
