#pragma once

#include <NF/Core/Geometry.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace nf::scene {

/// An HLOD cluster representing a collection of individual entities or sub-clusters
/// that are merged into a single proxy mesh when viewed from a distance.
struct HLODCluster {
    u32 cluster_id = 0;
    u32 level = 1;                     ///< 1 = basic actor cluster, 2 = cluster of clusters, etc.
    AABB bounds;                       ///< Combined tight AABB enclosing all members
    BoundingSphere sphere;             ///< Center and radius bounding the cluster
    std::vector<ecs::Entity> entities; ///< Constituent entities in this cluster
    std::vector<u32> child_clusters;   ///< Constituent child clusters (for level > 1)
    ecs::Entity proxy_entity = ecs::kInvalidEntity; ///< The single merged mesh entity rendered at a distance

    f32 transition_distance = 120.0f;  ///< Distance beyond which proxy is shown and entities are hidden
    f32 hysteresis = 12.0f;            ///< Hysteresis buffer (avoids popping on boundary)
    bool is_proxy_active = false;      ///< Current active state (true = proxy visible, false = entities visible)
    bool is_culled = false;            ///< Culled by view frustum
};

/// High-level HLOD Manager governing spatial clustering, LOD transitions, and culling
class HLODManager {
public:
    HLODManager() = default;

    /// Create a new empty cluster. Returns cluster ID.
    u32 create_cluster(u32 level = 1, f32 transition_dist = 120.0f, f32 hysteresis = 12.0f);

    /// Destroy an existing cluster by ID
    bool destroy_cluster(u32 cluster_id);

    /// Add an entity and its bounding box to a cluster
    bool add_entity_to_cluster(u32 cluster_id, ecs::Entity entity, const AABB& entity_bounds);

    /// Remove an entity from a cluster
    bool remove_entity_from_cluster(u32 cluster_id, ecs::Entity entity);

    /// Nest a child cluster under a parent cluster (hierarchical multi-level HLOD)
    bool add_child_cluster(u32 parent_cluster_id, u32 child_cluster_id);

    /// Set the representative proxy mesh entity for this cluster
    bool set_proxy_entity(u32 cluster_id, ecs::Entity proxy_entity);

    /// Find cluster by ID
    [[nodiscard]] const HLODCluster* get_cluster(u32 cluster_id) const;

    /// Mutable lookup for internal updates
    [[nodiscard]] HLODCluster* get_cluster_mut(u32 cluster_id);

    /// Total number of clusters
    [[nodiscard]] size_t cluster_count() const noexcept { return m_clusters.size(); }

    /// Number of clusters at a specific hierarchy level
    [[nodiscard]] size_t cluster_count_at_level(u32 level) const;

    /// Recompute cluster bounds and bounding sphere from its entities and child clusters
    void recompute_bounds(u32 cluster_id);

    /// Update cluster visibility, LOD transitions with hysteresis, and optional frustum culling
    void update(const Vec3& camera_position, const Frustum* frustum = nullptr);

    /// Get entities that should be drawn in the current frame (detailed actors)
    [[nodiscard]] const std::vector<ecs::Entity>& visible_entities() const noexcept { return m_visible_entities; }

    /// Get proxy entities that should be drawn in the current frame (simplified HLOD proxies)
    [[nodiscard]] const std::vector<ecs::Entity>& visible_proxies() const noexcept { return m_visible_proxies; }

    /// Active proxy count
    [[nodiscard]] size_t active_proxy_count() const noexcept { return m_visible_proxies.size(); }

    /// Automatically cluster a set of entities based on spatial proximity
    std::vector<u32> build_spatial_clusters(
        const std::vector<std::pair<ecs::Entity, AABB>>& entities,
        f32 max_cluster_radius = 50.0f,
        f32 transition_dist = 120.0f,
        f32 hysteresis = 12.0f
    );

    /// Clear all clusters and visibility state
    void clear();

private:
    std::unordered_map<u32, HLODCluster> m_clusters;
    std::unordered_map<ecs::Entity, AABB> m_entity_bounds;
    std::vector<ecs::Entity> m_visible_entities;
    std::vector<ecs::Entity> m_visible_proxies;
    u32 m_next_cluster_id = 1;
};

} // namespace nf::scene
