#pragma once

// NF/Scene/SpatialOctree.hpp — 3D Spatial Partitioning Octree for Entities
// Accelerates frustum culling, raycast picking, and spatial queries from O(N) to O(log N).

#include <NF/Core/Types.hpp>
#include <NF/Core/Geometry.hpp>
#include <NF/ECS/Entity.hpp>

#include <array>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nf::scene {

struct SpatialItem {
    ecs::Entity entity = ecs::kInvalidEntity;
    AABB        bounds;
};

class SpatialOctree {
public:
    explicit SpatialOctree(const AABB& root_bounds = AABB(Vec3(-256.0f, -64.0f, -256.0f),
                                                          Vec3(256.0f, 256.0f, 256.0f)),
                           u32 max_depth = 6,
                           u32 max_items_per_node = 8);
    ~SpatialOctree();

    void insert(ecs::Entity entity, const AABB& bounds);
    bool remove(ecs::Entity entity);
    void update(ecs::Entity entity, const AABB& new_bounds);
    void clear();

    bool contains(ecs::Entity entity) const {
        return m_entity_to_bounds.find(entity) != m_entity_to_bounds.end();
    }

    size_t size() const { return m_entity_to_bounds.size(); }
    const AABB& root_bounds() const { return m_root_bounds; }

    /// Spatial Queries
    void query_frustum(const Frustum& frustum, std::vector<ecs::Entity>& out_entities) const;
    void query_aabb(const AABB& box, std::vector<ecs::Entity>& out_entities) const;
    void query_ray(const Ray& ray, std::vector<std::pair<ecs::Entity, f32>>& out_hits) const;

private:
    struct Node {
        AABB bounds;
        u32 depth = 0;
        std::vector<SpatialItem> items;
        std::array<std::unique_ptr<Node>, 8> children;

        bool is_leaf() const { return children[0] == nullptr; }
        void subdivide();
    };

    void insert_into_node(Node& node, const SpatialItem& item);
    bool remove_from_node(Node& node, ecs::Entity entity);

    void query_frustum_node(const Node& node, const Frustum& frustum, std::vector<ecs::Entity>& out) const;
    void query_aabb_node(const Node& node, const AABB& box, std::vector<ecs::Entity>& out) const;
    void query_ray_node(const Node& node, const Ray& ray, std::vector<std::pair<ecs::Entity, f32>>& out) const;

    AABB m_root_bounds;
    u32 m_max_depth = 6;
    u32 m_max_items_per_node = 8;
    std::unique_ptr<Node> m_root;
    std::unordered_map<ecs::Entity, AABB> m_entity_to_bounds;
};

} // namespace nf::scene
