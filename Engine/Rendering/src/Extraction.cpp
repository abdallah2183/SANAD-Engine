// NF/Rendering/src/Extraction.cpp — game::GameWorld → RenderWorld extraction
//
// The ECS bridge that used to live here moved to NF/Runtime/SceneExtraction.cpp
// in Phase 11 (W2). It needed `<NF/ECS/ECS.hpp>` and `<NF/Scene/Transform.hpp>`,
// which made the renderer depend on the game world it draws.

#include <NF/Rendering/Extraction.hpp>

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

} // namespace nf::rendering
