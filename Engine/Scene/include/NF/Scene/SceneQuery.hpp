#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Geometry.hpp>
#include <NF/ECS/Entity.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/SpatialOctree.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Scene/NameComponent.hpp>

#include <optional>
#include <vector>
#include <string>

namespace nf::scene {

struct SceneRaycastHit {
    ecs::Entity entity{ecs::kInvalidEntity};
    float distance = 1e30f;
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

class SceneQuery {
public:
    /// Searches for the first active entity whose NameComponent::name equals the query.
    static ecs::Entity find_by_name(const ecs::World& world, const std::string& name);

    /// Collects all active entities whose NameComponent::name equals the query.
    static std::vector<ecs::Entity> find_all_by_name(const ecs::World& world, const std::string& name);

    /// Populates or updates an octree using all active entities that have a Transform component.
    /// Uses default_half_extents if no specific bounds are provided.
    static void populate_octree(
        const ecs::World& world,
        SpatialOctree& octree,
        const Vec3& default_half_extents = Vec3{0.5f, 0.5f, 0.5f}
    );

    /// Performs an accelerated spatial raycast against the scene octree.
    static std::optional<SceneRaycastHit> raycast(
        const ecs::World& world,
        const SpatialOctree& octree,
        const Ray& ray,
        float max_distance = 10000.0f
    );

    /// Queries all entities in the octree whose bounds intersect the camera frustum.
    static void query_frustum(
        const ecs::World& world,
        const SpatialOctree& octree,
        const Frustum& frustum,
        std::vector<ecs::Entity>& out_visible
    );

    /// Queries all entities within a spherical area.
    static void query_sphere(
        const ecs::World& world,
        const SpatialOctree& octree,
        const BoundingSphere& sphere,
        std::vector<ecs::Entity>& out_entities
    );

    /// Queries all entities within an axis-aligned bounding box.
    static void query_aabb(
        const ecs::World& world,
        const SpatialOctree& octree,
        const AABB& aabb,
        std::vector<ecs::Entity>& out_entities
    );
};

} // namespace nf::scene
