#include <NF/Scene/HLOD.hpp>

#include <algorithm>

namespace nf::scene {

u32 HLODManager::create_cluster(u32 level, f32 transition_dist, f32 hysteresis) {
    const u32 id = m_next_cluster_id++;
    HLODCluster cluster;
    cluster.cluster_id = id;
    cluster.level = level;
    cluster.transition_distance = std::max(1.0f, transition_dist);
    cluster.hysteresis = std::max(0.0f, hysteresis);
    m_clusters[id] = std::move(cluster);
    return id;
}

bool HLODManager::destroy_cluster(u32 cluster_id) {
    auto it = m_clusters.find(cluster_id);
    if (it != m_clusters.end()) {
        m_clusters.erase(it);
        return true;
    }
    return false;
}

bool HLODManager::add_entity_to_cluster(u32 cluster_id, ecs::Entity entity, const AABB& entity_bounds) {
    auto it = m_clusters.find(cluster_id);
    if (it == m_clusters.end() || !entity.valid()) {
        return false;
    }

    m_entity_bounds[entity] = entity_bounds;
    it->second.entities.push_back(entity);
    recompute_bounds(cluster_id);
    return true;
}

bool HLODManager::remove_entity_from_cluster(u32 cluster_id, ecs::Entity entity) {
    auto it = m_clusters.find(cluster_id);
    if (it == m_clusters.end()) {
        return false;
    }

    auto& ents = it->second.entities;
    auto ent_it = std::remove(ents.begin(), ents.end(), entity);
    if (ent_it != ents.end()) {
        ents.erase(ent_it, ents.end());
        recompute_bounds(cluster_id);
        return true;
    }
    return false;
}

bool HLODManager::add_child_cluster(u32 parent_cluster_id, u32 child_cluster_id) {
    if (parent_cluster_id == child_cluster_id) {
        return false;
    }
    auto pit = m_clusters.find(parent_cluster_id);
    auto cit = m_clusters.find(child_cluster_id);
    if (pit == m_clusters.end() || cit == m_clusters.end()) {
        return false;
    }

    pit->second.child_clusters.push_back(child_cluster_id);
    recompute_bounds(parent_cluster_id);
    return true;
}

bool HLODManager::set_proxy_entity(u32 cluster_id, ecs::Entity proxy_entity) {
    auto it = m_clusters.find(cluster_id);
    if (it == m_clusters.end()) {
        return false;
    }
    it->second.proxy_entity = proxy_entity;
    return true;
}

const HLODCluster* HLODManager::get_cluster(u32 cluster_id) const {
    auto it = m_clusters.find(cluster_id);
    if (it != m_clusters.end()) {
        return &it->second;
    }
    return nullptr;
}

HLODCluster* HLODManager::get_cluster_mut(u32 cluster_id) {
    auto it = m_clusters.find(cluster_id);
    if (it != m_clusters.end()) {
        return &it->second;
    }
    return nullptr;
}

size_t HLODManager::cluster_count_at_level(u32 level) const {
    size_t count = 0;
    for (const auto& pair : m_clusters) {
        if (pair.second.level == level) {
            count++;
        }
    }
    return count;
}

void HLODManager::recompute_bounds(u32 cluster_id) {
    auto it = m_clusters.find(cluster_id);
    if (it == m_clusters.end()) {
        return;
    }

    HLODCluster& cluster = it->second;
    cluster.bounds.reset();

    for (const auto& ent : cluster.entities) {
        auto b_it = m_entity_bounds.find(ent);
        if (b_it != m_entity_bounds.end() && b_it->second.is_valid()) {
            cluster.bounds.expand(b_it->second);
        }
    }

    for (u32 child_id : cluster.child_clusters) {
        auto c_it = m_clusters.find(child_id);
        if (c_it != m_clusters.end() && c_it->second.bounds.is_valid()) {
            cluster.bounds.expand(c_it->second.bounds);
        }
    }

    if (cluster.bounds.is_valid()) {
        cluster.sphere = BoundingSphere::from_aabb(cluster.bounds);
    } else {
        cluster.sphere = BoundingSphere(Vec3(0.0f), 0.0f);
    }
}

void HLODManager::update(const Vec3& camera_position, const Frustum* frustum) {
    m_visible_entities.clear();
    m_visible_proxies.clear();

    for (auto& pair : m_clusters) {
        HLODCluster& cluster = pair.second;
        if (!cluster.bounds.is_valid()) {
            continue;
        }

        // 1. Frustum Culling
        if (frustum) {
            cluster.is_culled = !frustum->intersects_aabb(cluster.bounds);
        } else {
            cluster.is_culled = false;
        }

        if (cluster.is_culled) {
            continue;
        }

        // 2. Distance and Hysteresis
        const f32 dist = (cluster.sphere.center - camera_position).length();
        const f32 half_hyst = cluster.hysteresis * 0.5f;

        if (!cluster.is_proxy_active) {
            if (dist >= cluster.transition_distance + half_hyst) {
                cluster.is_proxy_active = true;
            }
        } else {
            if (dist < cluster.transition_distance - half_hyst) {
                cluster.is_proxy_active = false;
            }
        }

        // 3. Visibility Collection
        if (cluster.is_proxy_active) {
            if (cluster.proxy_entity.valid()) {
                m_visible_proxies.push_back(cluster.proxy_entity);
            }
        } else {
            for (const auto& ent : cluster.entities) {
                m_visible_entities.push_back(ent);
            }
        }
    }
}

std::vector<u32> HLODManager::build_spatial_clusters(
    const std::vector<std::pair<ecs::Entity, AABB>>& entities,
    f32 max_cluster_radius,
    f32 transition_dist,
    f32 hysteresis
) {
    std::vector<u32> created_cluster_ids;

    for (const auto& item : entities) {
        const ecs::Entity ent = item.first;
        const AABB& box = item.second;
        if (!ent.valid() || !box.is_valid()) {
            continue;
        }

        bool assigned = false;
        for (u32 cid : created_cluster_ids) {
            HLODCluster* cluster = get_cluster_mut(cid);
            if (!cluster) continue;

            AABB candidate_bounds = cluster->bounds;
            candidate_bounds.expand(box);
            if (candidate_bounds.radius() <= max_cluster_radius) {
                add_entity_to_cluster(cid, ent, box);
                assigned = true;
                break;
            }
        }

        if (!assigned) {
            const u32 new_cid = create_cluster(1, transition_dist, hysteresis);
            add_entity_to_cluster(new_cid, ent, box);
            created_cluster_ids.push_back(new_cid);
        }
    }

    return created_cluster_ids;
}

void HLODManager::clear() {
    m_clusters.clear();
    m_entity_bounds.clear();
    m_visible_entities.clear();
    m_visible_proxies.clear();
}

} // namespace nf::scene
