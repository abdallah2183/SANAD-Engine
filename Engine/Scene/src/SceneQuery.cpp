#include <NF/Scene/SceneQuery.hpp>

#include <cmath>
#include <algorithm>

namespace nf::scene {

ecs::Entity SceneQuery::find_by_name(const ecs::World& world, const std::string& name) {
    auto entities = world.query<NameComponent>();
    for (ecs::Entity e : entities) {
        if (!world.is_alive(e)) continue;
        const auto* nc = world.get<NameComponent>(e);
        if (nc && nc->name == name) {
            return e;
        }
    }
    return ecs::kInvalidEntity;
}

std::vector<ecs::Entity> SceneQuery::find_all_by_name(const ecs::World& world, const std::string& name) {
    std::vector<ecs::Entity> results;
    auto entities = world.query<NameComponent>();
    for (ecs::Entity e : entities) {
        if (!world.is_alive(e)) continue;
        const auto* nc = world.get<NameComponent>(e);
        if (nc && nc->name == name) {
            results.push_back(e);
        }
    }
    return results;
}

void SceneQuery::populate_octree(
    const ecs::World& world,
    SpatialOctree& octree,
    const Vec3& default_half_extents)
{
    octree.clear();
    auto entities = world.query<Transform>();
    for (ecs::Entity e : entities) {
        if (!world.is_alive(e)) continue;
        const auto* t = world.get<Transform>(e);
        if (!t) continue;

        float sx = std::abs(t->world_scale_x) * default_half_extents.x;
        float sy = std::abs(t->world_scale_y) * default_half_extents.y;
        float sz = std::abs(t->world_scale_z) * default_half_extents.z;

        AABB box{
            Vec3{t->world_x - sx, t->world_y - sy, t->world_z - sz},
            Vec3{t->world_x + sx, t->world_y + sy, t->world_z + sz}
        };

        octree.insert(e, box);
    }
}

std::optional<SceneRaycastHit> SceneQuery::raycast(
    const ecs::World& world,
    const SpatialOctree& octree,
    const Ray& ray,
    float max_distance)
{
    std::vector<std::pair<ecs::Entity, f32>> hits;
    octree.query_ray(ray, hits);
    if (hits.empty()) return std::nullopt;

    float min_dist = max_distance;
    ecs::Entity best_entity = ecs::kInvalidEntity;
    for (const auto& [e, dist] : hits) {
        if (dist <= min_dist && world.is_alive(e)) {
            min_dist = dist;
            best_entity = e;
        }
    }
    if (!best_entity.valid()) return std::nullopt;

    SceneRaycastHit hit;
    hit.entity = best_entity;
    hit.distance = min_dist;
    hit.point = Vec3{
        ray.origin.x + ray.direction.x * min_dist,
        ray.origin.y + ray.direction.y * min_dist,
        ray.origin.z + ray.direction.z * min_dist
    };
    hit.normal = Vec3{-ray.direction.x, -ray.direction.y, -ray.direction.z};
    return hit;
}

void SceneQuery::query_frustum(
    const ecs::World& world,
    const SpatialOctree& octree,
    const Frustum& frustum,
    std::vector<ecs::Entity>& out_visible)
{
    out_visible.clear();
    std::vector<ecs::Entity> raw_entities;
    octree.query_frustum(frustum, raw_entities);

    out_visible.reserve(raw_entities.size());
    for (ecs::Entity e : raw_entities) {
        if (world.is_alive(e)) {
            out_visible.push_back(e);
        }
    }
}

void SceneQuery::query_sphere(
    const ecs::World& world,
    const SpatialOctree& octree,
    const BoundingSphere& sphere,
    std::vector<ecs::Entity>& out_entities)
{
    out_entities.clear();
    AABB sphere_box{
        Vec3{sphere.center.x - sphere.radius, sphere.center.y - sphere.radius, sphere.center.z - sphere.radius},
        Vec3{sphere.center.x + sphere.radius, sphere.center.y + sphere.radius, sphere.center.z + sphere.radius}
    };

    std::vector<ecs::Entity> candidates;
    octree.query_aabb(sphere_box, candidates);

    out_entities.reserve(candidates.size());
    for (ecs::Entity e : candidates) {
        if (!world.is_alive(e)) continue;
        const auto* t = world.get<Transform>(e);
        if (t) {
            float dx = t->world_x - sphere.center.x;
            float dy = t->world_y - sphere.center.y;
            float dz = t->world_z - sphere.center.z;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            if (dist_sq <= (sphere.radius * sphere.radius)) {
                out_entities.push_back(e);
                continue;
            }
        }
        // If entity doesn't have transform or passed box query
        out_entities.push_back(e);
    }
}

void SceneQuery::query_aabb(
    const ecs::World& world,
    const SpatialOctree& octree,
    const AABB& aabb,
    std::vector<ecs::Entity>& out_entities)
{
    out_entities.clear();
    std::vector<ecs::Entity> candidates;
    octree.query_aabb(aabb, candidates);

    out_entities.reserve(candidates.size());
    for (ecs::Entity e : candidates) {
        if (world.is_alive(e)) {
            out_entities.push_back(e);
        }
    }
}

} // namespace nf::scene
