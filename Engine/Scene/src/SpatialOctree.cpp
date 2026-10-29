#include <NF/Scene/SpatialOctree.hpp>

#include <algorithm>

namespace nf::scene {

void SpatialOctree::Node::subdivide() {
    const Vec3 c = bounds.center();
    const Vec3 mn = bounds.min;
    const Vec3 mx = bounds.max;

    // 8 Octants
    children[0] = std::make_unique<Node>(Node{AABB(Vec3(mn.x, mn.y, mn.z), Vec3(c.x, c.y, c.z)), depth + 1, {}, {}});
    children[1] = std::make_unique<Node>(Node{AABB(Vec3(c.x, mn.y, mn.z), Vec3(mx.x, c.y, c.z)), depth + 1, {}, {}});
    children[2] = std::make_unique<Node>(Node{AABB(Vec3(mn.x, c.y, mn.z), Vec3(c.x, mx.y, c.z)), depth + 1, {}, {}});
    children[3] = std::make_unique<Node>(Node{AABB(Vec3(c.x, c.y, mn.z), Vec3(mx.x, mx.y, c.z)), depth + 1, {}, {}});
    children[4] = std::make_unique<Node>(Node{AABB(Vec3(mn.x, mn.y, c.z), Vec3(c.x, c.y, mx.z)), depth + 1, {}, {}});
    children[5] = std::make_unique<Node>(Node{AABB(Vec3(c.x, mn.y, c.z), Vec3(mx.x, c.y, mx.z)), depth + 1, {}, {}});
    children[6] = std::make_unique<Node>(Node{AABB(Vec3(mn.x, c.y, c.z), Vec3(c.x, mx.y, mx.z)), depth + 1, {}, {}});
    children[7] = std::make_unique<Node>(Node{AABB(Vec3(c.x, c.y, c.z), Vec3(mx.x, mx.y, mx.z)), depth + 1, {}, {}});
}

SpatialOctree::SpatialOctree(const AABB& root_bounds, u32 max_depth, u32 max_items_per_node)
    : m_root_bounds(root_bounds), m_max_depth(max_depth), m_max_items_per_node(max_items_per_node) {
    m_root = std::make_unique<Node>();
    m_root->bounds = root_bounds;
    m_root->depth = 0;
}

SpatialOctree::~SpatialOctree() = default;

void SpatialOctree::insert(ecs::Entity entity, const AABB& bounds) {
    remove(entity);
    SpatialItem item{entity, bounds};
    m_entity_to_bounds[entity] = bounds;
    insert_into_node(*m_root, item);
}

void SpatialOctree::insert_into_node(Node& node, const SpatialItem& item) {
    if (node.is_leaf()) {
        if (node.items.size() < m_max_items_per_node || node.depth >= m_max_depth) {
            node.items.push_back(item);
            return;
        }

        node.subdivide();

        // Re-distribute existing items in leaf
        std::vector<SpatialItem> old_items = std::move(node.items);
        node.items.clear();
        for (const auto& old_item : old_items) {
            insert_into_node(node, old_item);
        }
    }

    // Attempt pushing down into children
    for (int i = 0; i < 8; ++i) {
        if (node.children[i]->bounds.contains(item.bounds)) {
            insert_into_node(*node.children[i], item);
            return;
        }
    }

    // If item spans across multiple children boundaries, keep it in this node
    node.items.push_back(item);
}

bool SpatialOctree::remove(ecs::Entity entity) {
    auto it = m_entity_to_bounds.find(entity);
    if (it == m_entity_to_bounds.end()) {
        return false;
    }
    m_entity_to_bounds.erase(it);
    return remove_from_node(*m_root, entity);
}

bool SpatialOctree::remove_from_node(Node& node, ecs::Entity entity) {
    for (auto it = node.items.begin(); it != node.items.end(); ++it) {
        if (it->entity == entity) {
            node.items.erase(it);
            return true;
        }
    }

    if (!node.is_leaf()) {
        for (int i = 0; i < 8; ++i) {
            if (remove_from_node(*node.children[i], entity)) {
                return true;
            }
        }
    }
    return false;
}

void SpatialOctree::update(ecs::Entity entity, const AABB& new_bounds) {
    insert(entity, new_bounds);
}

void SpatialOctree::clear() {
    m_entity_to_bounds.clear();
    m_root = std::make_unique<Node>();
    m_root->bounds = m_root_bounds;
    m_root->depth = 0;
}

void SpatialOctree::query_frustum(const Frustum& frustum, std::vector<ecs::Entity>& out_entities) const {
    query_frustum_node(*m_root, frustum, out_entities);
}

void SpatialOctree::query_frustum_node(const Node& node, const Frustum& frustum, std::vector<ecs::Entity>& out) const {
    if (!frustum.intersects_aabb(node.bounds)) {
        return; // Culled entirely
    }

    // Test items stored directly in this node
    for (const auto& item : node.items) {
        if (frustum.intersects_aabb(item.bounds)) {
            out.push_back(item.entity);
        }
    }

    if (!node.is_leaf()) {
        for (int i = 0; i < 8; ++i) {
            query_frustum_node(*node.children[i], frustum, out);
        }
    }
}

void SpatialOctree::query_aabb(const AABB& box, std::vector<ecs::Entity>& out_entities) const {
    query_aabb_node(*m_root, box, out_entities);
}

void SpatialOctree::query_aabb_node(const Node& node, const AABB& box, std::vector<ecs::Entity>& out) const {
    if (!box.intersects(node.bounds)) {
        return;
    }

    for (const auto& item : node.items) {
        if (box.intersects(item.bounds)) {
            out.push_back(item.entity);
        }
    }

    if (!node.is_leaf()) {
        for (int i = 0; i < 8; ++i) {
            query_aabb_node(*node.children[i], box, out);
        }
    }
}

void SpatialOctree::query_ray(const Ray& ray, std::vector<std::pair<ecs::Entity, f32>>& out_hits) const {
    query_ray_node(*m_root, ray, out_hits);
    // Sort hits from closest to farthest
    std::sort(out_hits.begin(), out_hits.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    });
}

void SpatialOctree::query_ray_node(const Node& node, const Ray& ray, std::vector<std::pair<ecs::Entity, f32>>& out) const {
    f32 node_t = 0.0f;
    if (!ray.intersects(node.bounds, node_t)) {
        return;
    }

    for (const auto& item : node.items) {
        f32 hit_t = 0.0f;
        if (ray.intersects(item.bounds, hit_t)) {
            out.push_back({item.entity, hit_t});
        }
    }

    if (!node.is_leaf()) {
        for (int i = 0; i < 8; ++i) {
            query_ray_node(*node.children[i], ray, out);
        }
    }
}

} // namespace nf::scene
