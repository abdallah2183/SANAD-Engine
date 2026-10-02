// NF/Runtime/SceneExtraction.cpp — the game-world → render-world bridge (Phase 11, W2)

#include <NF/Runtime/SceneExtraction.hpp>

#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <vector>

namespace nf::runtime {

// The scene's spot range and the renderer's default far plane are the same
// number living in two headers, which is the exact shape that went wrong once
// already (see `SpotLight::range`: a 25.0 hard-coded in the shader and another on
// the CPU, tied together by nothing). Pinned here, so changing either without the
// other is a compile error rather than a spot light that shadows to a different
// distance than it lights.
static_assert(SpotLightComponent{}.range == rendering::kLocalShadowSpotDefaultFar,
              "scene spot range must equal the renderer's default far plane");

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

        // The FULL world matrix: position, composed rotation, composed scale.
        //
        // This used to be Mat4::translate(position) with a comment saying
        // "rotation/scale do not accumulate through the hierarchy yet", which
        // was true of the PROPAGATION and was used as an excuse for the
        // RENDERING. The visible result was that a rotated object drew
        // unrotated and a scaled object drew at 1x1: the Inspector showed a
        // scale of 40 and the viewport showed a 1 m cube, and no amount of
        // dragging the gizmo changed the picture. world_matrix() is the single
        // place a Transform becomes a matrix, so extraction, bounds, picking
        // and the gizmo cannot disagree about an object's pose.
        ro.world = scene::world_matrix(*tr);

        ro.mesh_handle = mc->mesh;
        ro.material_handle = mc->material;
        ro.lod = mc->lod;

        // Bounds through the same matrix the draw uses. Translating only meant
        // culling and CPU picking tested the object's UNROTATED box, so a
        // scaled ground plane was culled while filling the screen.
        ro.bounds = rendering::transform_aabb(mesh->bounds(), ro.world);
        ro.sphere = rendering::transform_sphere(mesh->bounding_sphere(), ro.world);

        out.objects.push_back(std::move(ro));
    }
}

LocalLights collect_local_lights(const ecs::World& world) {
    LocalLights out;

    // Entity order, not storage order — see the LocalLights comment.
    auto by_id = [](ecs::Entity a, ecs::Entity b) { return a.id < b.id; };

    std::vector<ecs::Entity> lights = world.query<PointLightComponent>();
    std::sort(lights.begin(), lights.end(), by_id);
    for (ecs::Entity e : lights) {
        const PointLightComponent* c = world.get<PointLightComponent>(e);
        if (c == nullptr || !c->enabled) {
            continue;
        }
        if (out.points.size() >= rendering::Renderer3D::kMaxPointLights) {
            ++out.dropped_points;
            continue;
        }
        rendering::PointLight pl{};
        // An entity with no Transform sits at the origin, which is where its
        // mesh would be drawn too — consistent, and never an uninitialised
        // position read from a component that was never written.
        if (const scene::Transform* tr = world.get<scene::Transform>(e)) {
            pl.position = {tr->world_x, tr->world_y, tr->world_z};
        }
        pl.color = {c->color_r, c->color_g, c->color_b};
        pl.intensity = c->intensity;
        pl.radius = c->radius;
        // No `enabled` on the render struct: disabled lights were skipped
        // above and never uploaded (see LocalLights), so there is nothing to
        // flag downstream — the renderer never reads it.
        pl.shadows_enabled = c->cast_shadows;
        pl.shadow_strength = c->shadow_strength;
        pl.shadow_bias = c->shadow_bias;
        // 0 means "the radius", and that default is the renderer's own — left at
        // zero here rather than filled in, so the one rule about what 0 means
        // lives in the renderer and not in two places.
        pl.shadow_distance = c->shadow_distance;
        out.points.push_back(pl);
    }

    lights = world.query<SpotLightComponent>();
    std::sort(lights.begin(), lights.end(), by_id);
    for (ecs::Entity e : lights) {
        const SpotLightComponent* c = world.get<SpotLightComponent>(e);
        if (c == nullptr || !c->enabled) {
            continue;
        }
        if (out.spots.size() >= rendering::Renderer3D::kMaxSpotLights) {
            ++out.dropped_spots;
            continue;
        }
        rendering::SpotLight sl{};
        if (const scene::Transform* tr = world.get<scene::Transform>(e)) {
            sl.position = {tr->world_x, tr->world_y, tr->world_z};
        }
        sl.direction = {c->dir_x, c->dir_y, c->dir_z};
        sl.color = {c->color_r, c->color_g, c->color_b};
        sl.intensity = c->intensity;
        sl.inner_angle_rad = c->inner_angle_rad;
        sl.outer_angle_rad = c->outer_angle_rad;
        sl.range = c->range;
        // No `enabled` on the render struct (see the point-light note above).
        sl.shadows_enabled = c->cast_shadows;
        sl.shadow_strength = c->shadow_strength;
        sl.shadow_bias = c->shadow_bias;
        sl.shadow_distance = c->shadow_distance;
        out.spots.push_back(sl);
    }

    return out;
}

} // namespace nf::runtime
